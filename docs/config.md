Configuration
=============

* [Introduction](#introduction)
* [Service Wrapper Scripts](#service-wrapper-scripts)
* [Syntax](#syntax)
* [Limitations](#limitations)
  * [/etc/finit.conf](#etcfinitconf)
  * [/etc/finit.d](#etcfinitd)


Introduction
------------

Finit can be configured using only the original `/etc/finit.conf` file
or in combination with `/etc/finit.d/*.conf`.  Finit 3 can even start a
system using only `/etc/finit.d/*.conf`, highly useful for package-based
Linux distributions -- each package can provide its own "script" file.

- `/etc/finit.conf`: main configuration file
- `/etc/finit.d/*.conf`: snippets, usually one service per file

Not all configuration directives are available in `/etc/finit.d/*.conf`
and some directives are only available at bootstrap, runlevel `S`, see
the section [Limitations](#limitations) below for details.

To add a new service, simply drop a `.conf` file in `/etc/finit.d` and
run `initctl reload`.  (It is also possible to `SIGHUP` to PID 1, or
call `finit q`, but that has been deprecated with the `initctl` tool).
Finit monitors all known active `.conf` files, so if you want to force
a restart of any service you can simply touch its corresponding `.conf`
file in `/etc/finit.d` and call `initctl reload`.  Finit handle any
and all conditions and dependencies between services automatically.

It is also possible to drop `.conf` files in `/etc/finit.d/available/`
and use `initctl enable` to enable a service `.conf` file.  This may be
useful in particular to Linux distributions that may want to install all
files for a package and let the user decide when to enable a service.

On `initctl reload` the following is checked for all services:

- If a service's `.conf` file has been removed, or its conditions are no
  longer satisifed, the service is stopped.
- If the file is modified, or a service it depends on has been reloaded,
  the service is reloaded (stopped and started).
- If a new service is added it is automatically started — respecting
  runlevels and return values from any callbacks.

For more info on the different states of a service, see the separate
document [Finit Services](service.md).


Service Wrapper Scripts
-----------------------

If your service requires to run additional commands, executed before the
service is actually started, like the systemd `ExecStartPre`, you can
use a wrapper shell script to start your service.

The Finit service `.conf` file can be put into `/etc/finit.d/available`,
so you can control the service using `initctl`.  Then use the path to
the wrapper script in the Finit `.conf` service stanza.  The following
example employs a wrapper script in `/etc/start.d`.

**Example:**

* `/etc/finit.d/available/program.conf`:
```conf
service [235] <!> /etc/start.d/program -- Example Program
```

* `/etc/start.d/program:`
```shell
#!/bin/sh

# Prepare the command line options
OPTIONS="-u $(cat /etc/username)"

# Execute the program
exec /usr/bin/program $OPTIONS
```

**Note:**, that the example sets `<!>` to denote that it doesn't support
  `SIGHUP`. That way Finit will stop/start such the service instead of
  sending SIGHUP at restart/reload.


Syntax
------

* `host <NAME>`, or `hostname <NAME>`  
  Set system hostname to NAME, unless `/etc/hostname` exists in which
  case the contents of that file is used.

* `module <MODULE> [ARGS]`  
  Load a kernel module, with optional arguments

* `network <PATH>`  
  Script or program to bring up networking, with optional arguments

* `cgroup <NAME> cpuset:SPEC cpu:SPEC mem:SPEC`  
  Create a control group for resource limiting services using cgroups.
  Unlike `rlimit` this setting defines a common group to be selected for
  any set of run/task/services using the `group` directive (below).
  
  A cpuset for a cgroup can be the traditional list of CPU cores to
  assign to the group, or a range that Finit will use to dynamically
  allocate cores based on the amount available.  The range values can be
  either in absolute form or percent.

```shell
        cgroup first cpuset:1
```

  To create a cgroup with one exclusive CPU:

```shell
		cgroup single cpuset:[1,1]
```

  To create cgroup that uses at most two shared CPUs:

```shell
		cgroup uptotwo cpuset:[,2]
```

  To create cgroup with at least two shared CPUs:

```shell
		cgroup dual cpuset:[2,]
```

  To create cgroup with at least one CPU and at most half of all
  available CPUs:

```shell
		cgroup greedy cpuset:[1,50%]
```


* `group <NAME>`
  All subsequent `service`, `task`, or `run` directives are executed in the
  given control group `NAME`, which must be defined prior to this line.  If
  `NAME` does not yet exist the default group `default` is used.

  When separate Finit `.conf` files are used for services the default group
  is reset for each `.conf` file read.

* `rlimit [hard|soft] RESOURCE <LIMIT|unlimited>` Set the hard or soft
  limit for a resource, or both if that argument is omitted.  `RESOURCE`
  is the lower-case `RLIMIT_` string constants from `setrlimit(2)`,
  without the prefix.  E.g. to set `RLIMIT_CPU`, use `cpu`.
  
  LIMIT is an integer that depends on the resource being modified, see
  the man page, or the kernel `/proc/PID/limits` file, for details.
  Finit versions before v3.1 used `infinity` for `unlimited`, which is
  still supported, albeit deprecated.

```shell
        # No process is allowed more than 8MB of address space
        rlimit hard as 8388608

        # Core dumps may be arbitrarily large
        rlimit soft core infinity

        # CPU limit for all services, soft & hard = 10 sec
        rlimit cpu 10
```

  `rlimit` can be set globally, in `/etc/finit.conf`, or locally for
  a set of task/run/services, in `/etc/finit.d/*.conf`.

* `runlevel <N>`  
  N is the runlevel number 1-9, where 6 is reserved for reboot.  
  Default is 2.

* `run [LVLS] <COND> /path/to/cmd ARGS -- Optional description`  
  One-shot command to run in sequence when entering a runlevel, with
  optional arguments and description.
  
  `run` commands are guaranteed to be completed before running the next
  command.  Highly useful if true serialization is needed.

* `task [LVLS] <COND> /path/to/cmd ARGS -- Optional description`  
  One-shot like 'run', but starts in parallel with the next command.
  
  Both `run` and `task` commands are run in a shell, so pipes and
  redirects can be freely used:

```shell
        task [s] echo "foo" | cat >/tmp/bar
```

* `service [LVLS] <COND> /path/to/daemon ARGS -- Optional description`  
  Service, or daemon, to be monitored and automatically restarted if it
  exits prematurely.  Please note that you often need to provide a
  `--foreground` or `--no-background` argument to most daemons to
  prevent them from forking off a sub-process in the background.

```shell
        service [2345] <svc/sbin/zebra> /sbin/ospfd -- OSPF daemon
```

  The `[2345]` is the runlevels `ospfd` is allowed to run in, they are
  optional and default to level 2-5 if left out.
  
  The `<...>` is the condition for starting `ospfd`.  In this example
  Finit waits for another service, `/sbin/zebra`, to have created its
  PID file in `/var/run/zebra.pid` before starting `ospfd`.

  Some services do not maintain a PID file and rather than patching each
  application Finit provides a workaround.  A `pid` keyword can be set
  to have Finit automatically create (when starting) and later remove
  (when stopping) the PID file.  The file is created in the `/var/run`
  directory using the `basename(1)` of the service.  The default can be
  modified with an optional argument:

        pid[:[/path/to/]filename[.pid]]

  For example, by adding `pid:/run/foo.pid` to the service `bar` that
  PID file will, not only be created and removed automatically, but also
  be used by the Finit condition subsystem.  So another service/run/task
  can depend on `<svc/foo>`.

  If a service `bar` *does* maintain a PID file, but using `foo.pid`, we
  can inform Finit of this by simplly prepending an `!` to the argument.

        pid:!/run/foo.pid

  Here Finit will *not* create/remove/touch the PID file, only use it
  for the condition handling instead of the default PID file name.

  For a detailed description of conditions, and how to debug them, see
  the [Finit Conditions](conditions.md) document.

  If a service should not be automatically started, it can be configured
  as manual with the optional `manual` argument. The service can then be
  started at any time by running `initctl start <service>`.

        manual:yes

  The name of a service, shown by the `initctl` tool, defaults to the
  basename of the service executable. It can be changed with the
  optional `name` argument:

        name:<service-name>

* `inetd service/proto[@iflist] <wait|nowait> [LVLS] /path/to/daemon args`  
  Launch a daemon when a client initiates a connection on an Internet
  port.  Available services are listed in the UNIX `/etc/services` file.
  Finit can filter access to from a list of interfaces, `@iflist`, per
  inetd service as well as listen to custom ports.

```shell
        inetd ftp/tcp	nowait	@root	/usr/sbin/uftpd -i -f
        inetd tftp/udp	wait	@root	/usr/sbin/uftpd -i -t
```

  The following example listens to port 2323 for telnet connections and
  only allows clients connecting from `eth0`:

```shell
        inetd 2323/tcp@eth0 nowait [2345] /sbin/telnetd -i -F
```

  The interface list, `@iflist`, is of the format `@iface,!iface,iface`,
  where a single `!` means to deny access.  Notice how interfaces are
  comma separated with no spaces.

  The `inetd` directive can also have ` -- Optional Description`, only
  Finit does not output this text on the console when launching inetd
  services.  Instead this text is sent to syslog and also shown by the
  `initctl` tool.  More on inetd below.

* `runparts <DIR>`  
  Call [run-parts(8)][] on `DIR` to run start scripts.  All executable
  files, or scripts, in the directory are called, in alphabetic order.
  The scripts in this directory are executed at the very end of runlevel
  `S`, bootstrap.

  It can be beneficial to use `S01name`, `S02othername`, etc. if there
  is a dependency order between the scripts.  Symlinks to existing
  daemons can talso be used, but make sure they daemonize by default.

  Similar to the `/etc/rc.local` shell script, make sure that all your
  services and programs either terminate or start in the background or
  you will block Finit.

* `include <CONF>`  
  Include another configuration file.  Absolute path required.

* `log size:200k count:5`

  Log rotation for run/task/services using the `log` sub-option with
  redirection to a log file.  Global setting, applies to all services.

  The size can be given as bytes, without a specifier, or in `k`, `M`,
  or `G`, e.g. `size:10M`, or `size:3G`.  A value of `size:0` disables
  log rotation.  The default is `200k`.

  The count value is recommended to be between 1-5, with a default 5.
  Setting count to 0 means the logfile will be truncated when the MAX
  size limit is reached.

* `tty [LVLS] <DEV> [BAUD] [noclear] [nowait] [nologin] [TERM]`  
  `tty [LVLS] <CMD> <ARGS> [noclear] [nowait]`  
  The first variant of this option uses the built-in getty on the given
  TTY device DEV, in the given runlevels.  The DEV may be the special
  keyword `@console`, which is very useful on embedded systems.  Default
  baud rate is `38400`.

  **Example:**
```conf
        tty [12345] /dev/ttyAMA0 115200 noclear vt220
```

  The second `tty` syntax variant is for using an external getty, like
  agetty or the BusyBox getty.
    
  By default both variants *clear* the TTY and *wait* for the user to
  press enter before starting getty.

  **Example:**
```conf
        tty [12345] /sbin/getty  -L 115200 /dev/ttyAMA0 vt100
        tty [12345] /sbin/agetty -L ttyAMA0 115200 vt100 nowait
```

  The `noclear` option disables clearing the TTY after each session.
  Clearing the TTY when a user logs out is usually preferable.
  
  The `nowait` option disables the `press Enter to activate console`
  message before actually starting the getty program.  On small and
  embedded systems running multiple unused getty wastes both memory
  and CPU cycles, so `wait` is the preferred default.

  The `nologin` option disables getty and `/bin/login`, and gives the
  user a root (login) shell on the given TTY `<DEV>` immediately.
  Needless to say, this is a rather insecure option, but can be very
  useful for developer builds, during board bringup, or similar.

  Notice the ordering, the `TERM` option to the built-in getty must be
  the last argument.

  Embedded systems may want to enable automatic `DEV` by supplying the
  special `@console` device.  This works regardless weather the system
  uses `ttyS0`, `ttyAMA0`, `ttyMXC0`, or anything else.  Finit figures
  it out by querying sysfs: `/sys/class/tty/console/active`.

  **Example:**
```conf
        tty [12345] @console 115200 noclear vt220
```

  On really bare bones systems Finit offers a fallback shell, which
  should not be enabled on production systems since.  This because it
  may give a user root access without having to log in.  However, for
  bringup and system debugging it can come in handy:

```shell
        configure --enable-fallback-shell
```

  One can also use the `service` stanza to start a stand-alone shell:

```conf
        service [12345] /bin/sh -l
```

When running <kbd>make install</kbd> no default `/etc/finit.conf` will
be installed since system requirements differ too much.  Try out the
Debian 6.0 example `/usr/share/doc/finit/finit.conf` configuration that
is capable of service monitoring SSH, sysklogd, gdm and getty!

Every `run`, `task`, `service`, or `inetd` can also list the privileges
the `/path/to/cmd` should be executed with.  Simply prefix the path with
`[@USR[:GRP]]` like this:

```shell
    run [2345] @joe:users /usr/bin/logger "Hello world"
```

For multiple instances of the same command, e.g. a DHCP client or
multiple web servers, add `:ID` somewhere between the `run`, `task`,
`service` keyword and the command, like this:

```shell
    service :1 [2345] /sbin/httpd -f -h /http -p 80   -- Web server
    service :2 [2345] /sbin/httpd -f -h /http -p 8080 -- Old web server
```

Without the `:ID` to the service the latter will overwrite the former
and only the old web server would be started and supervised.

The `run`, `task`, `service`, or `inetd` stanzas also allow the keyword
`log` to redirect `stderr` and `stdout` of the application to a file or
syslog using the native `logit` tool.  The full syntax is:

    log:/path/to/file
    log:prio:facility.level,tag:ident
    log:console
    log:null
    log

Default `prio` is `daemon.info` and default `tag` is the basename of the
service or run/task command.

Log rotation is controlled using the global `log` setting.

**Example:**

    service log:prio:user.warn,tag:ntpd /sbin/ntpd pool.ntp.org -- NTP daemon

Worth noting is that conditions is allowed for all these stanzas.  For a
detailed description, see the [Conditions](conditions.md) document.


Limitations
-----------

To understand the limitations of `finit.conf` vs `finit.d` it is useful
to picture the different phases of the system: bootstrap, runtime, and
shutdown.

### /etc/finit.conf

This file used to be the only way to set up and boot a Finit system.
Today it is mainly used for pre-runtime settings like system hostname,
network bringup and shutdown:

- `host`, only at bootstrap, (runlevel `S`)
- `mknod`, only at bootstrap
- `network`, only at bootstrap
- `runparts`, only at bootstrap
- `include`
- `log`, global setting
- `shutdown`
- `runlevel`, only at bootstrap
- ... and all configuration stanzas from `/etc/finit.d` below

### /etc/finit.d

Support for per-service `.conf` files in `/etc/finit.d` was added in
v3.0 to handle changes of the system configuration at runtime.  As of
v3.1 `finit.conf` is also handled at runtime, except of course for any
stanza that only runs at bootstrap.  However, a `/etc/finit.d/*.conf`
does *not support* the above `finit.conf` settings described above, only
the following:

- `module`, but only at bootstrap
- `service`
- `task`
- `run`
- `inetd`
- `rlimit`
- `tty`

**NOTE:** The `/etc/finit.d` directory was previously the default Finit
  `runparts` directory.  Finit no longer has a default `runparts`, make
  sure to update your setup, or the finit configuration, accordingly.

[run-parts(8)]: http://manpages.debian.org/cgi-bin/man.cgi?query=run-parts
