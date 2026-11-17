# deadline-ledger (QNX Global Hackathon 2026, prototype)

Timing contracts as files on QNX 8.0: `deadlined` is a resource manager mounted at `/dev/deadline`.
A control loop signs a contract by writing `period_us=… budget_us=… grace_us=…` to `/dev/deadline/<name>`,
then sends one pulse per completed cycle. Each pulse re-arms a one-shot kernel timer; if it fires first,
the cycle missed its deadline and every `ionotify()` watcher of that file is woken.

Build on an x86_64 Linux host with QNX SDP 8.0: `source qnxsdp-env.sh && make`.
Run on a QNX 8.0 target (tested: QEMU x86_64 built by mkqnximage, KVM):

    ./deadlined &
    ./pumpd pump 2000 200 40 &          # 2 ms loop, 200 us of work, priority 40
    ./supervisor pump &                  # sleeps until a deadline is missed
    cat /dev/deadline/status

## QNX 8.0 pitfalls hit while building (11/16)
- `struct itimerspec` is `{ it_value, it_interval }` on QNX. Positional `{{0,0},{s,ns}}` silently disarms a one-shot timer. Use designated initializers.
- Secure events: a server's `MsgDeliverEvent`/`iofunc_notify_trigger` only reaches the client if the client registered the sigevent on that connection with `MsgRegisterEvent(&ev, fd)` before `ionotify()`. Without it the arm succeeds and nothing is ever delivered.
- `DL_DEBUG` is read by the QNX runtime linker; do not use it as an app env var.

## First measurements (QEMU x86_64 + KVM, 11/16)
- 1 ms pulse timer: mean jitter 39 us, worst 2.09 ms (virtualized host).
- pump stopped (SIGSTOP) 1 s x3: misses counted, supervisor woken on every miss.
- hog at priority 50, 5 ms bursts pinned to the pump CPU: 10 misses, worst gap 7.2 ms.

## Demo run (demo.sh, QEMU x86_64 + KVM, 11/16)
- Watchers are now woken on state changes only (KEPT -> MISSING, MISSING -> KEPT); the supervisor watches several contracts on one channel.
- Grace matters on a VM: with grace = period/2 (1 ms) the 2 ms pump logged 34 one-cycle episodes from timer jitter alone before any load.
  With grace 3 ms (above the 2.09 ms worst jitter measured) the noise is gone.
- Five 20 ms bursts at priority 50 on the pump's CPU: pump 5 episodes / 15 misses / 5 recoveries, worst gap 22.8 ms;
  supervisor woke exactly 10 times; the ventilator on the other CPU kept its contract (0 misses).

## Live dashboard (dlweb, 11/16, QEMU x86_64 + KVM)
- `dlweb` is a small HTTP server that runs on the QNX target (port 8080, `dashboard.sh` starts the scene).
  The page polls `/status` at 10 Hz (the same file `cat` reads) and listens on `/events`, a Server-Sent Events
  stream whose thread sleeps in `MsgReceivePulse` until deadlined fires `ionotify()` on the status file.
  Buttons start `hog` at priority 50 on a chosen CPU (`POST /load`) and write `reset` to every contract (`POST /reset`).
- Measured from the browser after a reset: one "steal CPU 1, 20 ms x5" press gave pump 5 episodes / 15 missed cycles,
  10 watcher wake-ups, 10 events pushed to the page (one per state change), ventilator on CPU 0: 0 misses.
- Pitfall: `pumpd`'s optional 5th argument is a run time in seconds; the dashboard scene must omit it or both loops
  stop after 20 s and stay MISSING (that is also what a crashed loop looks like on the dashboard).
