//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Getroot
//  Copyright (c) 2023 AirenSoft. All rights reserved.
//
//==============================================================================
#include <modules/address/address_utilities.h>

#include "whip_server.h"
#include "whip_interceptor.h"
#include "whip_private.h"

WhipServer::WhipServer(const cfg::bind::cmm::Webrtc &webrtc_bind_cfg)
	: _webrtc_bind_cfg(webrtc_bind_cfg)
{

}

bool WhipServer::Start(const std::shared_ptr<WhipObserver> &observer, const ov::SocketAddress *address, const ov::SocketAddress *tls_address, int worker_count)
{
	if ((_http_server != nullptr) || (_https_server != nullptr))
	{
		OV_ASSERT(false, "Server is already running (%p, %p)", _http_server.get(), _https_server.get());
		return false;
	}

	_observer = observer;
	
	bool result = true;
	auto manager = http::svr::HttpServerManager::GetInstance();
	std::shared_ptr<http::svr::HttpServer> http_server = nullptr;
	std::shared_ptr<http::svr::HttpsServer> https_server = nullptr;
	if (address != nullptr)
	{
		_http_server_address = *address;
		http_server = manager->CreateHttpServer("RtcSig", *address, worker_count);
	}

	if (tls_address != nullptr)
	{
		_https_server_address = *tls_address;
		https_server = manager->CreateHttpsServer("RtcSig", *tls_address, false, worker_count);
	}

	auto interceptor = CreateInterceptor();
	if (interceptor == nullptr)
	{
		logte("Could not create interceptor");
		return false;
	}

	result = result && ((http_server == nullptr) || http_server->AddInterceptor(interceptor));
	result = result && ((https_server == nullptr) || https_server->AddInterceptor(interceptor));

	if (result)
	{
		// for internal turn/tcp relay configuration
		_tcp_force = _webrtc_bind_cfg.GetIceCandidates().IsTcpForce();

		bool tcp_relay_parsed = false;
		auto tcp_relay_address = _webrtc_bind_cfg.GetIceCandidates().GetTcpRelay(&tcp_relay_parsed);
		if (tcp_relay_parsed)
		{
			// <TcpRelay>IP:Port</TcpRelay>
			// <TcpRelay>*:Port</TcpRelay>
			// <TcpRelay>${PublicIP}:Port</TcpRelay>
			
			auto address_items = tcp_relay_address.Split(":");
			if (address_items.size() != 2)
			{
				logte("Invalid TCP relay address(%s). The TCP relay address format must be IP:Port ", tcp_relay_address.CStr());
			}
			else if (address_items[0] == "*")
			{
				auto ip_list = ov::AddressUtilities::GetInstance()->GetIpList();
				for (const auto &ip : ip_list)
				{
					auto address = ov::String::FormatString("turn:%s:%s?transport=tcp", ip.CStr(), address_items[1].CStr()).CStr();
					_link_headers.push_back(GetIceServerLinkValue(address, DEFAULT_RELAY_USERNAME, DEFAULT_RELAY_KEY));
				}
			}
			else if (address_items[0] == "${PublicIP}")
			{
				auto public_ip = ov::AddressUtilities::GetInstance()->GetMappedAddress();
				auto address = ov::String::FormatString("turn:%s:%s?transport=tcp", public_ip->GetIpAddress().CStr(), address_items[1].CStr());
				_link_headers.push_back(GetIceServerLinkValue(address, DEFAULT_RELAY_USERNAME, DEFAULT_RELAY_KEY));
			}
			else
			{
				auto address = ov::String::FormatString("turn:%s?transport=tcp", tcp_relay_address.CStr());
				_link_headers.push_back(GetIceServerLinkValue(address, DEFAULT_RELAY_USERNAME, DEFAULT_RELAY_KEY));
			}
		}

		// for external ice server configuration
		auto &ice_servers_config = _webrtc_bind_cfg.GetIceServers();
		if (ice_servers_config.IsParsed())
		{
			for (auto ice_server_config : ice_servers_config.GetIceServerList())
			{
				ov::String username, credential;

				// UserName
				if (ice_server_config.GetUserName().IsEmpty() == false)
				{
					// "user_name" is out of specification. This is a bug and "username" is correct. "user_name" will be deprecated in the future.
					username = ice_server_config.GetUserName();
				}

				// Credential
				if (ice_server_config.GetCredential().IsEmpty() == false)
				{
					credential = ice_server_config.GetCredential();
				}

				// URLS
				auto &url_list = ice_server_config.GetUrls().GetUrlList();
				if (url_list.size() == 0)
				{
					logtw("There is no URL list in ICE Servers");
					continue;
				}

				for (auto url : url_list)
				{
					auto address = ov::String::FormatString("turn:%s?transport=tcp", url.CStr());
					_link_headers.push_back(GetIceServerLinkValue(address, username, credential));
				}
			}
		}

		_http_server = http_server;
		_https_server = https_server;
	}
	else
	{
		// Rollback
		manager->ReleaseServer(http_server);
		manager->ReleaseServer(https_server);
	}

	return result;
}

