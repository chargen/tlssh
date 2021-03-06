/**
 * @file src/util.cc
 * Random utility functions
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include<cstdio>
#include<utility>

#include"mywordexp.h"
#include"util2.h"
#include"xgetpwnam.h"
#include"errbase.h"
#include"fdwrap.h"

/**
 *
 */
Logger::~Logger()
{
        detach_all();
}


/**
 * Attach a second logger that will get all messages that pass
 * the filter of the current one.
 *
 * next:       Pointer to the other logger object.
 * ownership:  If true, the current object owns the pointer and will delete
 *             it when it's detached.
 */
void
Logger::attach(Logger *next, bool ownership)
{
        attached.push_back(std::make_pair(next, ownership));
}

/**
 *
 */
void
Logger::set_logmask(int m)
{
        logmask = m;
}

/**
 *
 */
void
Logger::vlog(int prio, const char *fmt, va_list ap) const
{
        std::string str(xvsprintf(fmt, ap));
        for (attached_t::const_iterator itr = attached.begin();
             itr != attached.end();
             ++itr) {
                itr->first->log(prio, str);
        }
        log(prio, str);
}


/**
 *
 */
void
Logger::detach_all()
{
        attached_t del_list(attached);
        attached.clear();
        for (attached_t::iterator itr = del_list.begin();
             itr != del_list.end();
             ++itr) {
                if (itr->second) {
                        delete itr->first;
                }
        }
}


/**
 *
 */
void
Logger::detach(Logger *l)
{
        attached_entry_t cur;
        attached_t::iterator next;
        for (attached_t::iterator itr = attached.begin();
             itr != attached.end();
             ) {
                next = itr;
                ++next;
                cur = *itr;
                if (cur.first == l) {
                        attached.erase(itr);
                        if (cur.second) {
                                delete cur.first;
                        }
                }
                itr = next;
        }
}


/**
 *
 */
FileLogger::FileLogger(const std::string &in_filename)
        :filename(in_filename),
         file(in_filename.c_str()),
         StreamLogger(file)
{
}


/**
 *
 */
StreamLogger::StreamLogger(std::ostream &os, const std::string timestring)
        :os(os),
         timestring(timestring)
{
}


/** log to a stream, with time string
 */
void
StreamLogger::log(int prio, const std::string &str) const
{
        if (!(get_logmask() & LOG_MASK(prio))) {
                return;
        }

        char tbuf[1024];
        struct tm tm;
        time_t t;
        time(&t);
        localtime_r(&t, &tm);
        if (!strftime(tbuf, sizeof(tbuf),
                      timestring.c_str(), &tm)) {
                strcpy(tbuf, "0000-00-00 00:00:00 UTC ");
        }
        // FIXME: check status of output stream, we may need \r\n, not just \n
        os << tbuf << str << std::endl;
}


/**
 *
 */
SysLogger::SysLogger(const std::string &inid, int fac)
        :id(inid)
{
        set_logmask(::setlogmask(0));
        openlog(id.c_str(), LOG_CONS | LOG_NDELAY | LOG_PID, fac);
}


/** return a sprintf()ed string
 */
std::string
xsprintf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
        std::string ret(xvsprintf(fmt, ap));
	va_end(ap);
        return ret;
}

/** return a vsprintf()ed string
 * doesn't use up the input
 */
std::string
xvsprintf(const char *fmt, va_list ap)
{
        int n;
        va_list ap_count;
        va_list ap_write;

        // find buffer size
        va_copy(ap_count, ap);
        FINALLY(
                n = vsnprintf(0, 0, fmt, ap_count);
                if (n < 0) {
                        THROW(Err::ErrBase, "snprintf()");
                }
                ,
                va_end(ap_count);
                );

        // fill buffer
        std::vector<char> buf(++n + 1, '\0');
        va_copy(ap_write, ap);
        FINALLY(
                vsnprintf(&buf[0], n, fmt, ap_write);
                ,
                va_end(ap_write);
                );
        return std::string(&buf[0]);
}


/** C++ wordexp wrapper
 *
 * @return The first 'hit' of wordexp
 */
std::string
xwordexp(const std::string &in)
{
	wordexp_t p;
        std::string ret;

	if (wordexp(in.c_str(), &p, 0)) {
		THROW(Err::ErrBase, "wordexp(" + in + ")");
	}

	if (p.we_wordc != 1) {
                wordfree(&p);
		THROW(Err::ErrBase, "wordexp(" + in + ") nmatch != 1");
	}
        try {
                ret = p.we_wordv[0];
        } catch(...) {
                wordfree(&p);
                throw;
        }
        wordfree(&p);
	return ret;
}


/** Tokenize a string, separated by space or tab
 *
 * @todo handle doublequotes
 */
std::vector<std::string>
tokenize(const std::string &s, size_t max_splits)
{
	std::vector<std::string> ret;
	size_t end;
	size_t start = 0;
        size_t splits = 0;
        std::string cur;

        for (;;) {
		// find beginning of word
		start = s.find_first_not_of(" \t", start);
		if (std::string::npos == start) {
			return ret;
		}

		// find end of word
		end = s.find_first_of(" \t", start);
		if (std::string::npos == end) {
			ret.push_back(trim(s.substr(start), "\""));
			break;
		}
                if (s[start] == '"') {
                        start++;
                        end = s.find_first_of("\"", start);
                        if (std::string::npos == end) {
                                ret.push_back(trim(s.substr(start)));
                                break;
                        }
                        ret.push_back(trim(s.substr(start, end - start)));
                } else {
                        ret.push_back(trim(s.substr(start, end - start)));
                }
                if (++splits == max_splits) {
                        ret.push_back(trim(s.substr(end)));
                        break;
                }
		start = end;
	}
	return ret;
}


/** cur off spaces and tabs at beginning and end of string
 *
 * @return trimmed string
 */
std::string
trim(const std::string &str, const std::string sep)
{
	size_t startpos = str.find_first_not_of(sep);
	if (std::string::npos == startpos) {
		return "";
	}

	size_t endpos = str.find_last_not_of(sep);

	return str.substr(startpos, endpos-startpos+1);
}


/** c++ wrapper of getpwnam_r()
 *
 * @param[in] name    Username to lookup
 * @param[in] buffer  std::vector<char> owned by the caller that can't be freed
 *                    until the returned struct will no longer be used.
 *
 * @return passwd struct for user
 */
struct passwd
xgetpwnam(const std::string &name, std::vector<char> &buffer)
{
	buffer.reserve(1024);
	struct passwd pw;
	struct passwd *ppw = 0;
	if (xgetpwnam_r(name.c_str(), &pw, &buffer[0], buffer.capacity(), &ppw)
	    || !ppw) {
                // throw name, it can't accidentally be a password since we
                // don't have passwords
		THROW(Err::ErrBase, "xgetpwnam(" + name + ")");
	}

	return pw;
}


/** return pointer to the first character after the last "/"
 */
char*
gnustyle_basename(const char *fn)
{
        char *p = (char*)strrchr(fn, '/');
        return p ? p + 1 : (char *)fn;
}

/* ---- Emacs Variables ----
 * Local Variables:
 * c-basic-offset: 8
 * indent-tabs-mode: nil
 * End:
 */
