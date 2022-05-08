/* Parser for /etc/finit.conf and /etc/finit.d/<SVC>.conf
 *
 * Copyright (c) 2012-2022  Joachim Wiberg <troglobit@gmail.com>
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

#include <ctype.h>
#include <dirent.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/resource.h>
#ifdef _LIBITE_LITE
# include <libite/lite.h>
# include <libite/queue.h>	/* BSD sys/queue.h API */
#else
# include <lite/lite.h>
# include <lite/queue.h>	/* BSD sys/queue.h API */
#endif
#include <time.h>
#include <glob.h>

#include "finit.h"
#include "cond.h"
#include "iwatch.h"
#include "private.h"
#include "service.h"
#include "tty.h"
#include "helpers.h"
#include "util.h"

#define BOOTSTRAP (runlevel == 0)
#define MATCH_CMD(l, c, x) \
	(!strncasecmp(l, c, strlen(c)) && (x = (l) + strlen(c)))

int logfile_size_max = 200000;	/* 200 kB */
int logfile_count_max = 5;

struct env_entry {
	TAILQ_ENTRY(env_entry) link;
	char *name;
};
static TAILQ_HEAD(, env_entry) env_list = TAILQ_HEAD_INITIALIZER(env_list);

struct rlimit initial_rlimit[RLIMIT_NLIMITS];
struct rlimit global_rlimit[RLIMIT_NLIMITS];

char cgroup_current[16]; /* cgroup.NAME sets current cgroup for a set of services */

struct conf_change {
	TAILQ_ENTRY(conf_change) link;
	char *name;
};

static struct iwatch iw_conf;
static uev_t etcw;

static TAILQ_HEAD(, conf_change) conf_change_list = TAILQ_HEAD_INITIALIZER(conf_change_list);

static int  parse_conf(char *file, int is_rcsd);
static void drop_changes(void);

int get_bool(char *arg, int default_value)
{
	if (!arg)
		goto fallback;

	if (string_compare(arg, "true") || string_compare(arg, "on") || string_compare(arg, "1"))
		return 1;
	if (string_compare(arg, "false") || string_compare(arg, "off") || string_compare(arg, "0"))
		return 0;
fallback:
	return default_value;
}

/*
 * finit.debug  = [on,off]
 * finit.fstab  = /path/to/etc/fstab.aternative
 * finit.status = [on,off]     (compat finit.show_status)
 * finit.status_style = [old,classic,modern]
 */
static void parse_finit_opts(char *opt)
{
	char *arg;

	arg = strchr(opt, '=');
	if (arg)
		*arg++ = 0;

	if (string_compare(opt, "debug")) {
		debug = get_bool(arg, 1);
		return;
	}

	if (string_compare(opt, "fstab")) {
		fstab = arg;
		return;
	}

	if (string_compare(opt, "status_style")) {
		if (!arg) {
			_e("status_style option requires an argument, skipping.");
			return;
		}

		if (string_compare(arg, "old") || string_compare(arg, "classic"))
			show_progress(PROGRESS_CLASSIC);
		else
			show_progress(PROGRESS_MODERN);
		return;
	}

	if (string_compare(opt, "status") || string_compare(opt, "show_status")) {
		show_progress(get_bool(arg, 1) ? PROGRESS_DEFAULT : PROGRESS_SILENT);
		return;
	}
}

/*
 * When runlevel (single integer) is given on the command line,
 * it overrides the runlevel in finit.conf and the built-in
 * default (from configure).  It do however have to pass the
 * same sanity checks.
 */
static int parse_runlevel(char *arg)
{
	const char *err = NULL;
	char *ptr = arg;
	long long num;

	/* Sanity check the token is just digit(s) */
	while (*ptr) {
		if (!isdigit(*ptr++))
			return 0;
	}

	num = strtonum(arg, 1, 9, &err);
	if (err || num == 6) {
		_d("Not a valid runlevel (%s), valid levels are [1-9], excluding 6, skipping.", arg);
		return 0;
	}

	return (int)num;
}

