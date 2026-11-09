# Porting a test file to the native runner

The test suite is moving from pytest to CTest with a small C++ runner, so that the repository needs no Python. Until
every file is ported, pytest stays green and authoritative and the native tests grow beside it. A file counts as
ported when every test identity it has in pytest exists natively with the same tier and the same result.

## Build and run

```sh
cmake -S tests/native -B build/native-tests
cmake --build build/native-tests -j6 -- -k     # Ninja: -- -k 0. One broken harness must not stop the rest
ctest --test-dir build/native-tests -j6 --output-on-failure -LE 'game|realtime'
```

`tests/native` is its own CMake project: configuring it runs no Python, and top-level defines such as `FF8_PLAYER`
cannot reach a harness. `build/native-tests/lionheart_tests --list` prints every case with its tier. On a shared machine limit the load with
`nice` and a smaller `-j`; `parity` takes its jobs from `CTEST_PARALLEL_LEVEL`.

Tiers are CTest labels: `-LE 'game|realtime'` is pytest's `-m 'not game and not realtime'`. `-L game` runs the
owned-game cases; without `LIONHEART_DISCS` they skip with the reason in `ctest -V`. `infra` is this framework's own
self-tests and never counts toward parity.

## Layout

| Path | What it holds |
|---|---|
| `tests/native/support/` | the framework (`lh_test.h` is all a process-style case includes; `py_random.h`, `values.h`, `equiv.h` add what internals cases need) |
| `tests/native/oracles/` | independent references (IDCT, ADPCM, XA, PNG reading) |
| `tests/native/identities.tsv` | every pytest identity with tier and baseline result, frozen at the import commit |
| `tests/native/cases/test_X.cpp` | the cases of `tests/test_X.py` |
| `tests/native/cases/test_X.cmake` | harnesses that file needs |
| `tests/*.c`, `tests/*.cpp` | C/C++ harnesses, shared with pytest until it is deleted |

Case and fragment files are found by glob, so porting a file edits nothing that another contributor also edits.
Reconfigure after adding one, or run the build again; `CONFIGURE_DEPENDS` picks it up.

## Identity rule

A native case name is the pytest node id without the leading `tests/` and the `.py` of the file name:

```
tests/test_spu_noise_clock.py::test_noise_clock_transitions[exact]
test_spu_noise_clock::test_noise_clock_transitions[exact]
```

The `test_X` part comes from the file name (`__FILE__`) and the function part from the macro argument. For
parametrized tests write pytest's id text exactly, including anything ugly (`[True-1]`, `\n` as two characters).
Do not add, merge, rename or reorder identities. Collapsing many pytest cases into one native case hides coverage,
even when the assertions look identical.

## Port one file

1. Read the `test_X.py` and its `identities.tsv` rows: `grep -F 'tests/test_X.py::' tests/native/identities.tsv`.
2. Write `cases/test_X.cmake`: one `lh_harness` per distinct `cc` command.
3. Write `cases/test_X.cpp`: one `LH_TEST` per test function, keeping pytest's function name.
4. Build, run, and check. Do not delete or change `test_X.py`.

```cpp
// Keep the module docstring as a comment; it says why the test exists.
#include "lh_test.h"

static const char *const kModes[] = {"ntsc", "pal"};      // ids are the strings themselves

LH_TEST_P(test_status_parity, kModes) {                   // t is the Case, p the element
    t.ok(t.harness("status").arg(p).timeout(15));
}

struct Row { const char *id; int width; };                // ids from a struct's id member
static const Row kRows[] = {{"narrow", 320}, {"wide", 640}};
LH_TEST_P(test_width, kRows) { LH_EQ(p.width % 2, 0); }

LH_GAME_TEST(test_real_disc) { const auto disc = lh::owned_disc(1); /* ... */ }
```

For stacked `@pytest.mark.parametrize`, build a `std::vector<Row>` in a static function and pass the call to
`LH_TEST_P`, composing each `id` so the list matches `identities.tsv`. A `tier` member (`lh::Tier::realtime`) on an
element gives that element its own tier, as `pytest.param(..., marks=...)` does.

