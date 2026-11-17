#!/bin/sh
# Live dashboard scene: the same two control loops as demo.sh, left running, plus dlweb on port 8080.
# Open http://<target>:8080/ and press a "steal CPU" button to starve a loop on purpose.
cd "$(dirname "$0")" || exit 1   # QNX target has ksh only, no bash
slay -f dlweb deadlined supervisor pumpd hog >/dev/null 2>&1
./deadlined & sleep 1
DL_CPU=1 DL_GRACE_US=3000 ./pumpd pump 2000 200 40 >/dev/null &   # infusion pump: 2 ms loop on CPU 1
DL_CPU=0 DL_GRACE_US=5000 ./pumpd vent 5000 500 30 >/dev/null &   # ventilator: 5 ms loop on CPU 0
sleep 1
./supervisor pump vent > /tmp/dl-supervisor.log 2>&1 &
sleep 2
for c in pump vent; do echo reset > /dev/deadline/$c; done   # drop start-up jitter from the ledger
./dlweb 8080 &
echo "dashboard: http://<target>:8080/"
