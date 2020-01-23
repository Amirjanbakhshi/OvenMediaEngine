//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Hyunjun Jang
//  Copyright (c) 2019 AirenSoft. All rights reserved.
//
//==============================================================================
#include "./signals.h"
#include "./main_private.h"
#include "main.h"

#include <signal.h>

#include <base/ovlibrary/ovlibrary.h>

#include <config/config_manager.h>
#include <orchestrator/orchestrator.h>

#define SIGNAL_CASE(x) \
	case x:            \
		return #x;

static const char *GetSignalName(int signum)
{
	switch (signum)
	{
#if IS_LINUX
		// http://man7.org/linux/man-pages/man7/signal.7.html
		// Linux (CentOS)
		SIGNAL_CASE(SIGHUP);
		SIGNAL_CASE(SIGINT);
		SIGNAL_CASE(SIGQUIT);
		SIGNAL_CASE(SIGILL);
		SIGNAL_CASE(SIGTRAP);
		SIGNAL_CASE(SIGABRT);
		SIGNAL_CASE(SIGBUS);
		SIGNAL_CASE(SIGFPE);
		SIGNAL_CASE(SIGKILL);
		SIGNAL_CASE(SIGUSR1);
		SIGNAL_CASE(SIGSEGV);
		SIGNAL_CASE(SIGUSR2);
		SIGNAL_CASE(SIGPIPE);
		SIGNAL_CASE(SIGALRM);
		SIGNAL_CASE(SIGTERM);
		SIGNAL_CASE(SIGSTKFLT);
		SIGNAL_CASE(SIGCHLD);
		SIGNAL_CASE(SIGCONT);
		SIGNAL_CASE(SIGSTOP);
		SIGNAL_CASE(SIGTSTP);
		SIGNAL_CASE(SIGTTIN);
		SIGNAL_CASE(SIGTTOU);
		SIGNAL_CASE(SIGURG);
		SIGNAL_CASE(SIGXCPU);
		SIGNAL_CASE(SIGXFSZ);
		SIGNAL_CASE(SIGVTALRM);
		SIGNAL_CASE(SIGPROF);
		SIGNAL_CASE(SIGWINCH);
		SIGNAL_CASE(SIGPOLL);
		SIGNAL_CASE(SIGPWR);
		SIGNAL_CASE(SIGSYS);
#elif IS_MACOS
		// Apple OSX and iOS (Darwin)
		SIGNAL_CASE(SIGHUP);
		SIGNAL_CASE(SIGINT);
		SIGNAL_CASE(SIGQUIT);
		SIGNAL_CASE(SIGILL);
		SIGNAL_CASE(SIGTRAP);
		SIGNAL_CASE(SIGABRT);
		SIGNAL_CASE(SIGEMT);
		SIGNAL_CASE(SIGFPE);
		SIGNAL_CASE(SIGKILL);
		SIGNAL_CASE(SIGBUS);
		SIGNAL_CASE(SIGSEGV);
		SIGNAL_CASE(SIGSYS);
		SIGNAL_CASE(SIGPIPE);
		SIGNAL_CASE(SIGALRM);
		SIGNAL_CASE(SIGTERM);
		SIGNAL_CASE(SIGURG);
		SIGNAL_CASE(SIGSTOP);
		SIGNAL_CASE(SIGTSTP);
		SIGNAL_CASE(SIGCONT);
		SIGNAL_CASE(SIGCHLD);
		SIGNAL_CASE(SIGTTIN);
		SIGNAL_CASE(SIGTTOU);
		SIGNAL_CASE(SIGIO);
		SIGNAL_CASE(SIGXCPU);
		SIGNAL_CASE(SIGXFSZ);
		SIGNAL_CASE(SIGVTALRM);
		SIGNAL_CASE(SIGPROF);
		SIGNAL_CASE(SIGWINCH);
		SIGNAL_CASE(SIGINFO);
		SIGNAL_CASE(SIGUSR1);
		SIGNAL_CASE(SIGUSR2);
#endif
		default:
			return "UNKNOWN";
	}
}

static void AbortHandler(int signum, siginfo_t *si, void *unused)
{
	printf("%s %s\n", PLATFORM_NAME, GetSignalName(SIGSEGV));

	ov::StackTrace::WriteStackTrace(OME_VERSION, signum, GetSignalName(signum));
	::exit(signum);
}

