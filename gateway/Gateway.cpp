/*
 * Copyright 2005 - 2016 Zarafa and its licensors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include "config.h"
#include <atomic>
#include <kopano/platform.h>
#include <memory>
#include <new>
#include <set>
#include <climits>
#include <csignal>
#include <netdb.h>
#include <poll.h>
#include <inetmapi/inetmapi.h>
#include <mapi.h>
#include <mapix.h>
#include <mapidefs.h>
#include <mapicode.h>
#include <kopano/mapiext.h>
#include <kopano/tie.hpp>
#include <mapiguid.h>
#include <kopano/CommonUtil.h>
#include <kopano/stringutil.h>
#include <iostream>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <cerrno>
#include <kopano/ECLogger.h>
#include <kopano/ECConfig.h>
#include <kopano/MAPIErrors.h>
#include <kopano/my_getopt.h>
#include <kopano/ECChannel.h>
#include "POP3.h"
#include "IMAP.h"
#include <kopano/ecversion.h>
#include "SSLUtil.h"
#include "fileutil.h"
#include <kopano/UnixUtil.h>
#include <unicode/uclean.h>
#include <openssl/ssl.h>
#include <kopano/hl.hpp>

/**
 * @defgroup gateway Gateway for IMAP and POP3 
 * @{
 */

using namespace KC;
using namespace KC::string_literals;
using std::cout;
using std::endl;

struct socks {
	std::vector<struct pollfd> pollfd;
	std::vector<int> linfd;
	std::vector<bool> pop3, ssl;
};

static int daemonize = 1;
int quit = 0;
static bool bThreads, g_dump_config;
static const char *szPath;
static ECLogger *g_lpLogger = NULL;
static std::shared_ptr<ECConfig> g_lpConfig;
static pthread_t mainthread;
static std::atomic<int> nChildren{0};
static std::string g_strHostString;
static struct socks g_socks;

static void sigterm(int s)
{
	quit = 1;
}

static void sighup(int sig)
{
	if (bThreads && pthread_equal(pthread_self(), mainthread)==0)
		return;
	if (g_lpConfig != nullptr && !g_lpConfig->ReloadSettings() &&
	    g_lpLogger != nullptr)
		ec_log_err("Unable to reload configuration file, continuing with current settings.");
	if (g_lpLogger == nullptr || g_lpConfig == nullptr)
		return;
	ec_log_info("Got SIGHUP config was reloaded");

	const char *ll = g_lpConfig->GetSetting("log_level");
	int new_ll = ll ? atoi(ll) : EC_LOGLEVEL_WARNING;
	ec_log_get()->SetLoglevel(new_ll);

	if (strlen(g_lpConfig->GetSetting("ssl_private_key_file")) > 0 &&
		strlen(g_lpConfig->GetSetting("ssl_certificate_file")) > 0) {
		if (ECChannel::HrSetCtx(g_lpConfig.get()) != hrSuccess)
			ec_log_err("Error reloading SSL context");
		else
			ec_log_info("Reloaded SSL context");
	}
	ec_log_get()->Reset();
	ec_log_info("Log connection was reset");
}

static void sigchld(int)
{
	int stat;
	while (waitpid(-1, &stat, WNOHANG) > 0)
		--nChildren;
}

// SIGSEGV catcher
static void sigsegv(int signr, siginfo_t *si, void *uc)
{
	generic_sigsegv_handler(g_lpLogger, "kopano-gateway", PROJECT_VERSION, signr, si, uc);
}

static HRESULT running_service(const char *szPath, const char *servicename);

static void print_help(const char *name)
{
	cout << "Usage:\n" << endl;
	cout << name << " [-F] [-h|--host <serverpath>] [-c|--config <configfile>]" << endl;
	cout << "  -F\t\tDo not run in the background" << endl;
	cout << "  -h path\tUse alternate connect path (e.g. file:///var/run/socket).\n\t\tDefault: file:///var/run/kopano/server.sock" << endl;
	cout << "  -V Print version info." << endl;
	cout << "  -c filename\tUse alternate config file (e.g. /etc/kopano-gateway.cfg)\n\t\tDefault: /etc/kopano/gateway.cfg" << endl;
	cout << endl;
}

