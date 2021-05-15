/* Setup and start system message bus, D-Bus
 *
 * Copyright (c) 2012-2021  Joachim Wiberg <troglobit@gmail.com>
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

#include <sys/types.h>
#include <lite/lite.h>

#include "finit.h"
#include "config.h"
#include "helpers.h"
#include "plugin.h"
#include "service.h"
#include "conf.h"

#define DAEMON "dbus-daemon"
#define ARGS   "--nofork --system --syslog-only"
#define DESC   "D-Bus message bus daemon"

#define DAEMONUSER "messagebus"

static void setup(void *arg)
{
	char line[256];
	mode_t prev;
	char *cmd;

	if (rescue) {
		_d("Skipping %s plugin in rescue mode.", __FILE__);
		return;
	}

	cmd = which(DAEMON);
	if (!cmd) {
		_d("Skipping plugin, %s is not installed.", DAEMON);
		return;
	}

	prev =umask(0);

	mksubsys("/var/run/dbus", 0755, DAEMONUSER, DAEMONUSER);
	if (whichp("dbus-uuidgen"))
		run_interactive("dbus-uuidgen --ensure", "Creating machine UUID for D-Bus");

	/* Clean up from any previous pre-bootstrap run */
	remove("/var/run/dbus/pid");

	/* Register service with Finit */
	snprintf(line, sizeof(line), "[S12345789] cgroup.system @%s:%s %s %s -- %s",
		 DAEMONUSER, DAEMONUSER, cmd, ARGS, DESC);
	if (service_register(SVC_TYPE_SERVICE, line, global_rlimit, NULL))
		_pe("Failed registering %s", DAEMON);
	free(cmd);

	umask(prev);
}

static plugin_t plugin = {
	.name = __FILE__,
	.hook[HOOK_BASEFS_UP] = {
		.cb  = setup
	},
	.depends = { "bootmisc", },
};

PLUGIN_INIT(plugin_init)
{
	plugin_register(&plugin);
}

PLUGIN_EXIT(plugin_exit)
{
	plugin_unregister(&plugin);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
