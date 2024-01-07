/* Initialize and serve a login terminal
 *
 * Copyright (c) 2016-2024  Joachim Wiberg <troglobit@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "config.h"		/* Generated by configure script */

#include <err.h>
#include <errno.h>
#include <limits.h>		/* LOGIN_NAME_MAX */
#include <paths.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <sys/ttydefaults.h>	/* Not included by default in musl libc */
#include <termios.h>
#include <time.h>
#include <unistd.h>		/* sysconf() */

#include "finit.h"
#include "helpers.h"
#include "utmp-api.h"

#ifndef _PATH_LOGIN
#define _PATH_LOGIN  "/bin/login"
#endif

#ifndef LOGIN_NAME_MIN
#define LOGIN_NAME_MIN 64
#endif

struct osrel {
	char name[32];
	char pretty[64];
	char id[32];
	char version[32];
	char version_id[32];
	char home_url[128];
	char doc_url[128];
	char bug_url[128];
	char support_url[128];
};

static long logname_len = 32;	/* useradd(1) limit at 32 chars */
static int  passenv     = 0;	/* Set /bin/login -p or not     */

static void get_val(char *line, const char *key, char *buf, size_t len)
{
	char *val;

	if ((val = fgetval(line, key, "="))) {
		strlcpy(buf, val, len);
		free(val);
	}
}

/*
 * Parse os-release
 */
static int osrel(struct osrel *rel)
{
	char codename[30];
	char *line;
	FILE *fp;

	fp = fopen("/etc/os-release", "r");
	if (!fp) {
		fp = fopen("/usr/lib/os-release", "r");
		if (!fp)
			return -1;
	}

	memset(rel->version, 0, sizeof(rel->version));
	memset(codename, 0, sizeof(codename));

	while ((line = fparseln(fp, NULL, NULL, NULL, 0))) {
		get_val(line, "NAME", rel->name, sizeof(rel->name));
		get_val(line, "PRETTY_NAME", rel->pretty, sizeof(rel->pretty));
		get_val(line, "ID", rel->id, sizeof(rel->id));
		get_val(line, "VERSION", rel->version, sizeof(rel->version));
		get_val(line, "VERSION_ID", rel->version_id, sizeof(rel->version_id));
		get_val(line, "VERSION_CODENAME", codename, sizeof(codename));
		get_val(line, "HOME_URL", rel->home_url, sizeof(rel->home_url));
		get_val(line, "DOCUMENTATION_URL", rel->doc_url, sizeof(rel->doc_url));
		get_val(line, "SUPPORT_URL", rel->support_url, sizeof(rel->support_url));
		get_val(line, "BUG_REPORT_URL", rel->bug_url, sizeof(rel->bug_url));

		free(line);
	}
	fclose(fp);

	/*
	 * Many distros don't set VERSION or VERSION_ID for their
	 * rolling, or unstable, branches.  However, most set the
	 * version code name, so we use that as VERSION.
	 */
	if (!rel->version[0] && codename[0]) {
		if (codename[0] == '(')
			strlcpy(rel->version, codename, sizeof(rel->version));
		else
			snprintf(rel->version, sizeof(rel->version), "(%s)", codename);
	}

	return 0;
}

/*
 * Read one character from stdin.
 */
static int readch(void)
{
	char ch1;
	int st;

	st = read(STDIN_FILENO, &ch1, 1);
	if (st <= 0)
		return -1;

	return ch1 & 0xFF;
}

/*
 * Parse and display a line from /etc/issue
 *
 * Includes Finit-specific extensions to display information from
 * /etc/os-release instead of relying on legacy uname.  Activated
 * only if /etc/os-release or /usr/lib/os-release exist.
 *
 * https://www.unix.com/man-page/Linux/8/getty/
 * https://www.systutorials.com/docs/linux/man/8-mingetty/
 */
static void parseln(char *line, struct utsname *uts, struct osrel *rel, char *tty, int compat)
{
	char buf[32] = { 0 };
	char *s, *s0;
	time_t now;

	s0 = line;
	for (s = line; *s != 0; s++) {
		if (*s == '\\') {
			if ((s - s0) > 0)
				dprint(STDOUT_FILENO, s0, s - s0);
			s0 = s + 2;
			switch (*++s) {
			case 'B':
				if (!compat)
					dprint(STDOUT_FILENO, rel->bug_url, 0);
				break;
			case 'D':
				if (!compat)
					dprint(STDOUT_FILENO, rel->doc_url, 0);
				break;
			case 'H':
				if (!compat)
					dprint(STDOUT_FILENO, rel->home_url, 0);
				break;
			case 'I':
				if (!compat)
					dprint(STDOUT_FILENO, rel->id, 0);
				break;
			case 'l':
				dprint(STDOUT_FILENO, tty, 0);
				break;
			case 'm':
				dprint(STDOUT_FILENO, uts->machine, 0);
				break;
			case 'N':
				if (!compat)
					dprint(STDOUT_FILENO, rel->name, 0);
				break;
			case 'n':
				dprint(STDOUT_FILENO, uts->nodename, 0);
				break;
#ifdef _GNU_SOURCE
			case 'o':
				dprint(STDOUT_FILENO, uts->domainname, 0);
				break;
#endif
			case 'r':
				if (compat)
					dprint(STDOUT_FILENO, uts->release, 0);
				else
					dprint(STDOUT_FILENO, rel->version_id, 0);
				break;
			case 'S':
				if (!compat)
					dprint(STDOUT_FILENO, rel->support_url, 0);
				break;
			case 's':
				if (compat)
					dprint(STDOUT_FILENO, uts->sysname, 0);
				else
					dprint(STDOUT_FILENO, rel->pretty, 0);
				break;
			case 't':
				now = time(NULL);
				ctime_r(&now, buf);
				dprint(STDOUT_FILENO, chomp(buf), 0);
				break;
			case 'v':
				if (compat)
					dprint(STDOUT_FILENO, uts->version, 0);
				else
					dprint(STDOUT_FILENO, rel->version, 0);
				break;
			case 0:
				goto leave;
			default:
				s0 = s - 1;
			}
		}
	}

leave:
	if ((s - s0) > 0)
		dprint(STDOUT_FILENO, s0, s - s0);
}

