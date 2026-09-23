#!/bin/bash
# Runs inside dbus-run-session: start the a11y bus by hand (bus activation of
# at-spi-bus-launcher is refused here), then run the driver.
/usr/libexec/at-spi-bus-launcher --launch-immediately &
LAUNCHER=$!
for i in $(seq 1 40); do
  gdbus call --session --dest org.a11y.Bus --object-path /org/a11y/bus --method org.a11y.Bus.GetAddress >/dev/null 2>&1 && break
  sleep 0.25
done
/usr/libexec/at-spi2-registryd &
REGISTRY=$!
sleep 1
python3 "$@"
RC=$?
kill $REGISTRY $LAUNCHER 2>/dev/null
exit $RC
