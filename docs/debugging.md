# Find a recompilation divergence

Use your own supported discs and run contributor setup first.
Keep one baseline executable and a native executable built after your change.
Use a fresh private copy of the same card, an explicit empty config, the same script and the same VBlank count.
Build the host tools first with `cmake -S tools/devtools -B build/devtools` and
`cmake --build build/devtools -j4`.

`build/devtools/compare` handles those copies and compares complete dumps and every native software PNG:

```sh
build/devtools/compare --before /path/to/baseline/ff8 --after build/dev/ff8 --exe build/generated/extracted/SLUS_008.92 --disc /path/to/disc1.cue --script tests/scripts/new-game.txt --vblanks 15000 --output build/debug-compare
```

To diagnose native code against the interpreter, run the same developer executable twice,
adding `--interpreter` to one run. Pass `--config` with an empty file, `--renderer soft`, `--headless`,
`--fast`, `--mute`, `--stall 0`, `--max-instructions 0`, `--input-script PATH`, `--max-vblanks N`,
`--dump-state PATH` and `--dump-frames DIRECTORY`. Set `--frame-step 1` for every PNG.
Use `--card1` with separate copies. A player can also be the interpreter reference.

Bisect `N` between the last matching VBlank and the first differing one.
Always rerun from boot or from independently produced compatible checkpoints.
Compare the complete binary dump first, then inspect CPU registers and the RAM difference near the divergence.
An image-only difference suggests presentation or GPU output. A guest-state difference needs CPU/hardware investigation.

`--log PATH` records guest tracing; `--log-events` enables hardware event tracing.
Statistics builds print interpreter/native work counts.
`build/devtools/gte-trace record --host-build build/dev --output build/gte` relinks a tracing host.
`build/devtools/gte-trace replay --output build/gte-replay` builds the trace replayer.
Keep traces and extracted code under `build/`. Do not attach game dumps or generated C to public issues.

Report the supported serial, input script, last matching VBlank, compiler/options, and log excerpts.
A small synthetic MIPS regression is the preferred fix evidence when the failing behavior can be isolated.

## Measuring frame spikes

On Linux x86-64, `build/devtools/frame-probe` relinks an existing CMake host with a diagnostic
object. It leaves the original binary alone and works with both the default player
and the developer build. Relink and preserve the baseline before rebuilding:

```sh
build/devtools/frame-probe build/player --output build/baseline/lionheart-timing
build/devtools/frame-probe build/dev --output build/baseline/ff8-timing
```

Set `FF8_FRAME_TIMINGS` to a CSV path when running a probe. Each VBlank records elapsed
time, the emulation thread's CPU time, guest cycles, and interpreter/native instruction
counts. Frame logging is outside the timed interval. The default probe clocks only
frame boundaries and presentation; it adds no per-instruction timers.

For diagnosis, relink with `--subsystems`. This adds exclusive timers for GTE, GPU
commands, CD/DMA and SPU, plus sampled host/guest PCs in `CSV.samples`. The `guest_ms`
column is the remaining CPU time, including scheduling and host input/audio work.
Nested device calls are charged to their innermost subsystem. These timers perturb
hot paths: do not use a subsystem probe to report a speedup. Vulkan's separate
`--gpu-timings CSV` reports encoding, software raster, upload and submission costs.

`build/devtools/frame-bench` alternates two timing probes, creates fresh card/config copies,
checks completion and identical per-frame guest work, and records load and distributions:

```sh
build/devtools/frame-bench --before build/baseline/lionheart-timing \
  --after build/candidate/lionheart-timing --exe build/generated/extracted/SLUS_008.92 \
  --disc /path/to/disc1.cue --card /path/to/disc1-early.mcr \
  --script tests/scripts/battle-magic.txt --first 2100 --last 4000 \
  --runs 5 --renderer soft --output build/frame-comparison
```

Repeat with `--renderer vulkan` and with the developer probes, both normally and
with `--interpreter`. For Vulkan, select the intended ICD in the environment.
The benchmark does not capture images, dump state, or enable subsystem profiling.
Optional `--before-state` and `--after-state` accept independently produced compatible
checkpoints; restore before `--first` to leave a warmup interval. Prove route exactness
from boot separately with `build/devtools/compare`. A checkpoint is specific to its build,
input script and execution mode.
