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

The migration is partial. Keep the original scripts and pytest files until all
contracts and baseline dispositions match.

| Original module | Native status | Evidence |
|---|---|---|
| `compare.py` | Ported as `compare` | Two card-collision identities; synthetic host receipts match byte for byte |
| `perf_report.py` | Ported as `perf-report` | Exact-boundary identity; stdout and JSON match with and without histograms |
| `frame_probe.py` | Native relinker around existing C observer | Both lifetime identities exercise relinking; linked executables match byte for byte with and without subsystem probes |
| `verify_host_pad.py` | Folded into `pad-support` | All five helper identities retain inputs and assertions |
| `iro_extract.py` | Ported as `iro-extract` | Five extraction identities; listing, extraction messages and extracted bytes match |
| `frame_bench.py` | Pending | Used by `docs/debugging.md` and `tests/test_frame_bench.py` |
| `gte_trace.py` | Pending | Used by `docs/debugging.md` and `tests/test_gte_diff.py` |
| `mdec_trace.py` | Pending | Instrumentation imported by `tests/test_mdec_calendar.py` |
| `render_quality.py` | Ported as `render-quality` | All three scene reports and both budget outcomes match stdout, JSON bytes and exit status |
| `spu_audio_compare.py` | Proposed for removal | No callers, documentation or tests found outside its own file |
| `map_movies.py` | Proposed for removal | No callers, documentation or tests found outside its own file |

The last two proposals are evidence from repository reference searches, not
permission to delete. No original script or pytest file has been removed.

The earlier developer-tool validation used synthetic archives, logs and a
compiled synthetic host. Rendering validation also uses synthetic images,
clips and an original two-instruction EXE. No owned-game readiness is implied.
The original scripts and pytest files remain unchanged.

Remaining test migrations include `test_frame_bench`, `test_gte_diff`,
`test_mdec_calendar` and `test_perf_stats`.
No dependency was added, and no runtime, backend or generator source was changed.
