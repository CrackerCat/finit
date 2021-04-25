/* Low-level service primitives and generic API for managing svc_t structures
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

#ifndef FINIT_SVC_H_
#define FINIT_SVC_H_

#include <sys/ipc.h>		/* IPC_CREAT */
#include <sys/resource.h>
#include <sys/types.h>		/* pid_t */
#include <lite/lite.h>
#include <lite/queue.h>		/* BSD sys/queue.h API */
#include <uev/uev.h>

#include "cgroup.h"
#include "helpers.h"

typedef int svc_cmd_t;

typedef enum {
	SVC_TYPE_FREE       = 0,	/* Free to allocate */
	SVC_TYPE_SERVICE    = 1,	/* Monitored, will be respawned */
	SVC_TYPE_TASK       = 2,	/* One-shot, runs in parallell */
	SVC_TYPE_RUN        = 4,	/* Like task, but wait for completion */
	SVC_TYPE_TTY        = 8,	/* Like service, but dedicated to TTYs */
	SVC_TYPE_SYSV       = 32,	/* SysV style init.d script w/ start/stop */
} svc_type_t;

#define SVC_TYPE_ANY          (-1)
#define SVC_TYPE_RESPAWN      (SVC_TYPE_SERVICE | SVC_TYPE_TTY)
#define SVC_TYPE_RUNTASK      (SVC_TYPE_RUN | SVC_TYPE_TASK | SVC_TYPE_SYSV)

typedef enum {
	SVC_HALTED_STATE = 0,	/* Not allowed in runlevel, or not enabled. */
	SVC_DONE_STATE,		/* Task/Run job has been run */
	SVC_STOPPING_STATE,	/* Waiting to collect the child process */
	SVC_CLEANUP_STATE,	/* Running post: script */
	SVC_SETUP_STATE,	/* Running pre: script */
	SVC_WAITING_STATE,	/* Condition is in flux, process SIGSTOPed */
	SVC_READY_STATE,	/* Enabled but condition not satisfied */
	SVC_RUNNING_STATE,	/* Process running */
} svc_state_t;

typedef enum {
	SVC_BLOCK_NONE = 0,
	SVC_BLOCK_MISSING,
	SVC_BLOCK_CRASHING,
	SVC_BLOCK_USER,
	SVC_BLOCK_BUSY,
	SVC_BLOCK_RESTARTING,
} svc_block_t;

#define MAX_ID_LEN       16
#define MAX_ARG_LEN      64
#define MAX_IDENT_LEN    (MAX_ARG_LEN + MAX_ID_LEN + 1)
#define MAX_STR_LEN      64
#define MAX_COND_LEN     (MAX_ARG_LEN * 3)
#define MAX_USER_LEN     16
#define MAX_NUM_FDS      64	     /* Max number of I/O plugins */
#define MAX_NUM_SVC_ARGS 64

/* Default kill delay (msec) after SIGTERM (svc->sighalt) that we SIGKILL processes */
#define SVC_TERM_TIMEOUT 3000

/* Prevent endless respawn of faulty services. */
#define SVC_RESPAWN_MAX  10

/*
 * Default enable for all services, can be stopped by means
 * of issuing an initctl call. E.g.
 *
 *   initctl <stop|start|restart> service
 */
typedef struct svc {
	TAILQ_ENTRY(svc) link;

	/* Origin of service */
	char           file[MAX_ARG_LEN];

	/* Limits and scoping */
	struct rlimit  rlimit[RLIMIT_NLIMITS];
	struct cgroup  cgroup;

	/* Service details */
	int            sighalt;        /* Signal to stop prorcess, default: SIGTERM */
	int            killdelay;      /* Delay in msec before sending SIGKILL */
	pid_t          oldpid, pid;
	char           pidfile[256];
	long           start_time;     /* Start time, as seconds since boot, from sysinfo() */
	int            started;	       /* Set for run/task/sysv to track if started */
	int            status;	       /* From waitpid() when process is collected */
	const svc_state_t state;       /* Paused, Reloading, Restart, Running, ... */
	svc_type_t     type;	       /* Service, run, task, ... */
	int            protect;        /* Services like dbus-daemon & udev by Finit */
	const int      dirty;	       /* 0: unmodified, 1: modified */
	const int      removed;
	int            starting;       /* ... waiting for pidfile to be re-asserted */
	int	       runlevels;
	int            sighup;	       /* This service supports SIGHUP :) */
	svc_block_t    block;	       /* Reason that this service is currently stopped */
	char           cond[MAX_COND_LEN];

	/* Instance specifics */
	int            job;	       /* For intenal use only, canonical ref is NAME:ID */
	char           name[MAX_ARG_LEN];
	char           id[MAX_ID_LEN]; /* :ID */

	/* Counters */
	char           once;	       /* run/task, (at least) once per runlevel */
	const char     restart_cnt;    /* Incremented for each restart by service monitor. */

	union {
		/* services we redirect stdout/stderr to syslog (not TTYs!) */
		struct {
			char  enabled;
			char  null;
			char  console;
			char  file[64];
			char  prio[20];
			char  ident[20];
		} log;

		/* Only for TTY type services */
		struct {
			char  dev[32];
			char  baud[10];
			char  term[10];
			char  noclear;
			char  nowait;
			char  nologin;
			char  notty;
			char  rescue;
		};
	};

	/* Identity */
	char	       username[MAX_USER_LEN];
	char	       group[MAX_USER_LEN];

	/* Command, arguments and service description */
	char	       cmd[MAX_ARG_LEN];
	char	       args[MAX_NUM_SVC_ARGS][MAX_ARG_LEN];
	int            args_dirty;
	char	       desc[MAX_STR_LEN];
	char	       env[MAX_ARG_LEN];
	char	       pre_script[MAX_ARG_LEN];
	char	       post_script[MAX_ARG_LEN];

	/*
	 * Used to forcefully kill services that won't shutdown on
	 * termination and to delay restarts of crashing services.
	 */
	uev_t          timer;
	void           (*timer_cb)(struct svc *svc);

	/* time at svc_del(), used by gc timer */
	struct timespec gc;
} svc_t;