static void parse_arg(char *arg)
{
	if (!strncmp(arg, "finit.", 6)) {
		parse_finit_opts(&arg[6]);
		return;
	}

	if (string_compare(arg, "rescue") || string_compare(arg, "recover")) {
		rescue = 1;
		return;
	}

	if (string_compare(arg, "single") || string_compare(arg, "S")) {
		single = 1;
		return;
	}

	/* Put any new command line options before this line. */

	cmdlevel = parse_runlevel(arg);
}

#ifdef KERNEL_CMDLINE
/*
 * Parse /proc/cmdline to find args for init.  Don't use this!
 *
 * Instead, rely on the kernel to give Finit its arguments as
 * regular argc + argv[].  Only use this if the system you run
 * on has a broken initramfs system that cannot forward args
 * to Finit properly.
 */
static void parse_kernel_cmdline(void)
{
	char line[LINE_SIZE], *cmdline, *tok;
	FILE *fp;

	fp = fopen("/proc/cmdline", "r");
	if (!fp)
		return;

	if (!fgets(line, sizeof(line), fp)) {
		fclose(fp);
		return;
	}

	cmdline = chomp(line);
	_d("%s", cmdline);

	while ((tok = strtok(cmdline, " \t"))) {
		cmdline = NULL;
		parse_arg(tok);
	}
	fclose(fp);
}
#else
#define parse_kernel_cmdline()
#endif

static void parse_kernel_loglevel(void)
{
	char line[LINE_SIZE], *ptr;
	FILE *fp;
	int val;

	fp = fopen("/proc/sys/kernel/printk", "r");
	if (!fp)
		return;

	if (!fgets(line, sizeof(line), fp)) {
		fclose(fp);
		return;
	}
	fclose(fp);

	ptr = chomp(line);
	_d("%s", ptr);
	val = atoi(ptr);
	if (val >= 7)
		kerndebug = 1;
}

/*
 * Kernel gives us all non-kernel options on our cmdline
 */
void conf_parse_cmdline(int argc, char *argv[])
{
	for (int i = 1; i < argc; i++)
		parse_arg(argv[i]);

	parse_kernel_cmdline();
	parse_kernel_loglevel();
}

/*
 * Clear all environment variables read in parse_env(), they may be
 * removed now so let the next call to parse_env() restore them.
 */
void conf_reset_env(void)
{
	struct env_entry *node, *tmp;

	TAILQ_FOREACH_SAFE(node, &env_list, link, tmp) {
		TAILQ_REMOVE(&env_list, node, link);
		if (node->name) {
			unsetenv(node->name);
			free(node->name);
		}
		free(node);
	}

	setenv("PATH", _PATH_STDPATH, 1);
	setenv("SHELL", _PATH_BSHELL, 1);
	setenv("LOGNAME", "root", 1);
	setenv("USER", "root", 1);
}

/*
 * Sets, and makes a note of, all KEY=VALUE lines in a given .conf line
 * from finit.conf, or other .conf file.  Note, PATH is always reset in
 * the conf_reset_env() function.
 */
static void parse_env(char *line)
{
	struct env_entry *node;
	char *key, *val, *end;

	/* skip any leading whitespace */
	key = line;
	while (isspace(*key))
		key++;

	/* find end of line */
	end = key;
	while (*end)
		end++;

	/* strip trailing whitespace */
	if (end > key) {
		end--;
		while (isspace(*end))
			*end-- = 0;
	}

	val = strchr(key, '=');
	if (!val)
		return;
	*val++ = 0;

	/* strip leading whitespace from value */
	while (isspace(*val))
		val++;

	/* unquote value, if quoted */
	if (val[0] == '"' || val[0] == '\'') {
		char q = val[0];

		if (*end == q) {
			val = &val[1];
			*end = 0;
		}
	}

	/* find end of key */
	end = key;
	while (*end)
		end++;

	/* strip trailing whitespace */
	if (end > key) {
		end--;
		while (isspace(*end))
			*end-- = 0;
	}

	setenv(key, val, 1);

	node = malloc(sizeof(*node));
	if (!node) {
		_pe("Out of memory cannot track env vars");
		return;
	}
	node->name = strdup(key);
	TAILQ_INSERT_HEAD(&env_list, node, link);
}

