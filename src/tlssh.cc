/** \file src/tlssh.cc
 * \brief Main tlssh client source file
 *
 * @mainpage TLSSH
 * @author Thomas Habets <thomas@habets.pp.se>
 * @defgroup TLSSH TLSSH Client
 */
/*
 * (BSD license without advertising clause below)
 *
 * Copyright (c) 2005-2009 Thomas Habets. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include<poll.h>
#include<termios.h>
#include<unistd.h>
#include<wordexp.h>
#include<signal.h>
#include<sys/ioctl.h>
#include<arpa/inet.h>

#include<iostream>
#include<fstream>

#include"tlssh.h"
#include"util.h"
#include"sslsocket.h"
#include"configparser.h"

using namespace tlssh_common;

BEGIN_NAMESPACE(tlssh)

// Constants
const char *argv0 = NULL;
const std::string protocol_version     = "tlssh.1";

const std::string DEFAULT_PORT         = "12345";
const std::string DEFAULT_CERTFILE     = "~/.tlssh/keys/default.crt";
const std::string DEFAULT_KEYFILE      = "~/.tlssh/keys/default.key";
const std::string DEFAULT_SERVERCAFILE = "/etc/tlssh/ServerCA.crt";
const std::string DEFAULT_SERVERCRL    = "/etc/tlssh/ServerCRL.der";
const std::string DEFAULT_SERVERCAPATH = "";
const std::string DEFAULT_CONFIG       = "/etc/tlssh/tlssh.conf";
const std::string DEFAULT_CIPHER_LIST  = "HIGH";
const std::string DEFAULT_TCP_MD5      = "tlssh";
const int         DEFAULT_AF           = AF_UNSPEC;


struct Options {
	std::string port;
	std::string certfile;
	std::string keyfile;
	std::string servercafile;
	std::string servercapath;
	std::string servercrl;
	std::string config;
	std::string cipher_list;
	std::string host;
	std::string tcp_md5;
	unsigned int verbose;
        int af;
};
Options options = {
 port:         DEFAULT_PORT,
 certfile:     DEFAULT_CERTFILE,
 keyfile:      DEFAULT_KEYFILE,
 servercafile: DEFAULT_SERVERCAFILE,
 servercapath: DEFAULT_SERVERCAPATH,
 servercrl:    DEFAULT_SERVERCRL,
 config:       DEFAULT_CONFIG,
 cipher_list:  DEFAULT_CIPHER_LIST,
 host:         "",
 tcp_md5:      DEFAULT_TCP_MD5,
 verbose:      0,
 af:           AF_UNSPEC,
};
	
SSLSocket sock;

bool sigwinch_received = true;
/** SIGWINCH handler
 *
 */
void
sigwinch(int)
{
        sigwinch_received = true;
}

/** Get local terminal size
 * @return Height & width of terminal connected to stdin.
 */
std::pair<int,int>
terminal_size()
{
        struct winsize ws;
        if (ioctl(fileno(stdin), TIOCGWINSZ, (char *)&ws)) {
                THROW(Err::ErrSys, "ioctl(TIOCGWINSZ)");
        }
        return std::pair<int,int>(ws.ws_row, ws.ws_col);
}

/** Generate IAC sequence for new terminal size.
 *
 */
std::string
iac_window_size()
{
        std::pair<int,int> ts(terminal_size());

        IACCommand cmd;
        cmd.s.iac = 255;
        cmd.s.command = 1;
        cmd.s.commands.ws.rows = htons(ts.first);
        cmd.s.commands.ws.cols = htons(ts.second);
        return std::string(&cmd.buf[0], &cmd.buf[6]);
}

/** Get terminal type of local terminal.
 *
 * @todo Only allow letters and numbers here
 */
std::string
terminal_type()
{
        return getenv("TERM");
}

/** Replace iac byte (255) with two iac bytes.
 *
 * @param[in] in Input string
 * @return       Escaped string
 */
std::string
escape_iac(const std::string &in)
{
        size_t startpos = 0, endpos;
        std::string ret;
        bool first_try = true;

        for (;;) {
                endpos = in.find(255, startpos);
                if (endpos == std::string::npos) {
                        break;
                }

                ret += in.substr(startpos, endpos-startpos) + "\xff\xff";
                startpos = endpos + 1;
        }
        if (first_try) {
                return in;
        }

        return ret + in.substr(startpos);
}

/** Main loop reading from terminal and writing to socket, and vice versa.
 *
 */
