#!/bin/sh

set -eu

# shellcheck disable=SC2154
make -C "$top_builddir" DESTDIR="$TENV_ROOT" install

# shellcheck disable=SC2154
FINITBIN="$(pwd)/$top_builddir/src/finit" DEST="$TENV_ROOT" make -f "$srcdir/tenv/root.mk"

# Drop plugins we don't need in test, only causes confusing FAIL in logs.
for plugin in tty.so urandom.so rtc.so modprobe.so hotplug.so; do
	find "$TENV_ROOT" -name $plugin -delete
done

ls -l $DESTDIR
ls -l $DESTDIR/bin
ls -l $DESTDIR/sbin
ls -l $DESTDIR/var/
ls -l $DESTDIR/var/run/