static int kmod_exists(char *mod)
{
	char buf[256];
	int found = 0;
	FILE *fp;

	fp = fopen("/proc/modules", "r");
	if (!fp)
		return 0;

	while (!found && fgets(buf, sizeof(buf), fp)) {
		char *kmod = strtok(buf, " \t");

		if (kmod && !strcmp(kmod, mod))
			found = 1;
	}
	fclose(fp);

	return found;
}

static void kmod_load(char *mod)
{
	char module[64] = { 0 };
	char cmd[CMD_SIZE];

	if (runlevel != 0)
		return;

	/* Strip args for progress below and kmod_exists() */
	strlcpy(module, mod, sizeof(module));
	if (!strtok(module, " \t"))
		return;

	if (kmod_exists(module))
		return;

	strcpy(cmd, "modprobe ");
	strlcat(cmd, mod, sizeof(cmd));

	run_interactive(cmd, "Loading kernel module %s", module);
}

/* Convert optional "[!123456789S]" string into a bitmask */
int conf_parse_runlevels(char *runlevels)
{
	int i, not = 0, bitmask = 0;

	if (!runlevels)
		runlevels = "[234]";
	i = 1;
	while (i) {
		int level;
		char lvl = runlevels[i++];

		if (']' == lvl || 0 == lvl)
			break;
		if ('!' == lvl) {
			not = 1;
			bitmask = 0x3FE;
			continue;
		}

		if ('s' == lvl || 'S' == lvl)
			lvl = '0';

		level = lvl - '0';
		if (level > 9 || level < 0)
			continue;

		if (not)
			CLRBIT(bitmask, level);
		else
			SETBIT(bitmask, level);
	}

	return bitmask;
}

void conf_parse_cond(svc_t *svc, char *cond)
{
	size_t i = 0;
	char *ptr;

	if (!svc) {
		_e("Invalid service pointer");
		return;
	}

	/* By default we assume UNIX daemons support SIGHUP */
	if (svc_is_daemon(svc))
		svc->sighup = 1;

	if (!cond)
		return;

	/* First character must be '!' if SIGHUP is not supported. */
	ptr = cond;
	if (ptr[i] == '!') {
		svc->sighup = 0;
		ptr++;
	}

	while (ptr[i] != '>' && ptr[i] != 0)
		i++;
	ptr[i] = 0;

	if (i >= sizeof(svc->cond)) {
		logit(LOG_WARNING, "Too long event list in declaration of %s: %s", svc->cmd, ptr);
		return;
	}

	if (!strncmp(ptr, "svc/", 4)) {
		logit(LOG_ERR, "Unsupported cond syntax for %s: <%s", svc->cmd, ptr);
		return;
	}

	strlcpy(svc->cond, ptr, sizeof(svc->cond));
}

struct rlimit_name {
	char *name;
	int val;
};

static const struct rlimit_name rlimit_names[] = {
	{ "as",         RLIMIT_AS         },
	{ "core",       RLIMIT_CORE       },
	{ "cpu",        RLIMIT_CPU        },
	{ "data",       RLIMIT_DATA       },
	{ "fsize",      RLIMIT_FSIZE      },
	{ "locks",      RLIMIT_LOCKS      },
	{ "memlock",    RLIMIT_MEMLOCK    },
	{ "msgqueue",   RLIMIT_MSGQUEUE   },
	{ "nice",       RLIMIT_NICE       },
	{ "nofile",     RLIMIT_NOFILE     },
	{ "nproc",      RLIMIT_NPROC      },
	{ "rss",        RLIMIT_RSS        },
	{ "rtprio",     RLIMIT_RTPRIO     },
#ifdef RLIMIT_RTTIME
	{ "rttime",     RLIMIT_RTTIME     },
#endif
	{ "sigpending", RLIMIT_SIGPENDING },
	{ "stack",      RLIMIT_STACK      },

	{ NULL, 0 }
};

