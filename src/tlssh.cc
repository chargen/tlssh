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
 * Copyright (c) 2010 Thomas Habets. All rights reserved.
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
#include<signal.h>
#include<sys/ioctl.h>
#include<sys/socket.h>
#include<arpa/inet.h>

#include<iostream>
#include<fstream>

#include"mywordexp.h"
#include"tlssh.h"
#include"util2.h"
#include"sslsocket.h"
#include"configparser.h"

using namespace tlssh_common;

Logger *logger;

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
        bool terminal;
        std::string remote_command;
        bool check_certdb;
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
 terminal:     true,
 remote_command: "",
 check_certdb: true,
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
        sock.full_write("version " + protocol_version + "\n");
        sock.full_write("env TERM " + terminal_type() + "\n");
        if (!options.terminal) {
                sock.full_write("terminal off\n");
        }
        sock.full_write("\n");

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
        printf("%s [ -46hsvV ] "
	       "[ -c <config> ] "
	       "[ -C <cipher-list> ] <hostname> [command]"
               "\n"
               "\t[ -p <cert+keyfile> ]"
	       "\n"
	       "\t-c <config>          Config file (default %s)\n"
               "\t-C <cipher-list>     Acceptable ciphers\n"
               "\t                     (default %s)\n"
	       "\t-h, --help           Help\n"
	       "\t-p <cert+keyfile>    Load login cert+key from file\n"
	       "\t-s                   Don't check cert database cache.\n"
	       "\t-V, --version        Print version and exit\n"
	       "\t--copying            Print license and exit\n"
	       , argv0,
	       DEFAULT_CONFIG.c_str(), DEFAULT_CIPHER_LIST.c_str());
	exit(err);
}

/** Read config file
 *
 * @param[in] fn Config file name
 */
void
read_config_file(const std::string &fn)
{
	std::ifstream fi(fn.c_str());
        if (!fi.is_open()) {
                return;
        }
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

		} else if (conf->keyword == "L3Protocol"
                           && conf->parms.size() == 1) {
                        if (conf->parms[1] == "IPv4") {
                                options.af = AF_INET;
                        } else if (conf->parms[1] == "IPv6") {
                                options.af = AF_INET6;
                        } else {
                                THROW(Err::ErrBase,
                                      "Unknown L3Protocol: "
                                      + conf->parms[1]
                                      + ", must be IPv4 or IPv6");
                        }

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
 * Overrides config file.
 */
void
parse_options(int argc, char * const *argv)
{
	int c;

	// expand default options. Not needed unless we change defaults
	options.certfile = xwordexp(options.certfile);
	options.keyfile = xwordexp(options.keyfile);

	// special options
	for (c = 1; c < argc; c++) {
		if (!strcmp(argv[c], "--")
                    || (*argv[c] != '-')) {
                        // end of options
			break;
		} else if (!strcmp(argv[c], "-C")
                           || !strcmp(argv[c], "-p")) {
                        // skip parameters for options that have them
			c++;
		} else if (!strcmp(argv[c], "--help")) {
			usage(0);
		} else if (!strcmp(argv[c], "--version")) {
			print_version();
			exit(0);
		} else if (!strcmp(argv[c], "--copying")) {
			print_copying();
			exit(0);
		} else if (!strcmp(argv[c], "-c")) {
                        if (c != argc - 1) {
                                options.config = argv[++c];
                        }
		}
	}
	try {
		read_config_file(options.config);
	} catch(const ConfigParser::ErrStream&) {
                THROW(Err::ErrBase,
                      "I/O error accessing config file: " + options.config);
	}
	int opt;
	while ((opt = getopt(argc, argv, "+46c:C:hp:svV")) != -1) {
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
                case 's':
                        options.check_certdb = false;
                        break;
		case 'v':
                        if (++options.verbose > 1) {
                                logger->set_logmask(logger->get_logmask()
                                                    | LOG_MASK(LOG_DEBUG));
                        }
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
                c = optind;
                options.remote_command = argv[c++];
                for (; c < argc; c++) {
                        options.remote_command += std::string(" ") + argv[c];
                }
                options.terminal = false;
	}
	options.host = argv[optind];
}

/**
 *
 */
std::string
certdb_filename()
{
        const char *home;
        home = getenv("HOME");
        if (!home) {
                home = "";
        }
        return std::string(home) + "/.tlssh/certdb";
}

bool
certdb_check()
{
        std::auto_ptr<X509Wrap> x509(sock.get_cert());
        std::ifstream f(certdb_filename().c_str());

        while (f.good()) {
                std::string line;
                getline(f, line);
                std::vector<std::string> tokens(tokenize(trim(line)));

                // host cert [ca path... ]
                if (tokens.size() < 2) {
                        logger->debug("Parse error in certdb");
                        continue;
                }

                // check for wrong hostname
                if (tokens[0] != options.host) {
                        continue;
                }

                // check for wrong cert
                if (tokens[1] != x509->get_fingerprint()) {
                        continue;
                }

                // FIXME: check that tokens[2].. match current CA path
                return true;
        }
        return false;
}

/**
 *
 */
void
do_certdatabase()
{
        if (certdb_check()) {
                // same cert as last time.
                return;
        }

        std::auto_ptr<X509Wrap> x509(sock.get_cert());

        fprintf(stderr,
                "It appears that you have never logged into this host before"
                " (when it had\nthis cert):\n    %s\n"
                "Its certificate fingerprint is:\n    %s\n"
                "and the cert was issued by:\n    %s\n"
                "Does this sound reasonable (yes/no)? ",
                options.host.c_str(),
                x509->get_fingerprint().c_str(),
                x509->get_issuer_common_name().c_str());

        std::string ans;
        getline(std::cin, ans);
        if (!(ans == "y"
              || ans == "yes")) {
                THROW(Err::ErrBase, "Unacceptable server certificate");
        }

        logger->debug("First time logging into %s, saving cert fingerprint",
                      options.host.c_str());

        std::fstream of(certdb_filename().c_str(),
                        std::ios_base::out |std::ios_base::app);
        if (!of.good()) {
                logger->warning("Can't open cert DB file!");
                return;
        }

        // FIXME: save the whole CA path
        of << options.host
           << " " << x509->get_fingerprint() << std::endl;
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

        sock.ssl_connect(options.host);

        if (options.check_certdb) {
                do_certdatabase();
        }

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

        logger = new StreamLogger(std::cerr,
                                  std::string(argv0) + ": ");
        logger->set_logmask(logger->get_logmask() & ~LOG_MASK(LOG_DEBUG));

	try {
		try {
			return main2(argc, argv);
		} catch(...) {
			reset_tio();
			throw;
		}
	} catch(const SSLSocket::ErrSSL &e) {
		std::cerr << "tlssh: " << e.what_verbose();
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
	} catch (...) {
		std::cerr << "tlssh: Unknown exception!" << std::endl;
                throw;
	}
}

/* ---- Emacs Variables ----
 * Local Variables:
 * c-basic-offset: 8
 * indent-tabs-mode: nil
 * End:
 */
