#!/bin/sh -e

srcdir=`dirname $0`
test -z "$srcdir" && srcdir=.

(test -f $srcdir/libaccounts-glib.pc.in ) || {
	echo -n "Error: Directory "\`$srcdir\`" does not look like the "
        echo "to-level libaccounts-glib directory."
	exit 1
}

gtkdocize --copy --flavour no-tmpl
autoreconf --install --force
. $srcdir/configure "$@"
