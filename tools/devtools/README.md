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

The five IROS identities in `test_movie_hd` are native. Its other eight
identities explicitly skip until a native Vulkan fixture exposes the separately
built `movie_contract` target and synthetic FFmpeg clips. Those skips differ from
the passing baseline; this file has not reached full parity.

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
| `frame_bench.py` | Ported as `frame-bench` | Four native identities; deterministic receipt fields and decimal text match |
| `gte_trace.py` | Ported as `gte-trace` | 18 native identities; exact/play replay stdout and recorded trace bytes match |
| `mdec_trace.py` | Pending | Instrumentation imported by `tests/test_mdec_calendar.py` |
| `render_quality.py` | Pending | Imported by `tests/test_pgxp.py` |
| `spu_audio_compare.py` | Proposed for removal | No callers, documentation or tests found outside its own file |
| `map_movies.py` | Proposed for removal | No callers, documentation or tests found outside its own file |

The last two proposals are evidence from repository reference searches, not
permission to delete. No original script or pytest file has been removed.

Validation used synthetic archives, logs and a compiled synthetic host; no
owned-game run was performed. The selected five pytest files pass all 23 cases.
Native registration matches their 23 inventory identities: 15 pass and the eight
HD presentation cases skip for the explicit prerequisites described above.
Each file detected a seeded incorrect expectation; all mutations were reverted.
The standalone tools and full native test project configure and build without
Python on PATH. The standard/infra selection has 233 passes and eight skips.
This does not establish complete migration parity or game/realtime readiness.

Remaining test migrations include `test_mdec_calendar`, `test_pgxp`, `test_perf_stats` and `test_movie_frames`.
No dependency was added, and no runtime, backend or generator source was changed.

`frame-bench` retains the alternating copied-card runs, checkpoint warmup,
contiguous samples, work hashes and summary distributions. Load and elapsed time
are observations of each execution and therefore differ between runs. With the
same native timing fixture, all other receipt fields and the decimal text match
the original tool. All four identities pass in both runners. A seeded percentile
expectation fails with a text diff and was reverted. No owned-game benchmark has
been run for this port.

`gte-trace record --host-build BUILD --output DIR` relinks a frozen Makefiles
host. `gte-trace replay --output DIR` builds both GTE paths. The driver reads
specialized declarations from the runtime header without importing the generator.
The 18 `test_gte_diff` identities cover both paths, trace corruption and bounded
observation. Both runners pass. A seeded wrong synthetic command count fails
readably and was reverted. Frozen hosts running the compiled GTE produce the
same trace bytes through both relinkers; replay stdout also matches for 100000
commands and 729 NCLIP corners. These are runtime fixtures, not game captures.
