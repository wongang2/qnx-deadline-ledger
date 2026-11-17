# Deadline Ledger: timing contracts as files on QNX 8.0

A QNX resource manager that turns real-time deadlines into files: a control loop signs a timing contract by writing one line to `/dev/deadline/<name>`, keeps it by sending one pulse per cycle, and watchers of that file are woken through `ionotify()` when a kernel timer shows the contract has started missing and again when the loop recovers.

Author: Wongang Jang. License: MIT (see `LICENSE`). Demo video: https://youtu.be/pBxeaunC78A

## The problem

Control loops in medical and robotic devices are written as "run every 2 ms", but nothing in the system knows that promise exists. When a loop slips, it often shows up only in a log afterwards. A supervisor that should switch to a safe state has to poll, or the loop has to report on itself, which is exactly what fails when the loop is starved.

## What it does

- A loop signs a contract: `echo "period_us=2000 budget_us=400 grace_us=3000" > /dev/deadline/pump`.
- After each completed cycle the loop sends one pulse (`MsgSendPulse`, non-blocking, so the loop never waits on the server).
- Each pulse re-arms a one-shot kernel timer for one period plus grace. If the timer fires first, the cycle missed its deadline. The miss is counted in the ledger.
- Watchers are woken on state changes only: when a miss episode starts (KEPT to MISSING) and when the loop recovers (MISSING to KEPT). Misses inside an episode are counted but nobody is woken for them.
- `cat /dev/deadline/pump` shows one contract's ledger, `cat /dev/deadline/status` shows all of them. Because it is a file, any tool can read it.
- `dlweb` serves a live dashboard from the target itself: two contracts (an infusion-pump loop and a ventilator loop), buttons that starve one CPU on purpose, and an event log fed by the kernel's `ionotify()` wake-ups.

## How the QNX-specific pieces are used

| QNX feature | Where | Use |
|---|---|---|
| Resource manager (`resmgr_attach`, `iofunc_*`, `dispatch_*`) | `deadlined.c` | Mounts `/dev/deadline`; contracts and `status` are files with read, write, devctl, notify and close handlers |
| Pulses (`MsgSendPulse`, `pulse_attach`) | `libdeadline.h`, `deadlined.c` | Per-cycle heartbeat; the pulse value packs the contract id (top 12 bits) and microseconds of work (low 20 bits) |
| Kernel timers delivering pulses (`timer_create` with `SIGEV_PULSE_INIT`) | `deadlined.c` | One one-shot timer per contract, re-armed on every heartbeat, delivers the deadline pulse to the server's own channel |
| `ionotify()` and `iofunc_notify_trigger()` | `deadlined.c`, `supervisor.c`, `dlweb.c` | Event delivery to watchers; supervisor and the dashboard's event stream sleep until the resource manager wakes them |
| `MsgRegisterEvent()` (secure events) | `supervisor.c`, `dlweb.c` | The client registers its sigevent on the connection before `ionotify()`, otherwise the server cannot deliver it |
| `devctl()` | `libdeadline.h`, `deadlined.c` | `DCMD_DEADLINE_GET_ID` returns the contract id bound to an fd |
| `MsgReceivePulse()`, `TimerTimeout()` | `supervisor.c`, `dlweb.c` | Blocking receive of the wake-up pulse, 15 s keep-alive timeout for the event stream |
| Priority scheduling and CPU run masks (`SCHED_FIFO`, `ThreadCtl(_NTO_TCTL_RUNMASK)`) | `pumpd.c`, `hog.c` | Pin loops to CPUs and starve one with a priority-50 job |

## Components

| File | Role |
|---|---|
| `deadlined.c` | The resource manager mounted at `/dev/deadline` (up to 64 contracts) |
| `libdeadline.h` | Header-only client side: `dl_contract_id()`, `dl_tick()`, pulse value packing |
| `pumpd.c` | Simulated control loop (usage: `pumpd <name> <period_us> <work_us> <priority> [seconds]`); keeps a contract |
| `supervisor.c` | Sleeps until a watched contract changes state, then prints a line (usage: `supervisor <name> [name ...]`) |
| `hog.c` | CPU thief (usage: `hog <priority> <burst_ms> <every_ms> <count>`) used to provoke misses |
| `dlweb.c`, `dlweb_page.html` | Dashboard web server on the target (default port 8080) and its page |
| `demo.sh` | Text scene: two loops, a supervisor, five CPU-starvation bursts, then prints the ledger |
| `dashboard.sh` | Live scene: the same two loops and the supervisor (log in `/tmp/dl-supervisor.log`), left running, plus `dlweb` on port 8080 |
| `Makefile` | Cross-build with `qcc` |
| `NOTES.md` | Development notes, measurements, pitfalls |

`dlweb` HTTP endpoints: `GET /` (page), `GET /status` (same text as `cat /dev/deadline/status`), `GET /events` (Server-Sent Events, pushed on `ionotify()`), `GET /log` (tail of `/tmp/dl-supervisor.log`), `POST /load?cpu=1&ms=20&n=5` (start `hog` at priority 50), `POST /reset` (write `reset` to every contract).

## Requirements

