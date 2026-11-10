# Native developer tools

Configure with `cmake -S tools/devtools -B build/devtools` and build with
`cmake --build build/devtools -j4`. This standalone host project needs C++17
and does not discover or invoke an interpreter.

`pad-support` folds the controller verification helpers into a native header.
Include `pad_support.h` for schedules, field samples, movement comparisons and
protocol checks. The former module had no command-line entry point.
The five identities in `test_host_pad_verification` retain the original inputs
and assertions, including skipped polls and centered idle axes.

`build/devtools/iro-extract ARCHIVE --list` inspects the IROS directory.
Use `--output DIRECTORY` to extract movies and cameras outside the repository.
Unknown compression, overlapping/truncated records, unsafe paths, duplicate
basenames and symlink escapes fail before unsafe extraction. Existing files are
compared before reuse; replacement uses an exclusively created `.part` file.

All 13 `test_movie_hd` identities are native. The eight presentation cases use
an isolated Vulkan build, its `movie_contract` target and synthetic FFmpeg clips.

`build/devtools/render-quality` accepts the original `render_quality.py` options:

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

## Migration status

All retained developer tools have native implementations, and every test file in
`tests/native/identities.tsv` has a complete native port. The newer
`test_dispatch_index.py` still needs a port and inventory rows. Keep the original
scripts and pytest files until that final contract is ported and verified.

| Original module | Native status | Evidence |
|---|---|---|
| `compare.py` | Ported as `compare` | Two card-collision identities; synthetic host receipts match byte for byte |
| `perf_report.py` | Ported as `perf-report` | Exact-boundary identity; stdout and JSON match with and without histograms |
| `frame_probe.py` | Native relinker around existing C observer | Both lifetime identities exercise relinking; linked executables match byte for byte with and without subsystem probes |
| `verify_host_pad.py` | Folded into `pad-support` | All five helper identities retain inputs and assertions |
| `iro_extract.py` | Ported as `iro-extract` | Five extraction identities; listing, extraction messages and extracted bytes match |
| `frame_bench.py` | Ported as `frame-bench` | Four native identities; deterministic receipt fields and decimal text match |
| `gte_trace.py` | Ported as `gte-trace` | 18 native identities; exact/play replay stdout and recorded trace bytes match |
| `mdec_trace.py` | Ported as `mdec-trace` | Existing 32 native identities also verify the native instrumentation transform |
| `render_quality.py` | Ported as `render-quality` | All three scene reports and both budget outcomes match stdout, JSON bytes and exit status |
| `spu_audio_compare.py` | Proposed for removal | No callers, documentation or tests found outside its own file |
| `map_movies.py` | Proposed for removal | No callers, documentation or tests found outside its own file |

The last two proposals are evidence from repository reference searches, not
permission to delete. No original script or pytest file has been removed.

The earlier developer-tool validation used synthetic archives, logs and a
compiled synthetic host. Rendering validation also uses synthetic images,
clips and an original two-instruction EXE. No owned-game readiness is implied.
The original scripts and pytest files remain unchanged.

All test files referenced by these tools now have native ports.
No dependency was added, and no runtime, backend or generator source was changed.

`frame-bench` retains the alternating copied-card runs, checkpoint warmup,
contiguous samples, work hashes and summary distributions. Load and elapsed time
are observations of each execution and therefore differ between runs. With the
same native timing fixture, all other receipt fields and the decimal text match
the original tool. All four identities pass in both runners. A seeded percentile
expectation fails with a text diff and was reverted. A second check replays 20 frames measured by the native observer over
200000 runtime GTE commands. Deterministic receipt text and the complete CLI
summary match byte for byte. No owned-game benchmark has been run for this port.

`gte-trace record --host-build BUILD --output DIR` relinks a frozen Makefiles
host. `gte-trace replay --output DIR` builds both GTE paths. The driver reads
specialized declarations from the runtime header without importing the generator.
The 18 `test_gte_diff` identities cover both paths, trace corruption and bounded
observation. Both runners pass. A seeded wrong synthetic command count fails
readably and was reverted. Frozen hosts running the compiled GTE produce the
same trace bytes through both relinkers; replay stdout also matches for 100000
commands and 729 NCLIP corners. These are runtime fixtures, not game captures.

`mdec-trace build --host-build BUILD --output DIR` stages an instrumented hardware
source and relinks it before the frozen runtime archive. `--runtime DIR` selects
a frozen runtime. `mdec-trace record --trace build/capture.mdec -- COMMAND` records
observations, and `replay` checks them. Traces must remain under `build/`.
The existing 32 calendar identities now also compare the C++ transform with the
independently generated CMake source. The transform, trace bytes and record/replay
text match the original on a frozen host running the compiled runtime. A seeded
calendar mismatch fails with `compare=0` and was reverted.

All 54 identities for these three tools have native ports alongside the unchanged
pytest cases. The merged suite also includes the HD movie presentation, PGXP and
movie frame checks. The native runner takes `FF8_PLAYER_HOST` from a separate
top-level build so player-dependent cases run. Original scripts and pytest files
remain in place, and no dependency was added.