svc_t      *svc_new                (char *cmd, char *id, int type);
int	    svc_del	           (svc_t *svc);
void	    svc_validate	   (svc_t *svc);

svc_t	   *svc_find	           (char *cmd, char *id);
svc_t	   *svc_find_by_pid        (pid_t pid);
svc_t	   *svc_find_by_jobid      (int job, char *id);
svc_t	   *svc_find_by_nameid     (char *name, char *id);
svc_t	   *svc_find_by_tty        (char *dev);
svc_t      *svc_find_by_pidfile    (char *fn);

svc_t      *svc_iterator           (svc_t **iter, int first);
svc_t      *svc_named_iterator     (svc_t **iter, int first, char *cmd);
svc_t      *svc_job_iterator       (svc_t **iter, int first, int job);

void	    svc_foreach	           (int (*cb)(svc_t *));
void        svc_foreach_type       (int types, int (*cb)(svc_t *));

svc_t	   *svc_stop_completed	   (void);

void	    svc_mark_dynamic       (void);
void	    svc_mark_dirty         (svc_t *svc);
void	    svc_mark_clean         (svc_t *svc);
void	    svc_clean_dynamic      (void (*cb)(svc_t *));
int	    svc_clean_bootstrap    (svc_t *svc);
void	    svc_prune_bootstrap	   (void);

void        svc_enable             (svc_t *svc);
int         svc_enabled            (svc_t *svc);
int         svc_is_unique          (svc_t *svc);

int         svc_parse_jobstr       (char *str, size_t len, int (*found)(svc_t *), int (not_found)(char *, char *));

static inline int svc_is_daemon    (svc_t *svc) { return svc && SVC_TYPE_SERVICE == svc->type; }
static inline int svc_is_sysv      (svc_t *svc) { return svc && SVC_TYPE_SYSV    == svc->type; }
static inline int svc_is_tty       (svc_t *svc) { return svc && SVC_TYPE_TTY     == svc->type; }
static inline int svc_is_runtask   (svc_t *svc) { return svc && (SVC_TYPE_RUNTASK & svc->type);}
static inline int svc_is_forking   (svc_t *svc) { return (svc_is_daemon(svc) || svc_is_sysv(svc)) && svc->pidfile[0] == '!'; }

static inline int svc_in_runlevel  (svc_t *svc, int runlevel) { return svc && ISSET(svc->runlevels, runlevel); }
static inline int svc_nohup        (svc_t *svc) { return svc &&  (0 == svc->sighup || 0 != svc->args_dirty); }
static inline int svc_has_pidfile  (svc_t *svc) { return svc_is_daemon(svc) && svc->pidfile[0] != 0 && svc->pidfile[0] != '!'; }
static inline int svc_has_pre      (svc_t *svc) { return svc->pre_script[0];  }
static inline int svc_has_post     (svc_t *svc) { return svc->post_script[0]; }

static inline void svc_starting    (svc_t *svc) { if (svc) svc->starting = 1;       }
static inline void svc_started     (svc_t *svc) { if (svc) svc->starting = 0;       }
static inline int  svc_is_starting (svc_t *svc) { return svc && 0 != svc->starting; }
static inline int  svc_is_running  (svc_t *svc) { return svc && svc->state == SVC_RUNNING_STATE; }

static inline int  svc_is_removed  (svc_t *svc) { return svc && svc->removed; }
static inline int  svc_is_changed  (svc_t *svc) { return svc &&  0 != svc->dirty; }
static inline int  svc_is_updated  (svc_t *svc) { return svc &&  1 == svc->dirty; }

