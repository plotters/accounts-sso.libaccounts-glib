#!/bin/sh

export AG_APPLICATIONS=$TESTDATADIR
export AG_SERVICES=$TESTDATADIR
export AG_SERVICE_TYPES=$TESTDATADIR
export AG_PROVIDERS=$TESTDATADIR
export ACCOUNTS=/tmp/
export AG_DEBUG=all
export G_MESSAGES_DEBUG=all

# If dbus-test-runner exists, use it to run the tests in a separate D-Bus
# session
if command -v dbus-test-runner > /dev/null ; then
    echo "Using dbus-test-runner"
    dbus-test-runner -m 180 -t ./accounts-glib-testsuite
else
    echo "Using existing D-Bus session"
    ./accounts-glib-testsuite "$@"
fi
