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
cannot reach a harness. `build/native-tests/lionheart_tests --list` prints every case with its tier.

Tiers are CTest labels: `-LE 'game|realtime'` is pytest's `-m 'not game and not realtime'`. `-L game` runs the
owned-game cases; without `LIONHEART_DISCS` they skip with the reason in `ctest -V`. `infra` is this framework's own
self-tests and never counts toward parity.

## Layout

| Path | What it holds |
|---|---|
| `tests/native/support/` | the framework (`lh_test.h` is all a case includes) |
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

## Done means

- [ ] One native case for every row of the file in `identities.tsv`, and none extra. `--check-identities` passes,
      and so does `ctest -L infra`:
      `build/native-tests/lionheart_tests --check-identities test_X`
- [ ] `ctest --test-dir build/native-tests -R '^test_X::' -LE realtime` matches pytest's result for the file.
      `cmake --build build/native-tests --target parity` compares the whole ported set with the baseline.
- [ ] The same case fails readably after a deliberately broken expectation or harness (then undo the break).
- [ ] `python -m pytest tests/test_X.py` still passes, and the Python file is unchanged.
- [ ] No game bytes, generated code or private paths in the files you added.

Add new pytest tests to `identities.tsv` with the same commit that adds them. Rows are not regenerated: the file
records what the suite had to match before Python was removed.

## Not covered yet

Tests of the Python generator's internals, `ctypes` tests, numeric and image oracles, and the `tools/` directory are
out of scope until their native replacements exist. Cases that need other top-level CMake targets (SDL, Vulkan) take
them from a separate top-level build. Native tests run on Linux hosts; a Windows target is built by a cross
configure that passes `-DLH_HOST_RUNNER=<native build>/lionheart_tests`.