static inline int  svc_is_blocked  (svc_t *svc) { return svc && svc->block != SVC_BLOCK_NONE; }
static inline int  svc_is_busy     (svc_t *svc) { return svc && svc->block == SVC_BLOCK_BUSY; }
static inline int  svc_is_missing  (svc_t *svc) { return svc && svc->block == SVC_BLOCK_MISSING; }

static inline void svc_unblock     (svc_t *svc) { if (svc) svc->block = SVC_BLOCK_NONE;       }
#define            svc_start(svc)  svc_unblock(svc)
static inline void svc_stop        (svc_t *svc) { if (svc) svc->block = SVC_BLOCK_USER; }
static inline void svc_busy        (svc_t *svc) { if (svc) svc->block = SVC_BLOCK_BUSY; }
static inline void svc_missing     (svc_t *svc) { if (svc) svc->block = SVC_BLOCK_MISSING; }
static inline void svc_restarting  (svc_t *svc) { if (svc) svc->block = SVC_BLOCK_RESTARTING; }
static inline void svc_crashing    (svc_t *svc) { if (svc) svc->block = SVC_BLOCK_CRASHING; }

/* Has condition in configuration and cond is allowed? */
static inline int svc_has_cond(svc_t *svc)
{
	if (!svc->cond[0])
		return 0;

	switch (svc->type) {
	case SVC_TYPE_SERVICE:
	case SVC_TYPE_TASK:
	case SVC_TYPE_RUN:
	case SVC_TYPE_TTY:
	case SVC_TYPE_SYSV:
		return 1;

	default:
		break;
	}

	return 0;
}

static inline char *svc_status(svc_t *svc)
{
	if (!svc)
		return "Unknown";

	switch (svc->state) {
	case SVC_HALTED_STATE:
		switch (svc->block) {
		case SVC_BLOCK_NONE:
			return "halted";

		case SVC_BLOCK_MISSING:
			return "missing";

		case SVC_BLOCK_CRASHING:
			return "crashed";

		case SVC_BLOCK_USER:
			return "stopped";

		case SVC_BLOCK_BUSY:
			return "busy";

		case SVC_BLOCK_RESTARTING:
			return "restart";
		}
		return "unknown";

	case SVC_DONE_STATE:
		return "done";

	case SVC_STOPPING_STATE:
		switch (svc->type) {
		case SVC_TYPE_RUN:
		case SVC_TYPE_TASK:
			return "active";

		default:
			return "stopping";
		}

	case SVC_WAITING_STATE:
		return "waiting";

	case SVC_CLEANUP_STATE:
		return "cleanup";

	case SVC_SETUP_STATE:
		return "setup";

	case SVC_READY_STATE:
		return "ready";

	case SVC_RUNNING_STATE:
		return "running";

	default:
		return "UNKNOWN";
	}
}

static inline const char *svc_dirtystr(svc_t *svc)
{
	if (!svc)
		return "Unknown";

	if (svc_is_removed(svc))
		return "removed";
	if (svc_is_updated(svc))
		return "updated";
	if (svc_is_changed(svc))
		return "UNKNOWN";

	return "clean";
}

/*
 * Returns svc unique identifier tuple 'name:id', or just 'name',
 * if that's enough to identify the service.
 */
static inline char *svc_ident(svc_t *svc, char *buf, size_t len)
{
	static char ident[MAX_IDENT_LEN];

	if (!buf) {
		buf = ident;
		len = sizeof(ident);
	}

	if (!svc)
		return "nil";

	strlcpy(buf, svc->name, len);
	if (strlen(svc->id)) {
		strlcat(buf, ":", len);
		strlcat(buf, svc->id, len);
	}

	return buf;
}

/*
 * Returns svc unique identifier tuple 'job:id', or just 'job',
 * if that's enough to identify the service.
 */
static inline char *svc_jobid(svc_t *svc, char *buf, size_t len)
{
	static char ident[MAX_IDENT_LEN];

	if (!buf) {
		buf = ident;
		len = sizeof(ident);
	}

	if (!svc)
		return "nul";

	if (strlen(svc->id))
		snprintf(buf, len, "%d:%s", svc->job, svc->id);
	else
		snprintf(buf, len, "%d", svc->job);

	return buf;
}

/*
 * non-zero env, no checking if file exists or not
 */
static inline char *svc_getenv(svc_t *svc)
{
	int v = 0;

	if (!svc->env[0])
		return NULL;

	if (svc->env[0] == '-')
		v = 1;

	return &svc->env[v];
}

/*
 * Check if svc has env, if env file exists returns true
 * if it doesn't exist, but has '-', also true.  if svc
 * doesn't have env, also true.
 */
static inline int svc_checkenv(svc_t *svc)
{
	char *env = svc_getenv(svc);

	if (!env || svc->env[0] == '-')
		return 1;

	return fexist(env);
}

#endif /* FINIT_SVC_H_ */

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