enum serviceType { ST_POP3 = 0, ST_IMAP };

struct HandlerArgs {
	serviceType type;
	std::unique_ptr<ECChannel> lpChannel;
	ECLogger *lpLogger;
	std::shared_ptr<ECConfig> lpConfig;
	bool bUseSSL;
};

static void *Handler(void *lpArg)
{
	auto lpHandlerArgs = static_cast<HandlerArgs *>(lpArg);
	std::shared_ptr<ECChannel> lpChannel(std::move(lpHandlerArgs->lpChannel));
	auto lpLogger = lpHandlerArgs->lpLogger;
	auto lpConfig = std::move(lpHandlerArgs->lpConfig);
	auto bUseSSL = lpHandlerArgs->bUseSSL;

	// szPath is global, pointing to argv variable, or lpConfig variable
	ClientProto *client;
	if (lpHandlerArgs->type == ST_POP3)
		client = new POP3(szPath, lpChannel, lpConfig);
	else
		client = new IMAP(szPath, lpChannel, lpConfig);
	// not required anymore
	delete lpHandlerArgs;

	// make sure the pipe logger does not exit when this handler exits, but only frees the memory.
	auto pipelog = dynamic_cast<ECLogger_Pipe *>(lpLogger);
	if (pipelog != nullptr)
		pipelog->Disown();

	std::string inBuffer;
	HRESULT hr;
	bool bQuit = false;
	int timeouts = 0;

	if (bUseSSL && lpChannel->HrEnableTLS() != hrSuccess) {
		ec_log_err("Unable to negotiate SSL connection");
		goto exit;
	}

	try {
		hr = client->HrSendGreeting(g_strHostString);
	} catch (const KMAPIError &e) {
		hr = e.code();
	}
	if (hr != hrSuccess)
		goto exit;

	// Main command loop
	while (!bQuit && !quit) {
		// check for data
		hr = lpChannel->HrSelect(60);
		if (hr == MAPI_E_CANCEL)
			/* signalled - reevaluate bQuit */
			continue;
		if (hr == MAPI_E_TIMEOUT) {
			if (++timeouts < client->getTimeoutMinutes())
				// ignore select() timeout for 5 (POP3) or 30 (IMAP) minutes
				continue;
			// close idle first, so we don't have a race condition with the channel
			client->HrCloseConnection("BYE Connection closed because of timeout");
			ec_log_err("Connection closed because of timeout");
			bQuit = true;
			break;
		} else if (hr == MAPI_E_NETWORK_ERROR) {
			ec_log_err("Socket error: %s", strerror(errno));
			bQuit = true;
			break;
		}

		timeouts = 0;

		inBuffer.clear();
		hr = lpChannel->HrReadLine(inBuffer);
		if (hr != hrSuccess) {
			if (errno)
				ec_log_err("Failed to read line: %s", strerror(errno));
			else
				ec_log_err("Client disconnected");
			bQuit = true;
			break;
		}

		if (quit) {
			client->HrCloseConnection("BYE server shutting down");
			hr = MAPI_E_CALL_FAILED;
			bQuit = true;
			break;
		}

		if (client->isContinue()) {
			// we asked the client for more data, do not parse the buffer, but send it "to the previous command"
			// that last part is currently only HrCmdAuthenticate(), so no difficulties here.
			// also, PLAIN is the only supported auth method.
			client->HrProcessContinue(inBuffer);
			// no matter what happens, we continue handling the connection.
			continue;
		}

		HRESULT hr = hrSuccess;
		try {
			/* Process IMAP command */
			hr = client->HrProcessCommand(inBuffer);
		} catch (const KC::KMAPIError &e) {
			hr = e.code();
		}

		if (hr == MAPI_E_NETWORK_ERROR) {
			ec_log_err("Connection error.");
			bQuit = true;
		}
		if (hr == MAPI_E_END_OF_SESSION) {
			ec_log_notice("Disconnecting client.");
			bQuit = true;
		}
	}

exit:
	ec_log_notice("Client %s thread exiting", lpChannel->peer_addr());
	client->HrDone(false);	// HrDone does not send an error string to the client
	delete client;
	if (!bThreads)
		g_lpLogger->Release();
	/** free SSL error data **/
	#if OPENSSL_VERSION_NUMBER < 0x10100000L
		ERR_remove_state(0);
	#endif

	if (bThreads)
		--nChildren;

	return NULL;
}