int str2rlim(char *str)
{
	const struct rlimit_name *rn;

	for (rn = rlimit_names; rn->name; rn++) {
		if (!strcmp(str, rn->name))
			return rn->val;
	}

	return -1;
}

char *rlim2str(int rlim)
{
	const struct rlimit_name *rn;

	for (rn = rlimit_names; rn->name; rn++) {
		if (rn->val == rlim)
			return rn->name;
	}

	return "unknown";
}

char *lim2str(struct rlimit *rlim)
{
	char tmp[25];
	static char buf[42];

	buf[0] = 0;
	if (RLIM_INFINITY == rlim->rlim_cur)
		snprintf(tmp, sizeof(tmp), "unlimited, ");
	else
		snprintf(tmp, sizeof(tmp), "%llu, ", (unsigned long long)rlim->rlim_cur);
	strlcat(buf, tmp, sizeof(buf));

	if (RLIM_INFINITY == rlim->rlim_max)
		snprintf(tmp, sizeof(tmp), "unlimited, ");
	else
		snprintf(tmp, sizeof(tmp), "%llu, ", (unsigned long long)rlim->rlim_max);
	strlcat(buf, tmp, sizeof(buf));

	return buf;
}

/* First form: `rlimit <hard|soft> RESOURCE LIMIT` */
void conf_parse_rlimit(char *line, struct rlimit arr[])
{
	char *level, *limit, *val;
	int resource = -1;
	rlim_t cfg;

	level = strtok(line, " \t");
	if (!level)
		goto error;

	limit = strtok(NULL, " \t");
	if (!limit)
		goto error;

	val = strtok(NULL, " \t");
	if (!val) {
		/* Second form: `rlimit RESOURCE LIMIT` */
		val   = limit;
		limit = level;
		level = "both";
	}

	resource = str2rlim(limit);
	if (resource < 0 || resource > RLIMIT_NLIMITS)
		goto error;

	/* Official keyword from v3.1 is `unlimited`, from prlimit(1) */
	if (!strcmp(val, "unlimited") || !strcmp(val, "infinity")) {
		cfg = RLIM_INFINITY;
	} else {
		const char *err = NULL;

		cfg = strtonum(val, 0, (long long)2 << 31, &err);
		if (err) {
			logit(LOG_WARNING, "rlimit: invalid %s value: %s",
			      rlim2str(resource), val);
			return;
		}
	}

	if (!strcmp(level, "soft"))
		arr[resource].rlim_cur = cfg;
	else if (!strcmp(level, "hard"))
		arr[resource].rlim_max = cfg;
	else if (!strcmp(level, "both"))
		arr[resource].rlim_max = arr[resource].rlim_cur = cfg;
	else
		goto error;

	return;
error:
	logit(LOG_WARNING, "rlimit: parse error");
}

/* cgroup NAME ctrl.prop:value,ctrl.prop:value ... */
static void conf_parse_cgroup(char *line)
{
	char config[strlen(line) + 1];
	char *ptr, *name;

	name = strtok(line, " \t");
	if (!name)
		return;

	if (strstr(name, "..") || strchr(name, '/'))
		return;		/* illegal */

	config[0] = 0;
	while ((ptr = strtok(NULL, " \t"))) {
		if (config[0])
			strlcat(config, ",", sizeof(config));
		strlcat(config, ptr, sizeof(config));
	}

	cgroup_add(name, config, 0);
}