bool WhipServer::Stop()
{

	return true;
}

bool WhipServer::AppendCertificate(const std::shared_ptr<const info::Certificate> &certificate)
{
	if (_https_server != nullptr && certificate != nullptr)
	{
		return _https_server->AppendCertificate(certificate) == nullptr;
	}

	return true;
}

bool WhipServer::RemoveCertificate(const std::shared_ptr<const info::Certificate> &certificate)
{
	if (_https_server != nullptr && certificate != nullptr)
	{
		return _https_server->RemoveCertificate(certificate) == nullptr;
	}

	return true;
}

void WhipServer::SetCors(const info::VHostAppName &vhost_app_name, const std::vector<ov::String> &url_list)
{
	_cors_manager.SetCrossDomains(vhost_app_name, url_list);
}

void WhipServer::EraseCors(const info::VHostAppName &vhost_app_name)
{
	_cors_manager.SetCrossDomains(vhost_app_name, {});
}

ov::String WhipServer::GetIceServerLinkValue(const ov::String &URL, const ov::String &username, const ov::String &credential)
{
	// <turn:turn.example.net?transport=tcp>; rel="ice-server"; username="user"; credential="myPassword"; credential-type="password"
	return ov::String::FormatString("<%s>; rel=\"ice-server\"; username=\"%s\"; credential=\"%s\"; credential-type=\"password\"", URL.CStr(), username.CStr(), credential.CStr());
}

