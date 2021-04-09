/* Functions for exec'ing processes
 *
 * Copyright (c) 2008-2010  Claudio Matsuoka <cmatsuoka@gmail.com>
 * Copyright (c) 2008-2021  Joachim Wiberg <troglobit@gmail.com>
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

#include <ctype.h>		/* isdigit() */
#include <dirent.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <sys/ttydefaults.h>	/* Not included by default in musl libc */
#include <termios.h>
#include <lite/lite.h>

#include "finit.h"
#include "cgroup.h"
#include "conf.h"
#include "helpers.h"
#include "sig.h"
#include "util.h"
#include "utmp-api.h"

#define NUM_ARGS    16


/* Wait for process completion, returns status of waitpid(2) syscall */
int complete(char *cmd, int pid)
{
	int status = 0;

	if (waitpid(pid, &status, 0) == -1) {
		if (errno == EINTR)
			_e("Caught unblocked signal waiting for %s, aborting", cmd);
		else if (errno == ECHILD)
			_e("Caught SIGCHLD waiting for %s, aborting", cmd);
		else
			_e("Failed starting %s, error %d: %s", cmd, errno, strerror (errno));

		return -1;
	}

	return status;
}

int run(char *cmd)
{
	int status, result, i = 0;
	char *args[NUM_ARGS + 1], *arg, *backup;
	pid_t pid;

	/* We must create a copy that is possible to modify. */
	backup = arg = strdup(cmd);
	if (!arg)
		return 1; /* Failed allocating a string to be modified. */

	/* Split command line into tokens of an argv[] array. */
	args[i++] = strsep(&arg, "\t ");
	while (arg && i < NUM_ARGS) {
		/* Handle run("su -c \"dbus-daemon --system\" messagebus");
		 *   => "su", "-c", "\"dbus-daemon --system\"", "messagebus" */
		if (*arg == '\'' || *arg == '"') {
			char *p, delim[2] = " ";

			delim[0]  = arg[0];
			args[i++] = arg++;
			strsep(&arg, delim);
			 p     = arg - 1;
			*p     = *delim;
			*arg++ = 0;
		} else {
			args[i++] = strsep(&arg, "\t ");
		}
	}
	args[i] = NULL;

	if (i == NUM_ARGS && arg) {
		_e("Command too long: %s", cmd);
		free(backup);
		errno = EOVERFLOW;
		return 1;
	}

	pid = fork();
	if (0 == pid) {
		FILE *fp;

		/* Reset signal handlers that were set by the parent process */
		sig_unblock();
		setsid();

		/* Always redirect stdio for run() */
		fp = fopen("/dev/null", "w");
		if (fp) {
			int fd = fileno(fp);

			dup2(fd, STDIN_FILENO);
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
		}

		sig_unblock();
		execvp(args[0], args);

		_exit(1); /* Only if execv() fails. */
	} else if (-1 == pid) {
		_pe("%s", args[0]);
		free(backup);

		return -1;
	}

	status = complete(args[0], pid);
	if (-1 == status) {
		free(backup);
		return 1;
	}

	result = WEXITSTATUS(status);
	if (WIFEXITED(status)) {
		_d("Started %s and ended OK: %d", args[0], result);
	} else if (WIFSIGNALED(status)) {
		_d("Process %s terminated by signal %d", args[0], WTERMSIG(status));
		if (!result)
			result = 1; /* Must alert callee that the command did complete successfully.
				     * This is necessary since not all programs trap signals and
				     * change their return code accordingly. --Jocke */
	}

	free(backup);

	return result;
}

int run_interactive(char *cmd, char *fmt, ...)
{
	int status, oldout = 1, olderr = 2;
	char line[LINE_SIZE];
	FILE *fp;

	if (!cmd) {
		errno = EINVAL;
		return 1;
	}

	if (fmt) {
		va_list ap;

		va_start(ap, fmt);
		printv(fmt, ap);
		va_end(ap);
	}

	/* Redirect output from cmd to a tempfile */
	fp = tempfile();
	if (fp && !debug) {
		oldout = dup(STDOUT_FILENO);
		olderr = dup(STDERR_FILENO);
		dup2(fileno(fp), STDOUT_FILENO);
		dup2(fileno(fp), STDERR_FILENO);
	}

	/* Run cmd ... */
	status = run(cmd);

	/* Restore stderr/stdout */
	if (fp && !debug) {
		if (oldout >= 0) {
			dup2(oldout, STDOUT_FILENO);
			close(oldout);
		}
		if (olderr >= 0) {
			dup2(olderr, STDERR_FILENO);
			close(olderr);
		}
	}

	if (fmt)
		print_result(status);

	/* Dump any results of cmd on stderr after we've printed [ OK ] or [FAIL]  */
	if (fp && !debug) {
		size_t len, written;

		rewind(fp);
		do {
			len     = fread(line, 1, sizeof(line), fp);
			written = fwrite(line, len, sizeof(char), stderr);
		} while (len > 0 && written == len);
	}

	if (fp)
		fclose(fp);

	return status;
}