static int parse_static(char *line, int is_rcsd)
{
	char cmd[CMD_SIZE];
	char *x;

	if (BOOTSTRAP && (MATCH_CMD(line, "host ", x) || MATCH_CMD(line, "hostname ", x))) {
		if (hostname) free(hostname);
		hostname = strdup(strip_line(x));
		return 0;
	}

	if (BOOTSTRAP && MATCH_CMD(line, "mknod ", x)) {
		char *dev = strip_line(x);

		strcpy(cmd, "mknod ");
		strlcat(cmd, dev, sizeof(cmd));
		run_interactive(cmd, "Creating device node %s", dev);
		return 0;
	}

	/* Kernel module to load */
	if (BOOTSTRAP && MATCH_CMD(line, "module ", x)) {
		kmod_load(strip_line(x));
		return 0;
	}

	if (BOOTSTRAP && MATCH_CMD(line, "network ", x)) {
		if (network) free(network);
		network = strdup(strip_line(x));
		return 0;
	}

	if (BOOTSTRAP && MATCH_CMD(line, "runparts ", x)) {
		if (runparts) free(runparts);
		runparts = strdup(strip_line(x));
		return 0;
	}

	if (MATCH_CMD(line, "include ", x)) {
		char *file = strip_line(x);

		strlcpy(cmd, file, sizeof(cmd));
		if (!fexist(cmd)) {
			_e("Cannot find include file %s, absolute path required!", x);
			return 1;
		}

		return parse_conf(cmd, is_rcsd);
	}

	if (MATCH_CMD(line, "log ", x)) {
		char *tok;
		static int size = 200000, count = 5;

		tok = strtok(x, ":= ");
		while (tok) {
			if (!strncmp(tok, "size", 4))
				size = strtobytes(strtok(NULL, ":= "));
			else if (!strncmp(tok, "count", 5))
				count = strtobytes(strtok(NULL, ":= "));

			tok = strtok(NULL, ":= ");
		}

		if (size >= 0)
			logfile_size_max = size;
		if (count >= 0)
			logfile_count_max = count;

		return 0;
	}

	if (MATCH_CMD(line, "shutdown ", x)) {
		if (sdown) free(sdown);
		sdown = strdup(strip_line(x));
		return 0;
	}

	/*
	 * The desired runlevel to start when leaving bootstrap (S).
	 * Finit supports 1-9, but most systems only use 1-6, where
	 * 6 is reserved for reboot and 0 for halt/poweroff.
	 */
	if (BOOTSTRAP && MATCH_CMD(line, "runlevel ", x)) {
		char *token = strip_line(x);
		const char *err = NULL;

		cfglevel = strtonum(token, 1, 9, &err);
		if (err)
			cfglevel = RUNLEVEL;
		if (cfglevel < 1 || cfglevel > 9 || cfglevel == 6)
			cfglevel = 2; /* Fallback */
		return 0;
	}

	/*
	 * Periodic check and instability index leveler, seconds
	 */
	if (MATCH_CMD(line, "service-interval ", x)) {
		char *token = strip_line(x);
		const char *err = NULL;
		int val;

		/* 0 min to 1 day, should check at least daily */
		val = strtonum(token, 0, 1440, &err);
		if (!err) {
			int disabled = !service_interval;

			service_interval = val * 1000; /* to milliseconds */
			if (disabled)
				service_init();
		}
		return 0;
	}

	return 1;
}