- Build host: x86_64 Linux host with QNX SDP 8.0 (QNX Everywhere), `make`, and `python3` (used by the Makefile to embed the dashboard page).
- Target: QNX 8.0 running on a QEMU x86_64 virtual machine (for example an image built with `mkqnximage`) or another board supported by QNX 8.0 (not tested here). The demo pins loops to CPU 0 and CPU 1, so the target needs at least two CPUs. The target shell has `ksh` only; the scripts are plain `sh`.
- The Makefile builds for x86_64 (`qcc -Vgcc_ntox86_64`). For another architecture change `CC` in the Makefile.

## Build

```sh
source /path/to/qnxsdp-env.sh
make            # builds deadlined pumpd supervisor hog dlweb
```

To remove the build outputs:

```sh
make clean
```

Variables in the Makefile: `CC = qcc -Vgcc_ntox86_64`, `CFLAGS = -O2 -Wall -Wextra -std=gnu11`. `dlweb` links `-lsocket`.

## Run on the target

Copy the five binaries, `demo.sh` and `dashboard.sh` into one directory on the target (the scripts `cd` to their own directory and start `./deadlined`, `./pumpd` and so on), for example with `scp`. Then, as root on the target:

```sh
sh demo.sh        # text scene, prints the ledger at the end
sh dashboard.sh   # live scene, leaves everything running, dashboard on port 8080
```

`demo.sh` starts `deadlined`, two loops (`pump`: 2 ms period on CPU 1, `vent`: 5 ms period on CPU 0), the supervisor, then runs `DL_CPU=1 ./hog 50 20 1500 5` (five 20 ms bursts at priority 50 on the pump's CPU) and prints `/dev/deadline/status`.

Manual use:

```sh
./deadlined &
./pumpd pump 2000 200 40 &          # 2 ms loop, 200 us of work, priority 40
./supervisor pump &                  # sleeps until the contract changes state
cat /dev/deadline/status
```

Environment variables: `DL_CPU` (pin `pumpd` or `hog` to a CPU), `DL_GRACE_US` (contract grace for `pumpd`, default half a period).

## See it in a browser

`dashboard.sh` starts `dlweb` on port 8080 inside the target. With QEMU user-mode networking, add a host forward to the VM's network option, for example:

```
-netdev user,id=net0,hostfwd=tcp:127.0.0.1:8080-:8080
```

Then open `http://127.0.0.1:8080/` on the host. Press a "steal CPU 1: 20 ms x 5 at priority 50" button to starve one loop: its card turns red, the miss appears in the event log, and the loop on the other CPU stays green. "reset ledger" clears the counters. If the VM is reachable on a network, use `http://<target>:8080/` instead.

## Measured results

All numbers were measured on QNX 8.0 in a QEMU x86_64 virtual machine with KVM, not on hardware.

| Measurement | Result |
|---|---|
| 1 ms pulse timer jitter | mean 39 us, worst 2.09 ms |
| Contract grace = period/2 (1 ms) on the 2 ms pump, before any load | 34 one-cycle miss episodes from timer jitter alone |
| Grace 3 ms (above the 2.09 ms worst jitter) | no noise misses |
| `demo.sh`: five 20 ms bursts at priority 50 on the pump's CPU | pump: 5 episodes, 15 missed cycles, 5 recoveries, worst gap 22.8 ms; supervisor woken exactly 10 times; ventilator on the other CPU: 0 misses |
| Dashboard, one "steal CPU 1, 20 ms x5" press after a reset | pump 5 episodes, 15 missed cycles (one per 5 ms period+grace window without a pulse, about 3 per 20 ms burst), 10 watcher wake-ups, 10 events pushed to the page (one per state change); ventilator: 0 misses |

## Known limitations

- Only measured on a virtual machine (QEMU x86_64 with KVM). No hardware board was tested, and `demo.sh` uses a 3 ms grace because the measured worst timer jitter on this VM was 2.09 ms. Grace on hardware was not measured.
- The control loops (`pumpd`) and the "infusion pump" and "ventilator" names are simulations for the demo. This is a prototype, not a certified medical component.
- A contract only covers a miss between pulses; it does not measure the work inside a cycle other than the `used` microseconds the loop reports.
- Up to 64 contracts, contract names up to 31 characters, no authentication on `dlweb` (do not expose port 8080 beyond a test network).

## QNX 8.0 pitfalls hit while building

- `struct itimerspec` is `{ it_value, it_interval }` on QNX. A positional initializer `{{0,0},{s,ns}}` silently disarms a one-shot timer. Use designated initializers.
- Secure events: a server's `MsgDeliverEvent` / `iofunc_notify_trigger` only reaches the client if the client registered the sigevent on that connection with `MsgRegisterEvent(&ev, fd)` before `ionotify()`. Without it the arm succeeds and nothing is ever delivered.
- `DL_DEBUG` is read by the QNX runtime linker; do not use it as an application environment variable.
- `pumpd`'s optional fifth argument is a run time in seconds. The dashboard scene must omit it, otherwise both loops stop and stay MISSING (which is also what a crashed loop looks like on the dashboard).

## License

MIT, Copyright (c) 2026 Wongang Jang. See `LICENSE`.