static void ReloadHandler(int signum, siginfo_t *si, void *unused)
{
	logti("Trying to reload configuration...");

	auto config_manager = cfg::ConfigManager::Instance();

	if (config_manager->ReloadConfigs() == false)
	{
		logte("An error occurred while reload configuration");
		return;
	}

	logti("Trying to apply OriginMap to Orchestrator...");
	if (Orchestrator::GetInstance()->ApplyOriginMap(config_manager->GetServer()->GetVirtualHostList()) == false)
	{
		logte("Could not reload OriginMap");
	}
}

bool InitializeSignals()
{
	//	 1) SIGHUP		 2) SIGINT		 3) SIGQUIT		 4) SIGILL		 5) SIGTRAP
	//	 6) SIGABRT		 7) SIGBUS		 8) SIGFPE		 9) SIGKILL		10) SIGUSR1
	//	11) SIGSEGV		12) SIGUSR2		13) SIGPIPE		14) SIGALRM		15) SIGTERM
	//	16) SIGSTKFLT	17) SIGCHLD		18) SIGCONT		19) SIGSTOP		20) SIGTSTP
	//	21) SIGTTIN		22) SIGTTOU		23) SIGURG		24) SIGXCPU		25) SIGXFSZ
	//	26) SIGVTALRM	27) SIGPROF		28) SIGWINCH	29) SIGIO		30) SIGPWR
	//	31) SIGSYS		34) SIGRTMIN	35) SIGRTMIN+1	36) SIGRTMIN+2	37) SIGRTMIN+3
	//	38) SIGRTMIN+4	39) SIGRTMIN+5	40) SIGRTMIN+6	41) SIGRTMIN+7	42) SIGRTMIN+8
	//	43) SIGRTMIN+9	44) SIGRTMIN+10	45) SIGRTMIN+11	46) SIGRTMIN+12	47) SIGRTMIN+13
	//	48) SIGRTMIN+14	49) SIGRTMIN+15	50) SIGRTMAX-14	51) SIGRTMAX-13	52) SIGRTMAX-12
	//	53) SIGRTMAX-11	54) SIGRTMAX-10	55) SIGRTMAX-9	56) SIGRTMAX-8	57) SIGRTMAX-7
	//	58) SIGRTMAX-6	59) SIGRTMAX-5	60) SIGRTMAX-4	61) SIGRTMAX-3	62) SIGRTMAX-2
	//	63) SIGRTMAX-1	64) SIGRTMAX

	bool result = true;
	struct sigaction sa
	{
	};

	sa.sa_flags = SA_SIGINFO;
	sa.sa_sigaction = AbortHandler;
	::sigemptyset(&sa.sa_mask);

	// Intentional signals (ignore)
	//     SIGQUIT, SIGINT, SIGTERM, SIGTRAP, SIGHUP, SIGKILL
	//     SIGVTALRM, SIGPROF, SIGALRM

	// Core dumped signal
	result = result && (::sigaction(SIGABRT, &sa, nullptr) == 0);  // assert()
	result = result && (::sigaction(SIGSEGV, &sa, nullptr) == 0);  // illegal memory access
	result = result && (::sigaction(SIGBUS, &sa, nullptr) == 0);   // illegal memory access
	result = result && (::sigaction(SIGILL, &sa, nullptr) == 0);   // execute a malformed instruction.
	result = result && (::sigaction(SIGFPE, &sa, nullptr) == 0);   // divide by zero
	result = result && (::sigaction(SIGSYS, &sa, nullptr) == 0);   // bad system call
	result = result && (::sigaction(SIGXCPU, &sa, nullptr) == 0);  // cpu time limit exceeded
	result = result && (::sigaction(SIGXFSZ, &sa, nullptr) == 0);  // file size limit exceeded

	// Terminated signal
	result = result && (::sigaction(SIGPIPE, &sa, nullptr) == 0);  // write on a pipe with no one to read it
#if IS_LINUX
	result = result && (::sigaction(SIGPOLL, &sa, nullptr) == 0);  // pollable event
#endif															   // IS_LINUX

	// Configuration reload signal
	sa.sa_sigaction = ReloadHandler;

	result = result && (::sigaction(SIGHUP, &sa, nullptr) == 0);

	return result;
}