static int parse_dynamic(char *line, struct rlimit rlimit[], char *file)
{
	char *x;

	/* Monitored daemon, will be respawned on exit */
	if (MATCH_CMD(line, "service ", x)) {
		service_register(SVC_TYPE_SERVICE, x, rlimit, file);
		return 0;
	}

	/* One-shot task, will not be respawned */
	if (MATCH_CMD(line, "task ", x)) {
		service_register(SVC_TYPE_TASK, x, rlimit, file);
		return 0;
	}

	/* Like task but waits for completion, useful w/ [S] */
	if (MATCH_CMD(line, "run ", x)) {
		service_register(SVC_TYPE_RUN, x, rlimit, file);
		return 0;
	}

	/* Similar to task but is treated like a SysV init script */
	if (MATCH_CMD(line, "sysv ", x)) {
		service_register(SVC_TYPE_SYSV, x, rlimit, file);
		return 0;
	}

	/* Read resource limits */
	if (MATCH_CMD(line, "rlimit ", x)) {
		conf_parse_rlimit(x, rlimit);
		return 0;
	}

	/* Read control group limits */
	if (MATCH_CMD(line, "cgroup ", x)) {
		conf_parse_cgroup(x);
		return 0;
	}

	/* Set current cgroup for the following services/run/tasks */
	if (MATCH_CMD(line, "cgroup.", x)) {
		strlcpy(cgroup_current, x, sizeof(cgroup_current));
		return 0;
	}

	/* Regular or serial TTYs to run getty */
	if (MATCH_CMD(line, "tty ", x)) {
		service_register(SVC_TYPE_TTY, strip_line(x), rlimit, file);
		return 0;
	}

	return 1;
}

static int parse_conf(char *file, int is_rcsd)
{
	struct rlimit rlimit[RLIMIT_NLIMITS];
	char line[LINE_SIZE] = "";
	FILE *fp;

	fp = fopen(file, "r");
	if (!fp)
		return 1;

	/* Prepare default limits and group for each service in /etc/finit.d/ */
	if (is_rcsd) {
		memcpy(rlimit, global_rlimit, sizeof(rlimit));
		cgroup_current[0] = 0;
	}

	_d("*** Parsing %s", file);
	while (!feof(fp)) {
		char *x;

		if (!fgets(line, sizeof(line), fp))
			continue;

		chomp(line);
		tabstospaces(line);
//DEV		_d("%s", line);

		/* Skip comments, i.e. lines beginning with # */
		if (MATCH_CMD(line, "#", x))
			continue;

		if (!parse_static(line, is_rcsd))
			continue;
		if (!parse_dynamic(line, is_rcsd ? rlimit : global_rlimit, file))
			continue;

		/* Not static or dynamic conf, check if it is a global env. */
		parse_env(line);
	}

	fclose(fp);

	return 0;
}

/*
 * Reload /etc/finit.conf and all *.conf in /etc/finit.d/
 */