std::shared_ptr<WhipInterceptor> WhipServer::CreateInterceptor()
{
	auto interceptor = std::make_shared<WhipInterceptor>();
	
	// OPTION
	interceptor->Register(http::Method::Options, R"([\s\S]*)", [this](const std::shared_ptr<http::svr::HttpExchange> &exchange) -> http::svr::NextHandler 
	{
		auto connection = exchange->GetConnection();
		auto request = exchange->GetRequest();
		auto response = exchange->GetResponse();

		logti("WHIP Requested: %s", request->ToString().CStr());

		auto request_url = request->GetParsedUri();
		if (request_url == nullptr)
		{
			logte("Could not parse request url: %s", request->GetUri().CStr());
			response->SetStatusCode(http::StatusCode::BadRequest);
			return http::svr::NextHandler::DoNotCall;
		}

		auto vhost_app_name = ocst::Orchestrator::GetInstance()->ResolveApplicationNameFromDomain(request_url->Host(), request_url->App());
		if (vhost_app_name.IsValid() == false)
		{
			logte("Could not resolve application name from domain: %s", request_url->Host().CStr());
			response->SetStatusCode(http::StatusCode::NotFound);
			return http::svr::NextHandler::DoNotCall;
		}

		response->SetStatusCode(http::StatusCode::OK);
		response->SetHeader("Access-Control-Allow-Methods", "POST, DELETE, PATCH, OPTIONS");
		response->SetHeader("Access-Control-Allow-Private-Network", "true");

		_cors_manager.SetupHttpCorsHeader(vhost_app_name, request, response);

		return http::svr::NextHandler::DoNotCall;
	});

	// POST
	interceptor->Register(http::Method::Post, R"([\s\S]*)", [this](const std::shared_ptr<http::svr::HttpExchange> &exchange) -> http::svr::NextHandler 
	{
		auto connection = exchange->GetConnection();
		auto request = exchange->GetRequest();
		auto response = exchange->GetResponse();

		logti("WHIP Requested: %s", request->ToString().CStr());

		if (_observer == nullptr)
		{
			logte("Internal Server Error - Observer is not set");
			response->SetStatusCode(http::StatusCode::InternalServerError);
			return http::svr::NextHandler::DoNotCall;
		}

		// Check if Content-Type is application/sdp
		auto content_type = request->GetHeader("Content-Type");
		if (content_type.IsEmpty() || content_type != "application/sdp")
		{
			logtw("Content-Type is not application/sdp: %s", content_type.CStr());
		}

		auto request_url = request->GetParsedUri();
		if (request_url == nullptr)
		{
			logte("Could not parse request url: %s", request->GetUri().CStr());
			response->SetStatusCode(http::StatusCode::BadRequest);
			return http::svr::NextHandler::DoNotCall;
		}

		auto vhost_app_name = ocst::Orchestrator::GetInstance()->ResolveApplicationNameFromDomain(request_url->Host(), request_url->App());
		if (vhost_app_name.IsValid() == false)
		{
			logte("Could not resolve application name from domain: %s", request_url->Host().CStr());
			response->SetStatusCode(http::StatusCode::NotFound);
			return http::svr::NextHandler::DoNotCall;
		}

		auto stream_name = request_url->Stream();

		// Set CORS header in response
		_cors_manager.SetupHttpCorsHeader(vhost_app_name, request, response);

		auto data = request->GetRequestBody();
		if (data == nullptr)
		{
			logte("Could not get request body");
			response->SetStatusCode(http::StatusCode::BadRequest);
			return http::svr::NextHandler::DoNotCall;
		}

		logti("WHIP SDP Offer: %s", data->ToString().CStr());

		auto offer_sdp = std::make_shared<SessionDescription>();
		if (offer_sdp->FromString(data->ToString()) == false)
		{
			logte("Could not parse SDP: %s", data->ToString().CStr());
			response->SetStatusCode(http::StatusCode::BadRequest);
			return http::svr::NextHandler::DoNotCall;
		}

		auto answer = _observer->OnSdpOffer(request, offer_sdp);
		response->SetStatusCode(answer._status_code);

		if (answer._status_code == http::StatusCode::Created)
		{
			// Set SDP
			response->SetHeader("Content-Type", "application/sdp");
			response->SetHeader("ETag", answer._entity_tag);
			response->SetHeader("Location", ov::String::FormatString("/%s/%s/%s", request_url->App().CStr(), request_url->Stream().CStr(), answer._session_id.CStr()));

			// Add ICE Server Link
			for (const auto &ice_server : _link_headers)
			{
				// Multiple Link headers are allowed
				response->AddHeader("Link", ice_server);
			}

			response->AppendString(answer._sdp->ToString());
		}
		else
		{
			// Set Error
			if (answer._error_message.IsEmpty() == false)
			{
				response->SetHeader("Content-Type", "text/plain");
				response->AppendString(answer._error_message);
			}
		}

		logtd("WHIP Response: %s", response->ToString().CStr());

		return http::svr::NextHandler::DoNotCall;
	});

	// DELETE
	interceptor->Register(http::Method::Delete, R"([\s\S]*)", [this](const std::shared_ptr<http::svr::HttpExchange> &exchange) -> http::svr::NextHandler 
	{
		auto connection = exchange->GetConnection();
		auto request = exchange->GetRequest();
		auto response = exchange->GetResponse();

		logti("WHIP Requested: %s", request->ToString().CStr());

		if (_observer == nullptr)
		{
			logte("Internal Server Error - Observer is not set");
			response->SetStatusCode(http::StatusCode::InternalServerError);
			return http::svr::NextHandler::DoNotCall;
		}

		auto request_url = request->GetParsedUri();
		if (request_url == nullptr)
		{
			logte("Could not parse request url: %s", request->GetUri().CStr());
			response->SetStatusCode(http::StatusCode::BadRequest);
			return http::svr::NextHandler::DoNotCall;
		}

		auto vhost_app_name = ocst::Orchestrator::GetInstance()->ResolveApplicationNameFromDomain(request_url->Host(), request_url->App());
		if (vhost_app_name.IsValid() == false)
		{
			logte("Could not resolve application name from domain: %s", request_url->Host().CStr());
			response->SetStatusCode(http::StatusCode::NotFound);
			return http::svr::NextHandler::DoNotCall;
		}

		auto stream_name = request_url->Stream();

		// Set CORS header in response
		_cors_manager.SetupHttpCorsHeader(vhost_app_name, request, response);

		auto session_key = request_url->File();
		if (session_key.IsEmpty())
		{
			logte("Could not get session key from url: %s", request_url->ToUrlString(true).CStr());
			response->SetStatusCode(http::StatusCode::BadRequest);
			return http::svr::NextHandler::DoNotCall;
		}

		if (_observer->OnSessionDelete(request, session_key) == true)
		{
			response->SetStatusCode(http::StatusCode::OK);
		}
		else
		{
			response->SetStatusCode(http::StatusCode::NotFound);
		}

		return http::svr::NextHandler::DoNotCall;
	});
		
	// PATCH
	interceptor->Register(http::Method::Patch, R"([\s\S]*)", [this](const std::shared_ptr<http::svr::HttpExchange> &exchange) -> http::svr::NextHandler 
	{
		auto connection = exchange->GetConnection();
		auto request = exchange->GetRequest();
		auto response = exchange->GetResponse();

		logti("WHIP Requested: %s", request->ToString().CStr());

		if (_observer == nullptr)
		{
			logte("Internal Server Error - Observer is not set");
			response->SetStatusCode(http::StatusCode::InternalServerError);
			return http::svr::NextHandler::DoNotCall;
		}

		// Check if Content-Type is application/trickle-ice-sdpfrag
		auto content_type = request->GetHeader("Content-Type");
		if (content_type.IsEmpty() || content_type != "application/trickle-ice-sdpfrag")
		{
			logte("Content-Type is not application/trickle-ice-sdpfrag");
		}

		auto request_url = request->GetParsedUri();
		if (request_url == nullptr)
		{
			logte("Could not parse request url: %s", request->GetUri().CStr());
			response->SetStatusCode(http::StatusCode::BadRequest);
			return http::svr::NextHandler::DoNotCall;
		}

		auto vhost_app_name = ocst::Orchestrator::GetInstance()->ResolveApplicationNameFromDomain(request_url->Host(), request_url->App());
		if (vhost_app_name.IsValid() == false)
		{
			logte("Could not resolve application name from domain: %s", request_url->Host().CStr());
			response->SetStatusCode(http::StatusCode::NotFound);
			return http::svr::NextHandler::DoNotCall;
		}

		auto stream_name = request_url->Stream();

		// Set CORS header in response
		_cors_manager.SetupHttpCorsHeader(vhost_app_name, request, response);

		auto session_id = request_url->GetQueryValue("session");
		if (session_id.IsEmpty())
		{
			logte("Could not get session id from query string");
			response->SetStatusCode(http::StatusCode::BadRequest);
			return http::svr::NextHandler::DoNotCall;
		}

		auto if_match = request->GetHeader("If-Match");

		auto data = request->GetRequestBody();
		if (data == nullptr)
		{
			logte("Could not get request body");
			response->SetStatusCode(http::StatusCode::BadRequest);
			return http::svr::NextHandler::DoNotCall;
		}

		logti("Received PATCH request: %s", data->ToString().CStr());
		
		// Parse SDP
		auto patch_sdp = std::make_shared<SessionDescription>();
		if (patch_sdp->FromString(data->ToString()) == false)
		{
			logte("Could not parse SDP: %s", data->ToString().CStr());
			response->SetStatusCode(http::StatusCode::BadRequest);
			return http::svr::NextHandler::DoNotCall;
		}

		auto answer = _observer->OnTrickleCandidate(request, session_id, if_match, patch_sdp);
		response->SetStatusCode(answer._status_code);
		response->SetHeader("Content-Type", "application/trickle-ice-sdpfrag");

		if (answer._entity_tag.IsEmpty() == false)
		{
			response->SetHeader("ETag", answer._entity_tag);
		}

		if (answer._status_code == http::StatusCode::OK)
		{
			response->AppendString(answer._sdp->ToString());
		}
		else
		{
			// Set Error
			if (answer._error_message.IsEmpty() == false)
			{
				response->SetHeader("Content-Type", "text/plain");
				response->AppendString(answer._error_message);
			}
		}

		return http::svr::NextHandler::DoNotCall;
	});

	return interceptor;
}