static void *Handler_Threaded(void *a)
{
	/*
	 * Steer the control signals to the main thread for consistency with
	 * the forked mode.
	 */
	++nChildren;
	kcsrv_blocksigs();
	return Handler(a);
}

static std::string GetServerFQDN()
{
	std::string retval = "localhost";
	char hostname[256] = {0};
	struct addrinfo *result = nullptr;

	auto rc = gethostname(hostname, sizeof(hostname));
	if (rc != 0)
		return retval;
	retval = hostname;
	rc = getaddrinfo(hostname, nullptr, nullptr, &result);
	if (rc != 0 || result == nullptr)
		return retval;
	/* Name lookup is required, so set that flag */
	rc = getnameinfo(result->ai_addr, result->ai_addrlen, hostname,
	     sizeof(hostname), nullptr, 0, NI_NAMEREQD);
	if (rc != 0)
		goto exit;
	if (hostname[0] != '\0')
		retval = hostname;
 exit:
	if (result)
		freeaddrinfo(result);
	return retval;
}

int main(int argc, char *argv[]) {
	HRESULT hr = hrSuccess;
	int c = 0;
	bool bIgnoreUnknownConfigOptions = false;

	ssl_threading_setup();
	const char *szConfig = ECConfig::GetDefaultPath("gateway.cfg");
	bool exp_config = false;
	static const configsetting_t lpDefaults[] = {
		{ "server_bind", "" },
		{ "run_as_user", "kopano" },
		{ "run_as_group", "kopano" },
		{ "pid_file", "/var/run/kopano/gateway.pid" },
		{"running_path", "/var/lib/kopano/empty"},
		{ "process_model", "thread" },
		{"coredump_enabled", "systemdefault"},
		{"pop3_listen", ""}, /* default in gw_listen() */
		{"pop3s_listen", ""},
		{"imap_listen", ""}, /* default in gw_listen() */
		{"imaps_listen", ""},
		{"pop3_enable", "auto", CONFIGSETTING_NONEMPTY | CONFIGSETTING_OBSOLETE},
		{"pop3_port", "110", CONFIGSETTING_NONEMPTY | CONFIGSETTING_OBSOLETE},
		{"pop3s_enable", "auto", CONFIGSETTING_NONEMPTY | CONFIGSETTING_OBSOLETE},
		{"pop3s_port", "995", CONFIGSETTING_NONEMPTY | CONFIGSETTING_OBSOLETE},
		{"imap_enable", "auto", CONFIGSETTING_NONEMPTY | CONFIGSETTING_OBSOLETE},
		{"imap_port", "143", CONFIGSETTING_NONEMPTY | CONFIGSETTING_OBSOLETE},
		{"imaps_enable", "auto", CONFIGSETTING_NONEMPTY | CONFIGSETTING_OBSOLETE},
		{"imaps_port", "993", CONFIGSETTING_NONEMPTY | CONFIGSETTING_OBSOLETE},
		{ "imap_only_mailfolders", "yes", CONFIGSETTING_RELOADABLE },
		{ "imap_public_folders", "yes", CONFIGSETTING_RELOADABLE },
		{ "imap_capability_idle", "yes", CONFIGSETTING_RELOADABLE },
		{ "imap_always_generate", "no", CONFIGSETTING_UNUSED },
		{ "imap_max_fail_commands", "10", CONFIGSETTING_RELOADABLE },
		{ "imap_max_messagesize", "128M", CONFIGSETTING_RELOADABLE | CONFIGSETTING_SIZE },
		{ "imap_generate_utf8", "no", CONFIGSETTING_UNUSED },
		{ "imap_expunge_on_delete", "no", CONFIGSETTING_RELOADABLE },
		{ "imap_store_rfc822", "", CONFIGSETTING_UNUSED },
		{ "imap_cache_folders_time_limit", "", CONFIGSETTING_UNUSED },
		{ "imap_ignore_command_idle", "no", CONFIGSETTING_RELOADABLE },
		{ "disable_plaintext_auth", "no", CONFIGSETTING_RELOADABLE },
		{ "server_socket", "http://localhost:236/" },
		{ "server_hostname", "" },
		{ "server_hostname_greeting", "no", CONFIGSETTING_RELOADABLE },
		{"ssl_private_key_file", "/etc/kopano/gateway/privkey.pem", CONFIGSETTING_RELOADABLE},
		{"ssl_certificate_file", "/etc/kopano/gateway/cert.pem", CONFIGSETTING_RELOADABLE},
		{"ssl_verify_client", "no", CONFIGSETTING_RELOADABLE},
		{"ssl_verify_file", "", CONFIGSETTING_RELOADABLE},
		{"ssl_verify_path", "", CONFIGSETTING_RELOADABLE},
#ifdef SSL_TXT_SSLV2
		{"ssl_protocols", "!SSLv2", CONFIGSETTING_RELOADABLE},
#else
		{"ssl_protocols", "", CONFIGSETTING_RELOADABLE},
#endif
		{"ssl_ciphers", "ALL:!LOW:!SSLv2:!EXP:!aNULL", CONFIGSETTING_RELOADABLE},
		{"ssl_prefer_server_ciphers", "no", CONFIGSETTING_RELOADABLE},
		{"log_method", "auto", CONFIGSETTING_NONEMPTY},
		{"log_file", ""},
		{"log_level", "3", CONFIGSETTING_NONEMPTY | CONFIGSETTING_RELOADABLE},
		{ "log_timestamp", "1" },
		{ "log_buffer_size", "0" },
		{ "tmp_path", "/tmp" },
		{"bypass_auth", "no"},
		{"html_safety_filter", "no"},
		{ NULL, NULL },
	};
	enum {
		OPT_HELP = UCHAR_MAX + 1,
		OPT_HOST,
		OPT_CONFIG,
		OPT_FOREGROUND,
		OPT_IGNORE_UNKNOWN_CONFIG_OPTIONS,
		OPT_DUMP_CONFIG,
	};
	static const struct option long_options[] = {
		{"help", 0, NULL, OPT_HELP},
		{"host", 1, NULL, OPT_HOST},
		{"config", 1, NULL, OPT_CONFIG},
		{"foreground", 1, NULL, OPT_FOREGROUND},
		{ "ignore-unknown-config-options", 0, NULL, OPT_IGNORE_UNKNOWN_CONFIG_OPTIONS },
		{"dump-config", no_argument, nullptr, OPT_DUMP_CONFIG},
		{NULL, 0, NULL, 0}
	};

	// Get commandline options
	while (1) {
		c = my_getopt_long_permissive(argc, argv, "c:h:iuFV", long_options, NULL);

		if (c == -1)
			break;

		switch (c) {
		case OPT_CONFIG:
		case 'c':
			szConfig = optarg;
			exp_config = true;
			break;
		case OPT_HOST:
		case 'h':
			szPath = optarg;
			break;
		case 'i':				// Install service
		case 'u':				// Uninstall service
			break;
		case OPT_FOREGROUND:
		case 'F':
			daemonize = 0;
			break;
		case OPT_IGNORE_UNKNOWN_CONFIG_OPTIONS:
			bIgnoreUnknownConfigOptions = true;
			break;
		case OPT_DUMP_CONFIG:
			g_dump_config = true;
			break;
		case 'V':
			cout << "kopano-gateway " PROJECT_VERSION << endl;
			return 1;
		case OPT_HELP:
		default:
			print_help(argv[0]);
			return 1;
		}
	}

	// Setup config
	g_lpConfig.reset(ECConfig::Create(lpDefaults));
	if (!g_lpConfig->LoadSettings(szConfig, !exp_config) ||
	    g_lpConfig->ParseParams(argc - optind, &argv[optind]) < 0 ||
	    (!bIgnoreUnknownConfigOptions && g_lpConfig->HasErrors())) {
		g_lpLogger = new ECLogger_File(EC_LOGLEVEL_INFO, 0, "-", false);	// create logger without a timestamp to stderr
		if (g_lpLogger == nullptr) {
			hr = MAPI_E_NOT_ENOUGH_MEMORY;
			goto exit;
		}
		ec_log_set(g_lpLogger);
		LogConfigErrors(g_lpConfig.get());
		hr = E_FAIL;
		goto exit;
	}
	if (g_dump_config)
		return g_lpConfig->dump_config(stdout) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

	// Setup logging
	g_lpLogger = CreateLogger(g_lpConfig.get(), argv[0], "KopanoGateway");
	ec_log_set(g_lpLogger);

	if ((bIgnoreUnknownConfigOptions && g_lpConfig->HasErrors()) || g_lpConfig->HasWarnings())
		LogConfigErrors(g_lpConfig.get());
	if (!TmpPath::instance.OverridePath(g_lpConfig.get()))
		ec_log_err("Ignoring invalid path-setting!");
	if (parseBool(g_lpConfig->GetSetting("bypass_auth")))
		ec_log_warn("Gateway is started with bypass_auth=yes meaning username and password will not be checked.");
	if (strcmp(g_lpConfig->GetSetting("process_model"), "thread") == 0) {
		bThreads = true;
		g_lpLogger->SetLogprefix(LP_TID);
	}
	mainthread = pthread_self();
	if (!szPath)
		szPath = g_lpConfig->GetSetting("server_socket");

	g_strHostString = g_lpConfig->GetSetting("server_hostname", NULL, "");
	if (g_strHostString.empty())
		g_strHostString = GetServerFQDN();
	g_strHostString.insert(0, " on ");
	hr = running_service(szPath, argv[0]);

exit:
	if (hr != hrSuccess)
		fprintf(stderr, "%s: Startup failed: %s (%x). Please check the logfile (%s) for details.\n",
			argv[0], GetMAPIErrorMessage(hr), hr, g_lpConfig->GetSetting("log_file"));
	ssl_threading_cleanup();
	DeleteLogger(g_lpLogger);

	return hr == hrSuccess ? 0 : 1;
}