int conf_reload(void)
{
	size_t i;
	glob_t gl;

	/* Set time according to current time zone */
	tzset();
	_d("Set time  daylight: %d  timezone: %ld  tzname: %s %s",
	   daylight, timezone, tzname[0], tzname[1]);

	/* Mark and sweep */
	cgroup_mark_all();
	svc_mark_dynamic();
	conf_reset_env();

	/*
	 * Reset global rlimit to bootstrap values from conf_init().
	 */
	memcpy(global_rlimit, initial_rlimit, sizeof(global_rlimit));

	if (rescue) {
		int rc;
		char line[80] = "tty [12345] rescue";

		/* If rescue.conf is missing, fall back to a root shell */
		rc = parse_conf(RESCUE_CONF, 0);
		if (rc)
			service_register(SVC_TYPE_TTY, line, global_rlimit, NULL);

		print(rc, "Entering rescue mode");
		goto done;
	}

	/* First, read /etc/finit.conf */
	parse_conf(FINIT_CONF, 0);

	/* Set global limits */
	for (int i = 0; i < RLIMIT_NLIMITS; i++) {
		if (setrlimit(i, &global_rlimit[i]) == -1)
			logit(LOG_WARNING, "rlimit: Failed setting %s: %s",
			      rlim2str(i), lim2str(&global_rlimit[i]));
	}

	/* Next, read all *.conf in /etc/finit.d/ */
	glob(FINIT_RCSD "/*.conf", 0, NULL, &gl);
	glob(FINIT_RCSD "/enabled/*.conf", GLOB_APPEND, NULL, &gl);

	for (i = 0; i < gl.gl_pathc; i++) {
		char *path = gl.gl_pathv[i];
		char *rp = NULL;
		struct stat st;
		size_t len;

		/* Check that it's an actual file ... beyond any symlinks */
		if (lstat(path, &st)) {
			_d("Skipping %s, cannot access: %s", path, strerror(errno));
			continue;
		}

		/* Skip directories */
		if (S_ISDIR(st.st_mode)) {
			_d("Skipping directory %s", path);
			continue;
		}

		/* Check for dangling symlinks */
		if (S_ISLNK(st.st_mode)) {
			rp = realpath(path, NULL);
			if (!rp) {
				logit(LOG_WARNING, "Skipping %s, dangling symlink: %s", path, strerror(errno));
				continue;
			}
		}

		/* Check that file ends with '.conf' */
		len = strlen(path);
		if (len < 6 || strcmp(&path[len - 5], ".conf"))
			_d("Skipping %s, not a Finit .conf file ... ", path);
		else
			parse_conf(path, 1);

		if (rp)
			free(rp);
	}

	globfree(&gl);

	/* Mark any reverse deps as chenaged. */
	service_update_rdeps();

	/* Set up top-level cgroups */
	cgroup_config();
done:
	/* Remove all unused top-level cgroups */
	cgroup_cleanup();

	/* Drop record of all .conf changes */
	drop_changes();

	/* Override configured runlevel, user said 'S' on /proc/cmdline */
	if (BOOTSTRAP && single)
		cfglevel = 1;

	/*
	 * Set host name, from %DEFHOST, *.conf or /etc/hostname.  The
	 * latter wins, if neither exists we default to "noname"
	 */
	set_hostname(&hostname);

	return 0;
}

static struct conf_change *conf_find(char *file)
{
	struct conf_change *node, *tmp;

	TAILQ_FOREACH_SAFE(node, &conf_change_list, link, tmp) {
		if (string_compare(node->name, file))
			return node;
	}

	return NULL;
}

static void drop_change(struct conf_change *node)
{
	if (!node)
		return;

	TAILQ_REMOVE(&conf_change_list, node, link);
	free(node->name);
	free(node);
}


static void drop_changes(void)
{
	struct conf_change *node, *tmp;

	TAILQ_FOREACH_SAFE(node, &conf_change_list, link, tmp)
		drop_change(node);
}

static int do_change(char *dir, char *name, uint32_t mask)
{
	char fn[strlen(dir) + strlen(name) + 2];
	struct conf_change *node;

	paste(fn, sizeof(fn), dir, name);
	_d("path: %s mask: %08x", fn, mask);

	node = conf_find(fn);
	if (mask & (IN_DELETE | IN_MOVED_FROM)) {
		drop_change(node);
		return 0;
	}

	if (node) {
		_d("Event already registered for %s ...", name);
		return 0;
	}

	node = malloc(sizeof(*node));
	if (!node)
		return 1;

	node->name = strdup(fn);
	if (!node->name) {
		free(node);
		return 1;
	}

	_d("Event registered for %s, mask 0x%x", fn, mask);
	TAILQ_INSERT_HEAD(&conf_change_list, node, link);

	return 0;
}

int conf_any_change(void)
{
	if (TAILQ_EMPTY(&conf_change_list))
		return 0;

	return 1;
}

int conf_changed(char *file)
{
	int rc = 0;
	char *rp;

	if (!file)
		return 0;

	rp = realpath(file, NULL);
	if (!rp)
		return 0;

	if (conf_find(rp))
		rc = 1;
	free(rp);

	return rc;
}