void
mainloop(FDWrap &terminal)
{
	struct pollfd fds[2];
	int err;
	std::string to_server;
	std::string to_terminal;

	for (;;) {
                if (sigwinch_received) {
                        sigwinch_received = false;
                        to_server += iac_window_size();
                }

		fds[0].fd = sock.getfd();
		fds[0].events = POLLIN;
		if (!to_server.empty()) {
			fds[0].events |= POLLOUT;
		}

		fds[1].fd = terminal.get();
		fds[1].events = POLLIN;
		if (!to_terminal.empty()) {
			fds[1].events |= POLLOUT;
		}

		err = poll(fds, 2, -1);
		if (!err) { // timeout
			continue;
		}
		if (0 > err) { // error
			continue;
		}

		// from client
		if (fds[0].revents & POLLIN) {
			try {
				do {
					to_terminal += sock.read();
				} while (sock.ssl_pending());
			} catch(const Socket::ErrPeerClosed &e) {
				return;
			}
		}

		// from terminal
		if (fds[1].revents & POLLIN) {
			to_server += escape_iac(terminal.read());
		}

		if ((fds[0].revents & POLLOUT)
		    && !to_server.empty()) {
			size_t n;
			n = sock.write(to_server);
			to_server = to_server.substr(n);
		}

		if ((fds[1].revents & POLLOUT)
		    && !to_terminal.empty()) {
			size_t n;
			n = terminal.write(to_terminal);
			to_terminal = to_terminal.substr(n);
		}
	}
}


struct termios old_tio;
bool old_tio_set = false;
/** Reset the terminal (termios) to what it was before this program was run
 *
 * This function is called by atexit()-hooks
 */
void
reset_tio(void)
{
	if (old_tio_set) {
		tcsetattr(0, TCSADRAIN, &old_tio);
	}
}

/** Set up a new connection.
 *
 * At this point 'sock' is ready to use, but SSL negotiations have not yet
 * started.
 *
 * @return Normal UNIX-style exit() value. Will be used by main()
 */
int
new_connection()
{
	sock.ssl_connect(options.host);
        sock.write("version " + protocol_version + "\n");
        sock.write("env TERM " + terminal_type() + "\n");
        sock.write("\n");

	FDWrap terminal(0, false);

	if (tcgetattr(terminal.get(), &old_tio)) {
                THROW(Err::ErrSys, "tcgetattr()");
        }
	old_tio_set = true;
	if (atexit(reset_tio)) {
                THROW(Err::ErrSys, "atexit(reset_tio)");
        }

	struct termios tio;
	cfmakeraw(&tio);
	if (tcsetattr(terminal.get(), TCSADRAIN, &tio)) {
                THROW(Err::ErrSys, "tcsetattr(,TCSADRAIN,)");
        }


	mainloop(terminal);
        return 0;
}

/** Show usage info (-h, --help) and exit
 *
 */
void
usage(int err)
{
	printf("%s [ -46hv ] "
	       "[ -c <config> ] "
	       "[ -C <cipher-list> ] "
	       "[ -p <cert+keyfile> ]"
	       "\n"
	       "\t-c <config>          Config file (default %s)\n"
	       "\t-C <cipher-list>     Acceptable ciphers (default %s)\n"
	       "\t-h, --help           Help\n"
	       "\t-V, --version        Print version and exit\n"
	       "\t-p <cert+keyfile>    Load login cert+key from file\n"
	       , argv0,
	       DEFAULT_CONFIG.c_str(), DEFAULT_CIPHER_LIST.c_str());
	exit(err);
}

/** Print version info according to GNU coding standards
 *
 */
void
print_version()
{
	printf("tlssh %s\n"
	       "Copyright (C) 2010 Thomas Habets <thomas@habets.pp.se>\n"
	       "License GPLv2: GNU GPL version 2 or later "
	       "<http://gnu.org/licenses/gpl-2.0.html>\n"
	       "This is free software: you are free to change and "
	       "redistribute it.\n"
	       "There is NO WARRANTY, to the extent permitted by law.\n",
	       VERSION);
}

/** Read config file
 *
 * @param[in] fn Config file name
 */
void
read_config_file(const std::string &fn)
{
	std::ifstream fi(fn.c_str());
	ConfigParser conf(fi);
	ConfigParser end;
	for (;conf != end; ++conf) {
		if (conf->keyword.empty()) {
			// empty
		} else if (conf->keyword[0] == '#') {
			// comment
		} else if (conf->keyword == "Port"
                           && conf->parms.size() == 1) {
			options.port = conf->parms[0];
		} else if (conf->keyword == "ServerCAFile"
                           && conf->parms.size() == 1) {
			options.servercafile = conf->parms[0];
		} else if (conf->keyword == "ServerCAPath"
                           && conf->parms.size() == 1) {
			options.servercapath = conf->parms[0];
		} else if (conf->keyword == "ServerCRL"
                           && conf->parms.size() == 1) {
			options.servercrl = conf->parms[0];
		} else if (conf->keyword == "CertFile"
                           && conf->parms.size() == 1) {
			options.certfile = xwordexp(conf->parms[0]);
		} else if (conf->keyword == "KeyFile"
                           && conf->parms.size() == 1) {
			options.keyfile = xwordexp(conf->parms[0]);
		} else if (conf->keyword == "CipherList"
                           && conf->parms.size() == 1) {
			options.cipher_list = conf->parms[0];
		} else if (conf->keyword == "-include"
                           && conf->parms.size() == 1) {
			try {
				read_config_file(xwordexp(conf->parms[0]));
			} catch(const ConfigParser::ErrStream&) {
				break;
			}
		} else if (conf->keyword == "include"
                           && conf->parms.size() == 1) {
			try {
				read_config_file(xwordexp(conf->parms[0]));
			} catch(const ConfigParser::ErrStream&) {
				THROW(Err::ErrBase,
                                      "I/O error accessing include file: "
                                      + conf->parms[0]);
			}
		} else {
			THROW(Err::ErrBase,
                              "Error in config file: " + conf->keyword);
		}
	}
}

