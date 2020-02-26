/* Simple pidfile event monitor for the Finit condition engine
 *
 * Copyright (c) 2015-2016  Tobias Waldekranz <tobias@waldekranz.com>
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

#include <limits.h>
#include <paths.h>

#include <sys/inotify.h>

#include "finit.h"
#include "cond.h"
#include "helpers.h"
#include "pid.h"
#include "plugin.h"
#include "service.h"

struct context {
	int fd;
	int wd;
};

static void pidfile_callback(void *arg, int fd, int events)
{
	static char ev_buf[8 *(sizeof(struct inotify_event) + NAME_MAX + 1) + 1];
	static char cond[MAX_COND_LEN];

	struct inotify_event *ev;
	ssize_t sz, len;
	svc_t *svc;

	sz = read(fd, ev_buf, sizeof(ev_buf) - 1);
	if (sz <= 0) {
		_pe("invalid inotify event");
		return;
	}
	ev_buf[sz] = 0;

	for (ev = (void *)ev_buf;
	     sz > (ssize_t)sizeof(*ev);
	     len = sizeof(*ev) + ev->len, ev = (void *)ev + len, sz -= len) {
		if (!ev->mask || !strstr(ev->name, ".pid"))
			continue;

		svc = svc_find_by_pidfile(ev->name);
		if (!svc)
			continue;

		mkcond(cond, sizeof(cond), svc->cmd);
		if (ev->mask & (IN_CREATE | IN_ATTRIB | IN_MODIFY | IN_MOVED_TO)) {
			svc_started(svc);
			if (svc_is_forking(svc)) {
				pid_t pid;

				pid = pid_file_read(pid_file(svc));
				_d("Forking service %s changed PID from %d to %d",
				   svc->cmd, svc->pid, pid);
				svc->pid = pid;
			}

			cond_set(cond);
		} else if (ev->mask & IN_DELETE)
			cond_clear(cond);
	}
}

/*
 * This function is called after `initctl reload` to reassert conditions
 * for services that have not been changed.
 *
 * We reassert the run/task/service's condition only if it is running,
 * but not if it's recently been changed or while it's starting up.
 */
static void pidfile_reconf(void *arg)
{
	static char cond[MAX_COND_LEN];
	svc_t *svc, *iter = NULL;

	for (svc = svc_iterator(&iter, 1); svc; svc = svc_iterator(&iter, 0)) {
		if (svc->state != SVC_RUNNING_STATE)
			continue;

		if (svc_is_changed(svc) || svc_is_starting(svc))
			continue;

		mkcond(cond, sizeof(cond), svc->cmd);
		if (cond_get(cond) == COND_ON)
			continue;

		cond_set_path(cond_path(cond), COND_ON);
	}

	/*
	 * This will call service_step(), which in turn will schedule itself
	 * as long as stepped services change state.  Services going from
	 * WAITING to RUNNING will reassert their conditions in that loop,
	 * which in turn may unlock other services, and so on.
	 */
	service_step_all(SVC_TYPE_SERVICE | SVC_TYPE_RUNTASK | SVC_TYPE_INETD);
}

static void pidfile_init(void *arg)
{
	char *path;
	struct context *ctx = arg;
	uint32_t mask = IN_CREATE | IN_ATTRIB | IN_DELETE | IN_MODIFY | IN_MOVED_TO;

	/*
	 * The bootmisc plugin is responsible for setting up /var/run.
	 * and/or /run, with proper symlinks etc.  We depend on bootmisc
	 * so it's safe here to query realpath() and set up inotify.
	 */
	path = realpath(_PATH_VARRUN, NULL);
	if (!path) {
		_pe("Failed ");
		return;
	}

	ctx->wd = inotify_add_watch(ctx->fd, path, mask);
	free(path);

	if (ctx->wd < 0) {
		_pe("inotify_add_watch()");
		close(ctx->fd);
		return;
	}

	_d("pidfile monitor active");
}

static struct context pidfile_ctx;

/*
 * We require /var/run to be set up before calling pidfile_init(),
 * so the bootmisc plugin must run first.
 */
static plugin_t plugin = {
	.name = __FILE__,
	.hook[HOOK_BASEFS_UP]  = { .arg = &pidfile_ctx, .cb = pidfile_init },
	.hook[HOOK_SVC_RECONF] = { .cb = pidfile_reconf },
	.io = {
		.cb    = pidfile_callback,
		.flags = PLUGIN_IO_READ,
	},
	.depends = { "bootmisc", "netlink" },
};

PLUGIN_INIT(plugin_init)
{
	pidfile_ctx.fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (pidfile_ctx.fd < 0) {
		_pe("inotify_init()");
		return;
	}

	plugin.io.fd = pidfile_ctx.fd;
	plugin_register(&plugin);
}

PLUGIN_EXIT(plugin_exit)
{
	inotify_rm_watch(pidfile_ctx.fd, pidfile_ctx.wd);
	close(pidfile_ctx.fd);

	plugin_unregister(&plugin);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
