# Player failure traces

The host preserves the runtime's stop reason, guest state and stdout summary.
It also writes abnormal stop summaries to stderr and returns 1. Existing startup,
state, capture and cleanup errors retain status 2. The session wrapper only opens
its existing error dialog for status 2; an abnormal guest stop closes the game
window and exits, without returning to the hub or displaying a message.

## Ending audit

These are all `cpu_stop` call sites (relative source paths), including emitted
code and the deliberate test stop. The implementation and declaration are not
call sites.

| Source | Reasons | Classification |
| --- | --- | --- |
| backends/host/main.c | audio dump failure; disc swap failure; entry point returned | Abnormal |
| backends/host/main.c | display closed or failed | Normal user close; abnormal if display failed |
| backends/host/main.c | VBlank limit reached | Normal |
| runtime/cpu.c | instruction budget exhausted | Normal limit |
| runtime/cpu.c | CPU exception; stall; guest callback nesting; native overlay nesting | Abnormal |
| runtime/interp.c | fetch outside RAM; branch in delay slot | Abnormal |
| runtime/irq.c | cyclic BIOS IRQ priority chain | Abnormal |
| runtime/sio.c | SIO allocation failure (then abort) | Abnormal |
| runtime/dma.c | transfer exceeds RAM; oversized GPU list; cyclic/oversized GPU list; unsupported channel; invalid CD direction | Abnormal |
| runtime/bios.c | unterminated string; unsupported printf conversion; unimplemented syscall; oversized string copy; oversized memory operation; invalid heap; ReturnFromException outside exception; unimplemented BIOS function | Abnormal |
| runtime/hw.c | MMIO statistics capacity; hardware/GPU/SPU allocation failure (last three then abort) | Abnormal |
| generator/src/emit.c (emitted code) | branch in delay slot | Abnormal |
| generator/src/emit.c (emitted code) | missing delay slot; branch in delay slot | Abnormal |
| tests/overlay_dispatch_harness.c | test stop | Deliberate abnormal fixture; successful self-test returns 0 |

Other endings and control transfers:

- GUI shell Quit returns 0. Shell errors and failed GUI test assertions return 2.
- State load interrupts dispatch through its safepoint and resumes with the loaded
  CPU; it is not a terminal stop. Initial state-load failures return 2.
- Input script EOF keeps the guest running. GUI script EOF records completion;
  explicit script Quit follows normal quit. Script errors return 2.
- Self-test executables are separate test harnesses; successful completion returns
  0 and failed assertions return nonzero. They do not use the player entry point.
- CLI, configuration, initialization, save/dump, card and shutdown failures retain
  their existing nonzero results. None are normal guest completions.
- Fatal host signals terminate by the signal; unhandled Windows exceptions continue
  searching handlers, preserving system crash reporting.

`Stop:` is parsed by `build/devtools/compare`, `build/devtools/frame-bench`, and boot,
player, GUI-pause and PGXP tests. `Last PC:` is retained in exactly the same
stdout summary format; no existing consumer parses that label directly.

## The next start

The session marks itself running in `<data>/running`, held locked, and clears the mark when the player returns 0
or 2. A mark left behind by a fatal signal, a kill, or an abnormal guest stop (status 1, which closes the window
without a message) opens the hub on the next start with one line naming the log that recorded it, by then rotated
to `logs/lionheart.1.log`, and an action that opens it. A second player on the same data folder cannot take the
lock and reports nothing.

## Fatal path

The session installs handlers before starting the player and redirects fatal
writes directly to the open session file after creating it. This avoids losing
bytes queued to the stderr copying thread when the process dies. The handler
formats a fixed short hexadecimal signal/exception line without allocation,
stdio, SDL or locks. POSIX uses `write`, `kill`, and a disposition reset on entry;
Windows uses `WriteFile` and continues normal exception handling. Guest PC is
omitted: the session owns no CPU, and retaining a raw CPU pointer across state
replacement and disposal would make the crash path unsafe. Normal stop summaries
include the exact last guest PC.

No runtime, generated-code or generator behavior is changed.