| In Python | Natively |
|---|---|
| `subprocess.run(..., check=True, timeout=30)` | `t.ok(cmd.timeout(30))` |
| `subprocess.run(...)` then `returncode` checks | `auto r = t.run(cmd); LH_EQ(r.rc, 2);` |
| `tmp_path` | `t.temp()` (fresh; removed on success, kept on failure) |
| `cwd=ROOT` | the default working directory is the repository root |
| `cwd=tmp_path`, `env={...}` | `.cwd(t.temp())`, `.env("NAME", "value")`, `.unset("NAME")` |
| `pytest.skip("why")` | `lh::skip("why")` |
| `assert x == y` | `LH_EQ(x, y)`, or `LH_CHECK_MSG(cond, "why")` |
| `assert 'x' in out` | `LH_CONTAINS(out, "x")`, `LH_NOT_CONTAINS` |
| `assert a == b` on bytes, text or files | `LH_BYTES_EQ`, `LH_TEXT_EQ`, `LH_FILE_EQ` |
| `re.search(...)[1]` | `lh::capture(text, R"(...)")` |
| `hashlib.sha256(...)` | `lh::sha256(bytes)`, `lh::sha256_file(path)` |
| `owned_disc(n)`, `owned_path(rel)` | `lh::owned_disc(n)`, `lh::owned_path(rel)` |
| `real_player` | `t.player()` (takes `FF8_PLAYER_HOST`; it fails instead of building one) |
| `subprocess.run(input=bytes)` | `t.ok(cmd.input(bytes))` (binary is fine) |
| `ctypes.CDLL`, `argtypes`, `restype` | `t.library(name).fn<Signature>("symbol")`, below |
| `random.Random(n)`, its methods | `lh::PyRandom(n)`, same method names, below |
| a Python reference inside a test | an independent header under `oracles/`, below |

Pass paths as `lh::fs::path`, not strings: a Windows harness under Wine gets them as `Z:\...` automatically.
Use `t.harness`, `t.player` or `t.target` for anything built for the target, and `t.host` only for tools that run
on the build machine; the runner adds the emulator to the former only.

Module-scoped fixtures that only build a binary disappear: declare the harness in the fragment.

## Harnesses

Copy the Python `cc` line into `lh_harness` so that the flags stay reviewable side by side:

```cmake
lh_harness(status
  FLAGS -std=c99 -O1 -DFF8_STATS
  INCLUDES runtime                  # -I, relative to the repository
  RUNTIME_ALL                       # *map(str, sorted((ROOT / 'runtime').glob('*.c')))
  LINK_FLAGS -Wl,--wrap=calloc
  SANITIZE_FLAGS ${LH_SANITIZE} -O1 # what the Python added under FF8_TEST_SANITIZE
  SOURCES tests/gpu_status_scanline.c)
```

Sources are relative to the repository. C++ sources take `CXX_FLAGS`. Harness names are private to their file, so a
parametrized test can use one harness per parameter named after its id. `RUNTIME_ALL` shares compiled objects
between harnesses with identical flags.

`assert()` is what most harnesses check. The build defines `-UNDEBUG` on every harness whatever the build type, and
a self-test fails if assertions ever stop working.

C that Python keeps in a string moves to `cases/test_X.c`, copied exactly. Where the Python generates a harness per
test from parameters, generate the variants with a `foreach` in the fragment.

`-fsanitize=...` in `FLAGS` also reaches the link, so copying a Python line with a mandatory sanitizer needs no
`LINK_FLAGS` for it. `SANITIZE_FLAGS` remain for flags the Python only added under `FF8_TEST_SANITIZE`.

## Reaching into the runtime (ctypes tests)

A Python `ctypes` test becomes a native case that loads the same code as a library:

```cmake
lh_harness(model LIBRARY                # -shared -fPIC; loaded by the case, not run
  FLAGS -std=c99 -O2 -DFF8_STATS
  INCLUDES runtime
  RUNTIME_ALL RUNTIME_EXCLUDE hw.c      # every runtime/*.c except a file the fragment replaces
  LINK_FLAGS -Wl,-Bsymbolic             # each build's calls stay inside it, as the Python's did
  LIBS z                                # -l libraries, after the objects
  SOURCES tests/foo_harness.c "${generated}")
```

```cpp
auto step = t.library("model").fn<void(void *, uint64_t)>("foo_step");   // argtypes/restype as a C++ signature
```