static HRESULT gw_listen(ECConfig *cfg)
{
	/* Modern directives */
	auto pop3_sock  = vector_to_set<std::string, ec_bindaddr_less>(tokenize(cfg->GetSetting("pop3_listen"), ' ', true));
	auto pop3s_sock = vector_to_set<std::string, ec_bindaddr_less>(tokenize(cfg->GetSetting("pop3s_listen"), ' ', true));
	auto imap_sock  = vector_to_set<std::string, ec_bindaddr_less>(tokenize(cfg->GetSetting("imap_listen"), ' ', true));
	auto imaps_sock = vector_to_set<std::string, ec_bindaddr_less>(tokenize(cfg->GetSetting("imaps_listen"), ' ', true));
	/*
	 * Historic directives. Enclosing in [] is a nudge for the parser and
	 * will work with non-IPv6 too.
	 */
	auto addr = cfg->GetSetting("server_bind");
	auto cvar = g_lpConfig->GetSetting("pop3_enable");
	if (!parseBool(cvar)) {
		/* vetoes everything */
		pop3_sock.clear();
	} else if (strcmp(cvar, "yes") == 0) {
		/* "yes" := "read extra historic variable" */
		auto port = cfg->GetSetting("pop3_port");
		if (port[0] != '\0')
			pop3_sock.emplace("["s + addr + "]:" + port);
	} else if (pop3_sock.empty()) {
		pop3_sock.emplace("*:110");
	}
	cvar = g_lpConfig->GetSetting("pop3s_enable");
	if (!parseBool(cvar)) {
		pop3s_sock.clear();
	} else if (strcmp(cvar, "yes") == 0) {
		auto port = cfg->GetSetting("pop3s_port");
		if (port[0] != '\0')
			pop3s_sock.emplace("["s + addr + "]:" + port);
	}
	cvar = g_lpConfig->GetSetting("imap_enable");
	if (!parseBool(cvar)) {
		imap_sock.clear();
	} else if (strcmp(cvar, "yes") == 0) {
		auto port = cfg->GetSetting("imap_port");
		if (port[0] != '\0')
			imap_sock.emplace("["s + addr + "]:" + port);
	} else if (imap_sock.empty()) {
		imap_sock.emplace("*:143");
	}
	cvar = g_lpConfig->GetSetting("imaps_enable");
	if (!parseBool(cvar)) {
		imaps_sock.clear();
	} else if (strcmp(cvar, "yes") == 0) {
		auto port = cfg->GetSetting("imaps_port");
		if (port[0] != '\0')
			imaps_sock.emplace("["s + addr + "]:" + port);
	}

	if ((!pop3s_sock.empty() || !imaps_sock.empty()) &&
	    ECChannel::HrSetCtx(g_lpConfig.get()) != hrSuccess) {
		ec_log_err("Error loading SSL context, POP3S and IMAPS will be disabled");
		pop3s_sock.clear();
		imaps_sock.clear();
	}
	if (pop3_sock.empty() && pop3s_sock.empty() &&
	    imap_sock.empty() && imaps_sock.empty()) {
		ec_log_crit("POP3, POP3S, IMAP and IMAPS are all four disabled");
		return E_FAIL;
	}

	/* Launch */
	struct pollfd pfd;
	memset(&pfd, 0, sizeof(pfd));
	pfd.events = POLLIN;
	int ret;
	for (const auto &spec : pop3_sock) {
		ret = ec_listen_generic(spec.c_str(), &pfd.fd);
		if (ret < 0)
			return MAPI_E_NETWORK_ERROR;
		g_socks.pollfd.push_back(pfd);
		g_socks.linfd.push_back(pfd.fd);
		g_socks.pop3.push_back(true);
		g_socks.ssl.push_back(false);
	}
	for (const auto &spec : pop3s_sock) {
		ret = ec_listen_generic(spec.c_str(), &pfd.fd);
		if (ret < 0)
			return MAPI_E_NETWORK_ERROR;
		g_socks.pollfd.push_back(pfd);
		g_socks.linfd.push_back(pfd.fd);
		g_socks.pop3.push_back(true);
		g_socks.ssl.push_back(true);
	}
	for (const auto &spec : imap_sock) {
		ret = ec_listen_generic(spec.c_str(), &pfd.fd);
		if (ret < 0)
			return MAPI_E_NETWORK_ERROR;
		g_socks.pollfd.push_back(pfd);
		g_socks.linfd.push_back(pfd.fd);
		g_socks.pop3.push_back(false);
		g_socks.ssl.push_back(false);
	}
	for (const auto &spec : imaps_sock) {
		ret = ec_listen_generic(spec.c_str(), &pfd.fd);
		if (ret < 0)
			return MAPI_E_NETWORK_ERROR;
		g_socks.pollfd.push_back(pfd);
		g_socks.linfd.push_back(pfd.fd);
		g_socks.pop3.push_back(false);
		g_socks.ssl.push_back(true);
	}
	return hrSuccess;
}