static void conf_cb(uev_t *w, void *arg, int events)
{
	static char ev_buf[8 *(sizeof(struct inotify_event) + NAME_MAX + 1) + 1];
	struct inotify_event *ev;
	ssize_t sz;
	size_t off;

	sz = read(w->fd, ev_buf, sizeof(ev_buf) - 1);
	if (sz <= 0) {
		_pe("invalid inotify event");
		return;
	}
	ev_buf[sz] = 0;

	for (off = 0; off < (size_t)sz; off += sizeof(*ev) + ev->len) {
		struct iwatch_path *iwp;

		if (off + sizeof(*ev) > (size_t)sz)
			break;

		ev = (struct inotify_event *)&ev_buf[off];
		if (off + sizeof(*ev) + ev->len > (size_t)sz)
			break;

		if (!ev->mask)
			continue;

		_d("name %s, event: 0x%08x", ev->name, ev->mask);

		/* Find base path for this event */
		iwp = iwatch_find_by_wd(&iw_conf, ev->wd);
		if (!iwp)
			continue;

		if (do_change(iwp->path, ev->name, ev->mask)) {
			_pe(" Out of memory");
			break;
		}
	}

#ifdef ENABLE_AUTO_RELOAD
	if (conf_any_change())
		service_reload_dynamic();
#endif
}

/*
 * Set up inotify watcher and load all *.conf in /etc/finit.d/
 */
int conf_monitor(void)
{
	int rc = 0;

	/*
	 * If only one watcher fails, that's OK.  A user may have only
	 * one of /etc/finit.conf or /etc/finit.d in use, and may also
	 * have or not have symlinks in place.  We need to monitor for
	 * changes to either symlink or target.
	 */
	rc += iwatch_add(&iw_conf, FINIT_RCSD, IN_ONLYDIR);
	rc += iwatch_add(&iw_conf, FINIT_RCSD "/available/", IN_ONLYDIR | IN_DONT_FOLLOW);
	rc += iwatch_add(&iw_conf, FINIT_RCSD "/enabled/",   IN_ONLYDIR | IN_DONT_FOLLOW);
	rc += iwatch_add(&iw_conf, FINIT_CONF, 0);

	/*
	 * Systems with /etc/default, /etc/conf.d, or similar, can also
	 * monitor changes in env files sourced by .conf files (above)
	 * define your own with --with-sysconfig=/path/to/envfiles
	 */
	rc += iwatch_add(&iw_conf, "/etc/default/", IN_ONLYDIR);
	rc += iwatch_add(&iw_conf, "/etc/conf.d/", IN_ONLYDIR);
#ifdef FINIT_SYSCONFIG
	rc += iwatch_add(&iw_conf, FINIT_SYSCONFIG, IN_ONLYDIR);
#endif

	return rc + conf_reload();
}

/*
 * Prepare .conf parser and load /etc/finit.conf for global settings
 */
int conf_init(uev_ctx_t *ctx)
{
	int fd;

	/* default hostname */
	hostname = strdup(DEFHOST);

	/*
	 * Get current global limits, which may be overridden from both
         * finit.conf, for Finit and its services like getty+watchdogd,
         * and *.conf in finit.d/, for each service(s) listed there.
         */
        for (int i = 0; i < RLIMIT_NLIMITS; i++) {
                if (getrlimit(i, &initial_rlimit[i]))
			logit(LOG_WARNING, "rlimit: Failed reading setting %s: %s",
			      rlim2str(i), strerror(errno));
	}

	/* Initialize global rlimits, e.g. for built-in services */
	memcpy(global_rlimit, initial_rlimit, sizeof(global_rlimit));

	/* Read global rlimits and global cgroup setup from /etc/finit.conf */
	parse_conf(FINIT_CONF, 0);

	/* prepare /etc watcher */
	fd = iwatch_init(&iw_conf);
	if (fd < 0)
		return 1;

	if (uev_io_init(ctx, &etcw, conf_cb, NULL, fd, UEV_READ)) {
		_pe("Failed setting up I/O callback for /etc watcher");
		close(fd);
		return 1;
	}

	return 0;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