Every CTest test is its own process, so a library's globals last one case and a crash or failed `assert()` inside it
takes down only that case, which still prints `FAIL <name>: ... crashed with SIGSEGV`. Assert with `LH_*` as usual.
To compare two builds of the runtime, declare two libraries with `-Wl,-Bsymbolic` and load both: `RTLD_LOCAL` keeps
their identically named symbols apart, and one build's functions can read the other's state (see `test_mdec_calendar`).
For `static` runtime functions keep what the Python did: a small `cases/test_X.c` that `#include`s the runtime `.c`
and exports a wrapper. Do not edit the runtime. A Windows target cannot be loaded by the Linux runner, so `t.library`
skips there with a reason. System libraries such as SDL: `pkg_check_modules` in the fragment, as `cases/test_gpu.cmake`.

### Identical random inputs

`lh::PyRandom` reproduces CPython's `Random` for integer seeds: `random()`, `getrandbits(k)` for `k <= 64`,
`randbytes(n)`, `randrange(start, stop)`, `randrange(stop)`, `randint(a, b)`. Use the same seed and the same calls in
the same order. C++ evaluates the arguments of one call in an unspecified order, so never put two draws in one
call; use separate declarations or a braced list. `randrange` with a step and `numpy.random` are not implemented.

### Independent oracles

An oracle under `oracles/` must not include, link or copy the code it judges; that independence is the point. Write
it from the hardware rule or the Python equation, with `oracles/numeric.h` for Python's floor division and `clip`.
`oracles/png.h` has its own inflate, so a bug in the zlib the display code links cannot cancel out.

### Proving equality while Python is still here

A port with random inputs or an oracle also proves itself: call `lh::equiv::record(t, ...)` for each vector, and add a
function to `tests/native/check_against_python.py` that re-runs the real pytest body with the runtime replaced by a
replay of those records. The replay compares inputs and returns the native oracle's answers, so the pytest assertion
judges the native oracle; run the script after building (`python tests/native/check_against_python.py`). Delete it,
`support/equiv.h` and the `lh::equiv::record` lines with Python.

## Done means

- [ ] One native case for every row of the file in `identities.tsv`, and none extra. `--check-identities` passes,
      and so does `ctest -L infra`:
      `build/native-tests/lionheart_tests --check-identities test_X`
- [ ] `ctest --test-dir build/native-tests -R '^test_X::' -LE realtime` matches pytest's result for the file.
      `cmake --build build/native-tests --target parity` compares the whole ported set with the baseline.
- [ ] The same case fails readably after a deliberately broken expectation or harness (then undo the break).
- [ ] `python -m pytest tests/test_X.py` still passes, and the Python file is unchanged.
- [ ] No game bytes, generated code or private paths in the files you added.
- [ ] Inputs and oracles, if the file has them, pass `tests/native/check_against_python.py`.

Add new pytest tests to `identities.tsv` with the same commit that adds them. Rows are not regenerated: the file
records what the suite had to match before Python was removed.

## Not covered yet

Tests of the generator's internals, `tools/`, a loopback HTTP service, NumPy image helpers and Vulkan replay are not
covered yet. Cases that need other top-level CMake targets take them from a separate top-level build. Native tests
run on Linux hosts; a Windows target is built by a cross configure that passes `-DLH_HOST_RUNNER=<native build>/lionheart_tests`.

## What can be ported next

| Group | Files | Needs beyond this guide |
|---|---|---|
| ctypes and identical inputs | `test_gte` (412), `test_host_viewport` | a library per the Python's `cc` line; `test_gte` also ports its equations to an oracle header |
| checksums | `test_code_sha1` | a SHA-1 oracle next to `lh::sha256` |
| host library tests | `test_host_crt`, `test_host_hotkeys`, `test_host_osd` | the ctypes parts are now expressible; Vulkan parts as those files decide |
| needs the C generator | `test_scratchpad`, `test_idle`, `test_poll`, `test_poll_calendars`, `test_runtime`, `test_gte_timing`, `test_gte_diff`, `test_timing_boundaries`, `test_timing_partial_irq`, `test_timing_regions`, `test_timing_policy`, `test_recomp_configuration`, `test_perf_stats`, `test_recompiler` | wait for the C generator; the library and replay parts are ready |
| Vulkan replay | `test_pgxp` (reuse `oracles/png.h`), `test_movie_hd`, `test_vulkan` | Vulkan fixtures |
| service and NumPy | `test_ra_client`, `test_ra_gui`, `test_state_bridge`, `test_movie_frames` | a loopback HTTP fixture; NumPy's generator and image helpers |