/**
 * Runs the gateway service, starting a new thread or fork child for
 * incoming connections on any configured service.
 *
 * @param[in]	szPath		Unused, should be removed.
 * @param[in]	servicename	Name of the service, used to create a Unix pidfile.
 */
static HRESULT running_service(const char *szPath, const char *servicename)
{
	HRESULT hr = hrSuccess;
	int err = 0;
	pthread_attr_t ThreadAttr;
	ec_log(EC_LOGLEVEL_ALWAYS, "Starting kopano-gateway version " PROJECT_VERSION " (pid %d)", getpid());

	// SIGSEGV backtrace support
	KAlternateStack sigstack;
	struct sigaction act;
	memset(&act, 0, sizeof(act));

	if (bThreads) {
		if (pthread_attr_init(&ThreadAttr) != 0)
			return MAPI_E_NOT_ENOUGH_MEMORY;
		if (pthread_attr_setdetachstate(&ThreadAttr, PTHREAD_CREATE_DETACHED) != 0) {
			ec_log_err("Can't set thread attribute to detached");
			goto exit;
		}
		// 1Mb of stack space per thread
		if (pthread_attr_setstacksize(&ThreadAttr, 1024 * 1024)) {
			ec_log_err("Can't set thread stack size to 1Mb");
			goto exit;
		}
	}

	hr = gw_listen(g_lpConfig.get());
	if (hr != hrSuccess)
		return hr;

	// Setup signals
	signal(SIGPIPE, SIG_IGN);
	act.sa_sigaction = sigsegv;
	act.sa_flags = SA_ONSTACK | SA_RESETHAND | SA_SIGINFO;
	sigemptyset(&act.sa_mask);
    sigaction(SIGSEGV, &act, NULL);
    sigaction(SIGBUS, &act, NULL);
    sigaction(SIGABRT, &act, NULL);
	act.sa_flags   = SA_RESTART;
	act.sa_handler = sigterm;
	sigaction(SIGTERM, &act, nullptr);
	sigaction(SIGINT, &act, nullptr);
	act.sa_handler = sighup;
	sigaction(SIGHUP, &act, nullptr);
	act.sa_handler = sigchld;
	sigaction(SIGCHLD, &act, nullptr);

    struct rlimit file_limit;
	file_limit.rlim_cur = KC_DESIRED_FILEDES;
	file_limit.rlim_max = KC_DESIRED_FILEDES;
    
	if (setrlimit(RLIMIT_NOFILE, &file_limit) < 0)
		ec_log_warn("setrlimit(RLIMIT_NOFILE, %d) failed, you will only be able to connect up to %d sockets. Either start the process as root, or increase user limits for open file descriptors", KC_DESIRED_FILEDES, getdtablesize());
	unix_coredump_enable(g_lpConfig->GetSetting("coredump_enabled"));

	// fork if needed and drop privileges as requested.
	// this must be done before we do anything with pthreads
	if (unix_runas(g_lpConfig.get()))
		goto exit;
	if (daemonize && unix_daemonize(g_lpConfig.get()))
		goto exit;
	if (!daemonize)
		setsid();
	unix_create_pidfile(servicename, g_lpConfig.get());
	if (!bThreads)
		g_lpLogger = StartLoggerProcess(g_lpConfig.get(), g_lpLogger); // maybe replace logger
	ec_log_set(g_lpLogger);

	hr = MAPIInitialize(NULL);
	if (hr != hrSuccess) {
		ec_log_crit("Unable to initialize MAPI: %s (%x)",
			GetMAPIErrorMessage(hr), hr);
		goto exit;
	}

	ec_log(EC_LOGLEVEL_ALWAYS, "Starting kopano-gateway version " PROJECT_VERSION " (pid %d)", getpid());
	// Mainloop
	while (!quit) {
		auto nfds = g_socks.pollfd.size();
		for (size_t i = 0; i < nfds; ++i)
			g_socks.pollfd[i].revents = 0;
		err = poll(&g_socks.pollfd[0], nfds, 10 * 1000);
		if (err < 0) {
			if (errno != EINTR) {
				ec_log_err("Socket error: %s", strerror(errno));
				quit = 1;
				hr = MAPI_E_NETWORK_ERROR;
			}
			continue;
		} else if (err == 0) {
			continue;
		}

		for (size_t i = 0; i < nfds; ++i) {
			if (!(g_socks.pollfd[i].revents & POLLIN))
				/* OS might set more bits than requested */
				continue;

		// One socket has signalled a new incoming connection
		std::unique_ptr<HandlerArgs> lpHandlerArgs(new HandlerArgs);
		lpHandlerArgs->lpLogger = g_lpLogger;
		lpHandlerArgs->lpConfig = g_lpConfig;
		lpHandlerArgs->type = g_socks.pop3[i] ? ST_POP3 : ST_IMAP;
		lpHandlerArgs->bUseSSL = g_socks.ssl[i];
		const char *method = "", *model = bThreads ? "thread" : "process";

		if (lpHandlerArgs->type == ST_POP3)
			method = lpHandlerArgs->bUseSSL ? "POP3s" : "POP3";
		else if (lpHandlerArgs->type == ST_IMAP)
			method = lpHandlerArgs->bUseSSL ? "IMAPs" : "IMAP";
		hr = HrAccept(g_socks.pollfd[i].fd, &unique_tie(lpHandlerArgs->lpChannel));
		if (hr != hrSuccess) {
			ec_log_err("Unable to accept %s socket connection.", method);
			continue;
		}

		pthread_t tid;
		ec_log_notice("Starting worker %s for %s request", model, method);
		if (!bThreads) {
			++nChildren;
			if (unix_fork_function(Handler, lpHandlerArgs.get(), g_socks.linfd.size(), &g_socks.linfd[0]) < 0) {
				ec_log_err("Could not create %s %s: %s", method, model, strerror(errno));
				--nChildren;
			}
			continue;
		}
		err = pthread_create(&tid, &ThreadAttr, Handler_Threaded, lpHandlerArgs.get());
		if (err != 0) {
			ec_log_err("Could not create %s %s: %s", method, model, strerror(err));
			continue;
		}
		set_thread_name(tid, "ZGateway " + std::string(method));
		lpHandlerArgs.release();
		}
	}

	ec_log(EC_LOGLEVEL_ALWAYS, "POP3/IMAP Gateway will now exit");
	// in forked mode, send all children the exit signal
	if (!bThreads) {
		signal(SIGTERM, SIG_IGN);
		kill(0, SIGTERM);
	}

	// wait max 10 seconds (init script waits 15 seconds)
	for (int i = 10; nChildren != 0 && i != 0; --i) {
		if (i % 5 == 0)
			ec_log_warn("Waiting for %d processes/threads to exit", nChildren.load());
		sleep(1);
	}

	if (nChildren)
		ec_log_warn("Forced shutdown with %d processes/threads left", nChildren.load());
	else
		ec_log_notice("POP3/IMAP Gateway shutdown complete");
	MAPIUninitialize();

exit:
	ECChannel::HrFreeCtx();
	SSL_library_cleanup(); // Remove SSL data for the main application and other related libraries
	if (bThreads)
		pthread_attr_destroy(&ThreadAttr);
	// cleanup ICU data so valgrind is happy
	u_cleanup();
	return hr;
}

/** @} */
