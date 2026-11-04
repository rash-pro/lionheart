# Find a recompilation divergence

Use your own supported discs and run contributor setup first.
Keep one baseline executable and a native executable built after your change.
Use a fresh private copy of the same card, an explicit empty config, the same script and the same VBlank count.
`tools/compare.py` handles those copies and compares complete dumps and every native software PNG:

```sh
python tools/compare.py --before /path/to/baseline/ff8 --after build/dev/ff8 --exe build/generated/extracted/SLUS_008.92 --disc /path/to/disc1.cue --script tests/scripts/new-game.txt --vblanks 15000 --output build/debug-compare
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
`tools/gte_trace.py record --host-build build/dev --output build/gte` relinks a tracing host.
`tools/gte_trace.py replay --output build/gte-replay` builds the trace replayer.
Keep traces and extracted code under `build/`. Do not attach game dumps or generated C to public issues.

Report the supported serial, input script, last matching VBlank, compiler/options, and log excerpts.
A small synthetic MIPS regression is the preferred fix evidence when the failing behavior can be isolated.