/** Parse options given on command line
 *
 */
void
parse_options(int argc, char * const *argv)
{
	int c;

	// expand default options. Not needed unless we change defaults
	options.certfile = xwordexp(options.certfile);
	options.keyfile = xwordexp(options.keyfile);

	// special options
	for (c = 1; c < argc - 1; c++) {
		if (!strcmp(argv[c], "--")) {
			break;
		} else if (!strcmp(argv[c], "--help")) {
			usage(0);
		} else if (!strcmp(argv[c], "--version")) {
			print_version();
			exit(0);
		} else if (!strcmp(argv[c], "-c")) {
			options.config = argv[c+1];
		}
	}
	try {
		read_config_file(options.config);
	} catch(const ConfigParser::ErrStream&) {
		THROW(Err::ErrBase,
                      "I/O error accessing config file: " + options.config);
	}
	int opt;
	while ((opt = getopt(argc, argv, "46c:C:hp:vV")) != -1) {
		switch (opt) {
                case '4':
                        options.af = AF_INET;
                        break;
                case '6':
                        options.af = AF_INET6;
                        break;
		case 'c':  // already handled above
			break;
		case 'C':
			options.cipher_list = optarg;
			break;
		case 'h':
			usage(0);
			break;
		case 'p':
			options.certfile = optarg;
			options.keyfile = optarg;
			break;
		case 'v':
			options.verbose++;
			break;
		case 'V':
			print_version();
			exit(0);
			break;
		default:
			usage(1);
		}
	}

	if (optind + 1 != argc) {
		usage(1);
	}
	options.host = argv[optind];
}

END_NAMESPACE(tlssh)


BEGIN_LOCAL_NAMESPACE()
using namespace tlssh;

/** exception-wrapped version of main()
 *
 */
int
main2(int argc, char * const argv[])
{
	parse_options(argc, argv);

        if (SIG_ERR == signal(SIGWINCH, sigwinch)) {
                THROW(Err::ErrSys, "signal(SIGWINCH)");
        }

	sock.ssl_set_cipher_list(options.cipher_list);
	sock.ssl_set_capath(options.servercapath);
	sock.ssl_set_cafile(options.servercafile);
	sock.ssl_set_certfile(options.certfile);
	sock.ssl_set_keyfile(options.keyfile);
	sock.ssl_set_crlfile(options.servercrl);
	if (options.verbose) {
		sock.set_debug(true);
	}

	Socket rawsock;

	rawsock.connect(options.af, options.host, options.port);
        rawsock.set_tcp_md5(options.tcp_md5);
        rawsock.set_tcp_md5_sock();
        rawsock.set_nodelay(true);
        rawsock.set_keepalive(true);
	sock.ssl_attach(rawsock);

	return new_connection();
}
END_LOCAL_NAMESPACE()

/** main() for tlssh client
 *
 */
int
main(int argc, char **argv)
{
	argv0 = argv[0];
	try {
		try {
			return main2(argc, argv);
		} catch(...) {
			reset_tio();
			throw;
		}
	} catch(const SSLSocket::ErrSSL &e) {
		std::cerr << e.what_verbose();
	} catch(const Err::ErrBase &e) {
                if (options.verbose) {
                        fprintf(stderr, "%s: %s\n",
                                argv0, e.what_verbose().c_str());
                } else {
                        fprintf(stderr, "%s: %s\n",
                                argv0, e.what());
                }
	} catch (const std::exception &e) {
		std::cerr << "tlssh std::exception: "
			  << e.what() << std::endl;
	} catch (const char *e) {
		std::cerr << "tlssh: const char*: "
			  << e << std::endl;
	} catch (...) {
		std::cerr << "tlssh: Unknown exception!" << std::endl;
	}
}

/* ---- Emacs Variables ----
 * Local Variables:
 * c-basic-offset: 8
 * indent-tabs-mode: nil
 * End:
 */