/*
 * Parse and display /etc/issue
 */
static void issue(char *tty)
{
	char buf[BUFSIZ] = "Welcome to \\s \\v \\n \\l\n\n";
	struct utsname uts;
	struct osrel rel;
	int compat;
	FILE *fp;

	/*
	 * Get data about this machine.
	 */
	uname(&uts);
	compat = osrel(&rel);

	fp = fopen("/etc/issue", "r");
	if (fp) {
		while (fgets(buf, sizeof(buf), fp))
			parseln(buf, &uts, &rel, tty, compat);

		fclose(fp);
	} else {
		parseln(buf, &uts, &rel, tty, compat);
	}

	parseln("\\n login: ", &uts, &rel, tty, compat);
}

/*
 * Handle the process of a GETTY.
 */
static int get_logname(char *tty, char *name, size_t len)
{
	char *np;
	int ch;

	/*
	 * Display prompt.
	 */
	ch = ' ';
	*name = '\0';
	while (ch != '\n') {
		issue(tty);

		np = name;
		while ((ch = readch()) != '\n') {
			if (ch < 0)
				return 1;

			if (np < name + len)
				*np++ = ch;
		}

		*np = '\0';
		if (*name == '\0')
			ch = ' ';	/* blank line typed! */
	}

	name[len - 1] = 0;
	return 0;
}

/*
 * Start login(1) with the current username as its argument.  It replies
 * to the calling user by typing "Password: " ...
 */
static int exec_login(char *name)
{
	struct stat st;

	if (passenv)
		execl(_PATH_LOGIN, _PATH_LOGIN, "-p", name, NULL);
	else
		execl(_PATH_LOGIN, _PATH_LOGIN, name, NULL);

	/*
	 * Failed to exec login, should not happen on normal systems.
	 * Try a starting a rescue shell instead.
	 */
	if (fstat(0, &st) == 0 && S_ISCHR(st.st_mode)) {
		warnx("Failed exec %s, attempting fallback to %s ...", _PATH_LOGIN, _PATH_SULOGIN);
		execl(_PATH_SULOGIN, _PATH_SULOGIN, NULL);

		warnx("Failed exec %s, attempting fallback to %s ...", _PATH_SULOGIN, _PATH_BSHELL);
		execl(_PATH_BSHELL, _PATH_BSHELL, NULL);
	}

	return 1;	/* We shouldn't get here ... */
}

static int getty(char *tty, speed_t speed, char *term, char *user)
{
	const char cln[] = "\r\e[K\n";
	char name[logname_len + 1]; /* +1 for NUL termination */
	pid_t sid;

	/*
	 * Clean up tty name.
	 */
	if (!strncmp(tty, _PATH_DEV, strlen(_PATH_DEV)))
		tty += 5;

	/* The getty process is responsible for the UTMP login record */
	utmp_set_login(tty, NULL);

	/* Replace "Please press enter ..." with login: */
	dprint(STDERR_FILENO, cln, strlen(cln));

restart:
	stty(STDIN_FILENO, speed);
	if (!user) {
		if (get_logname(tty, name, sizeof(name)))
			goto restart;
	} else
		strlcpy(name, user, sizeof(name));

	/* check current session associated with our tty */
	sid = tcgetsid(0);
	if (sid < 0 || getpid() != sid) {
		if (ioctl(STDIN_FILENO, TIOCSCTTY, 1) == -1)
			err(1, "failed stealing controlling TTY");
	}

	if (term && term[0])
		setenv("TERM", term, 1);

	return exec_login(name);
}

static int usage(int rc)
{
	warnx("usage: getty [-h?p] tty [speed [term]]");
	return rc;
}

int main(int argc, char *argv[])
{
	char *tty, *speed = "0", *term = NULL;
	int c;

	while ((c = getopt(argc, argv, "h?p")) != EOF) {
		switch(c) {
		case 'h':
		case '?':
			return usage(0);
		case 'p':
			passenv = 1;
			break;
		default:
			return usage(1);
		}
	}

	if (optind >= argc)
		return usage(1);

	tty = argv[optind++];
	if (optind < argc)
		speed = argv[optind++];
	if (optind < argc)
		term = argv[optind++];

	logname_len = sysconf(_SC_LOGIN_NAME_MAX);
	if (logname_len == -1i)
	       logname_len = LOGIN_NAME_MAX;
	if (logname_len < LOGIN_NAME_MIN)
		logname_len = LOGIN_NAME_MIN;

	return getty(tty, atoi(speed), term, NULL);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
