#!/bin/sh
# Demo scene: two control loops on separate CPUs, one gets starved by a higher-priority job.
# The supervisor is woken exactly twice per incident (miss, recovery); the other loop keeps its contract.
cd "$(dirname "$0")" || exit 1   # QNX target has ksh only, no bash
slay -f deadlined supervisor pumpd hog >/dev/null 2>&1
./deadlined & sleep 1
# grace: QEMU timer jitter measured up to 2.09 ms on the test host; on bare metal use a few hundred us
DL_CPU=1 DL_GRACE_US=3000 ./pumpd pump 2000 200 40 20 &   # infusion pump: 2 ms loop on CPU 1
DL_CPU=0 DL_GRACE_US=5000 ./pumpd vent 5000 500 30 20 &   # ventilator: 5 ms loop on CPU 0
sleep 1
./supervisor pump vent &
sleep 3
echo "--- a priority-50 job steals CPU 1 for 20 ms, five times ---"
DL_CPU=1 ./hog 50 20 1500 5
sleep 2
echo "--- ledger ---"
cat /dev/deadline/status
slay -f supervisor pumpd deadlined >/dev/null 2>&1