int exec_runtask(char *cmd, char *args[])
{
	size_t i;
	char buf[1024] = "";
	char *argv[4] = {
		"sh",
		"-c",
		buf,
		NULL
	};

	strlcat(buf, cmd, sizeof(buf));
	for (i = 1; args[i]; i++) {
		strlcat(buf, " ", sizeof(buf));
		strlcat(buf, args[i], sizeof(buf));
	}
	logit(LOG_DEBUG, "Calling %s %s", _PATH_BSHELL, buf);
	_d("Calling %s %s", _PATH_BSHELL, buf);

	return execvp(_PATH_BSHELL, argv);
}

static void prepare_tty(char *tty, speed_t speed, char *procname, struct rlimit rlimit[])
{
	struct sigaction sa;
	struct termios term;
	char name[80];
	int fd;

	/* Detach from initial controlling TTY and become session leader */
	vhangup();
	setsid();

	fd = open(tty, O_RDWR);
	if (fd < 0) {
		logit(LOG_ERR, "Failed opening %s: %s", tty, strerror(errno));
		_exit(1);
	}

	dup2(fd, STDIN_FILENO);
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);
	close(fd);

	if (ioctl(STDIN_FILENO, TIOCSCTTY, 1) < 0)
		logit(LOG_WARNING, "Failed TIOCSCTTY on %s: %s", tty, strerror(errno));

	/*
	 * Reset to sane defaults in case of messup from prev. session
	 */
	stty(STDIN_FILENO, speed);

	/*
	 * Disable ISIG (INTR, QUIT, SUSP) before handing over to getty.
	 * It is up to the getty process to allow them again.
	 */
	if (!tcgetattr(STDIN_FILENO, &term)) {
		term.c_lflag    &= ~ISIG;
		term.c_cc[VEOF]  = _POSIX_VDISABLE;
		term.c_cc[VINTR] = _POSIX_VDISABLE;
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &term);
	}

	/* Reset signal handlers that were set by the parent process */
	sig_unblock();

	/*
	 * Ignore a few signals, needed to prevent Ctrl-C at login:
	 * prompt and to prevent QUIT from dumping core.
	 */
	sigemptyset(&sa.sa_mask);
	sa.sa_flags   = SA_RESTART;
	sa.sa_handler = SIG_IGN;
	sigaction(SIGHUP,  &sa, NULL);
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);

	/* Set configured limits */
	for (int i = 0; i < RLIMIT_NLIMITS; i++) {
		if (setrlimit(i, &rlimit[i]) == -1)
			logit(LOG_WARNING, "%s: rlimit: Failed setting %s", tty, rlim2str(i));
	}

	/* Finit is responsible for the UTMP INIT_PROCESS record */
	utmp_set_init(tty, 0);
	if (!strncmp("/dev/", tty, 5))
		tty += 5;
	snprintf(name, sizeof(name), "%s %s", procname, tty);
	prctl(PR_SET_NAME, name, 0, 0, 0);
}

static int activate_console(int noclear, int nowait)
{
	struct termios orig;
	int ret = 0;

	if (nowait || rescue)
		return 1;

	if (!noclear)
		dprint(STDERR_FILENO, "\e[r\e[H\e[J", 9);

	/* Disable ECHO, XON/OFF while waiting for <CR> */
	if (!tcgetattr(STDIN_FILENO, &orig)) {
		struct termios c = orig;

		c.c_iflag &= ~(BRKINT|ICRNL|INPCK|ISTRIP|IXON|IXOFF);
		c.c_oflag &= ~(OPOST);
		c.c_cflag |=  (CS8);
		c.c_lflag &= ~(ECHO|ICANON|IEXTEN|ISIG);
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &c);
	}

	while (!fexist(SYNC_SHUTDOWN)) {
		char c;
		static const char clr[] = "\r\e[2K";
		static const char cup[] = "\e[A";
		static const char msg[] = "\nPlease press Enter to activate this console.";

		if (fexist(SYNC_STOPPED)) {
			sleep(5);
			continue;
		}

		dprint(STDERR_FILENO, clr, strlen(clr));
		dprint(STDERR_FILENO, msg, strlen(msg));
		while (read(STDIN_FILENO, &c, 1) == 1 && c != '\r')
			continue;

		if (fexist(SYNC_STOPPED))
			continue;

		dprint(STDERR_FILENO, clr, strlen(clr));
		dprint(STDERR_FILENO, cup, strlen(cup));
		ret = 1;
		break;
	}

	/* Restore TTY */
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig) == -1)
		ret = 0;

	return ret;
}

