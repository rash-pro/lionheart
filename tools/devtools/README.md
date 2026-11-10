# Native developer tools

Configure with `cmake -S tools/devtools -B build/devtools` and build with
`cmake --build build/devtools -j4`. This standalone host project needs C++17
and does not discover or invoke an interpreter.

`pad-support` folds the controller verification helpers into a native header.
Include `pad_support.h` for schedules, field samples, movement comparisons and
protocol checks. The helpers have no command-line entry point.
The five identities in `test_host_pad_verification` retain the original inputs
and assertions, including skipped polls and centered idle axes.

`build/devtools/iro-extract ARCHIVE --list` inspects the IROS directory.
Use `--output DIRECTORY` to extract movies and cameras outside the repository.
Unknown compression, overlapping/truncated records, unsafe paths, duplicate
basenames and symlink escapes fail before unsafe extraction. Existing files are
compared before reuse; replacement uses an exclusively created `.part` file.

All 13 `test_movie_hd` identities are native. The eight presentation cases use
an isolated Vulkan build, its `movie_contract` target and synthetic FFmpeg clips.

`build/devtools/render-quality` accepts scene and crack-budget options:

```sh
build/devtools/render-quality --off build/off --on build/on --scene battle \
  --output build/quality.json --max-crack-pixels 8
```

It compares completed frames 11 through 20, writes the same JSON and stdout,
and returns 0 or 1 according to the crack budget. `render_quality.h` shares the
independent count equations with `test_pgxp`. `movie_frames.h` supplies native
per-tile movie comparisons, RGB15 rounding and stereo anchor checks.

Summarize exact diagnostic boundaries with:

```sh
build/devtools/perf-report --log build/profile/world.txt --first 1800 \
  --last 2400 --histogram build/profile/world-pcs.csv --output build/profile/world.json
```

The native `test_perf_tools` identity retains the four expected counter deltas
and rejects a missing exact boundary. Candidate sorting remains stable for ties.
Histogram output preserves input order for equal instruction counts.

`build/devtools/compare` takes the same options as the previous comparison
script. It copies the source card, isolates configuration and saves, verifies
completion and every frame, then compares complete state/frame/card hashes and
cycle counts. It keeps only the final PNG after writing each receipt.
Both `test_compare` identities protect conflicting output cards, including a
symlink to the source card. SHA-256 uses the existing native test implementation.

`build/devtools/frame-probe BUILD --output FILE [--subsystems]` relinks a
frozen Linux x86-64 Makefiles host with the existing C observer. It reads the
host's cache and link command, preserves argument quoting, generates specialized
GTE wrappers from the public declarations, and removes temporary objects after
linking. The two existing native probe identities now drive the relinker against
a frozen synthetic host before checking sampler lifetime and exact counters.

## Trace and benchmark tools

`frame-bench` runs alternating copied-card routes with checkpoint warmup and contiguous samples.
Receipts include work hashes and summary distributions.

`gte-trace record --host-build BUILD --output DIR` relinks a frozen Makefiles host.
`gte-trace replay --output DIR` builds and compares both GTE paths.

`mdec-trace build --host-build BUILD --output DIR` stages an instrumented hardware source
and relinks it before the frozen runtime archive. `--runtime DIR` selects a frozen runtime.
`mdec-trace record --trace build/capture.mdec -- COMMAND` records observations; `replay` checks them.
Traces must remain under `build/`.

## Disc metadata maintenance

`build/devtools/ff8files` provides `import`, `list`, `get` and `verify`:

```sh
build/devtools/ff8files list disc1.cue --overlays
build/devtools/ff8files list disc1.cue field --json
build/devtools/ff8files get disc1.cue field /scratch/field.bin --loaded
build/devtools/ff8files verify disc2.cue --scan-disc1 disc1.cue
build/devtools/ff8files import --upstream /source/ff8-decomp --commit FULL_SHA \
  --output /scratch/metadata disc1.cue disc2.cue disc3.cue disc4.cue
```

`verify` also holds the per-set entry hints in `overlays.json` (`set_entry_points`,
keyed by a set's catalog SHA-1) to what the generator requires: one catalog set per
row, ascending addresses spelled `0x` and eight lowercase digits, and an instruction
of every image the disc loads for that set.

`--metadata DIR` selects an inventory directory for list/get/verify. Extraction
supports full paths, overlay names and `@table:index` identities; `--raw`,
`--loaded` and `--decompress` are mutually exclusive. Extraction inside the
checkout is restricted to `work/` and `build/`.

The tool links the generator's CUE/ISO, bounded IMG, LZS and SHA-1 readers.
The YAML reader supports block mappings, block sequences (including indentless
sequences), quoted/plain scalars, hexadecimal integers, comments and single-line
flow collections. These cover the upstream lock and binary configuration; tags,
anchors and multiline scalars fail explicitly. No YAML dependency is needed.
Ordered JSON uses the vendored MIT-licensed JSON for Modern C++ header so key
order and ASCII escaping are preserved.

Import calls `git rev-parse HEAD` unless `--commit` is supplied.
`--inventory-only` records skipped symbol conversion; `--unverified-cache` also
requires inventory-only and records an unknown source commit. Use these modes
only when the upstream symbol inputs or source identity are unavailable.
`test_metadata_tool` uses a synthetic raw-sector ISO and exercises the YAML
subset, list output, extraction and verification of corrupt metadata.