/*
 * Start a getty on @tty
 *
 * At the login: prompt, no signals are allowed, both Ctrl-C and Ctrl-D
 * should be disabled.  Ctrl-S and Ctrl-Q are optional, but most getty
 * allow them.
 *
 * Prior to getty is called and login: is printed, Finit may display the
 * "Please press Enter ..." if @nowait is unset.  This mode must be RAW,
 * only accepting <CR> and not echoing anything, this also means no
 * signals are allowed.  For ease of implementation Finit will call the
 * stty() function to reset the TTY and then force RAW mode until a <CR>
 * has been received.  This is handled in the same fashion for both this
 * function and run_getty2(), which is used for external getty.
 *
 * When handing over to /bin/login, Ctrl-C and Ctrl-D must be enabled
 * since /bin/login usually only disables ECHO until a password line has
 * been entered.  Upon starting the user's $SHELL the ISIG flag is reset
 */
pid_t run_getty(char *tty, char *baud, char *term, int noclear, int nowait, struct rlimit rlimit[])
{
	pid_t pid;

	pid = fork();
	if (!pid) {
		speed_t speed;
		int rc = 1;

		speed = stty_parse_speed(baud);
		prepare_tty(tty, speed, "tty", rlimit);
		if (activate_console(noclear, nowait)) {
			logit(LOG_INFO, "Starting built-in getty on %s, speed %u", tty, speed);
			rc = getty(tty, speed, term, NULL);
		}

		_exit(rc);
	}

	cgroup_user("getty", pid);

	return pid;
}

pid_t run_getty2(char *tty, char *cmd, char *args[], int noclear, int nowait, struct rlimit rlimit[])
{
	pid_t pid;

	pid = fork();
	if (!pid) {
		int rc = 1;

		/* Dunno speed, tell stty() to not mess with it */
		prepare_tty(tty, B0, "getty", rlimit);
		if (activate_console(noclear, nowait)) {
			logit(LOG_INFO, "Starting external getty on %s, speed %u", tty, B0);
			rc = execv(cmd, args);
		}

		vhangup();
		_exit(rc);
	}

	cgroup_user("getty", pid);

	return pid;
}

pid_t run_sh(char *tty, int noclear, int nowait, struct rlimit rlimit[])
{
	pid_t pid;

	pid = fork();
	if (!pid) {
		int rc = 1;

		prepare_tty(tty, B0, "finit-sh", rlimit);
		if (activate_console(noclear, nowait))
			rc = sh(tty);

		_exit(rc);
	}

	cgroup_user("root", pid);

	return pid;
}

int run_parts(char *dir, char *cmd)
{
	struct dirent **e;
	int i, num;

	num = scandir(dir, &e, NULL, alphasort);
	if (num < 0) {
		_d("No files found in %s, skipping ...", dir);
		return -1;
	}

	for (i = 0; i < num; i++) {
		const char *name = e[i]->d_name;
		char path[strlen(dir) + strlen(name) + 2];
		struct stat st;
		char *argv[4] = {
			"sh",
			"-c",
			path,
			NULL
		};
		pid_t pid = 0;
		int status;
		int exit_status;

		paste(path, sizeof(path), dir, name);
		if (stat(path, &st)) {
			_d("Failed stat(%s): %s", path, strerror(errno));
			continue;
		}

		if (!S_ISEXEC(st.st_mode) || S_ISDIR(st.st_mode)) {
			_d("Skipping %s ...", path);
			continue;
		}

		/* If the callee didn't supply a run_parts() argument */
		if (!cmd) {
			/* Check if S<NUM>service or K<NUM>service notation is used */
			_d("Checking if %s is a sysvinit startstop script ...", name);
			if (name[0] == 'S' && isdigit(name[1]))
				strlcat(path, " start", sizeof(path));
			else if (name[0] == 'K' && isdigit(name[1]))
				strlcat(path, " stop", sizeof(path));
		} else {
			strlcat(path, cmd, sizeof(path));
		}

		print_desc("Calling ", path);
		pid = fork();
		if (!pid) {
			sig_unblock();
			return execvp(_PATH_BSHELL, argv);
		}

		status = complete(path, pid);
		exit_status = WEXITSTATUS(status);
		if (WIFEXITED(status) && exit_status)
			_w("%s exited with status %d", path, exit_status);
		else if (WIFSIGNALED(status))
			_w("%s terminated by signad %d", path, WTERMSIG(status));
		print_result(status);
	}

	while (num--)
		free(e[num]);
	free(e);

	return 0;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
