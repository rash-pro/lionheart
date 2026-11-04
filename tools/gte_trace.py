"""Link the host trace observer or build the two-path GTE replay executable.

Record with FF8_GTE_TRACE=build/world.gte FF8_GTE_TRACE_FIRST=1800
FF8_GTE_TRACE_LAST=1829 build/gte/record EXE [normal host arguments].
Run build/gte/replay build/world.gte to check both paths and recorded outputs.
FF8_GTE_FRAMES names an existing directory for host PNGs at VBlanks 1800,
2400, and 3000, matching the guest VBlank labels rather than zero-based host names.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def flags(text, name):
    return next((shlex.split(line.split(" = ", 1)[1]) for line in text.splitlines()
                 if line.startswith(name + " = ")), [])


def build_replay(output, compiler, extra=(), link=(), name="replay"):
    output.mkdir(parents=True, exist_ok=True)
    common = [compiler, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
              "-I", str(ROOT / "runtime"), *extra]
    if os.environ.get("FF8_TEST_SANITIZE"):
        common += ["-fsanitize=undefined", "-fno-sanitize-recover=all"]
    objects = []
    for reference in (False, True):
        obj = output / ("reference.o" if reference else "gte.o")
        defines = (["-DFF8_GTE_REFERENCE", "-Dgte_cmd=gte_reference_cmd",
                    "-Dgte_read=gte_reference_read", "-Dgte_write=gte_reference_write"] if reference else [])
        subprocess.run([*common, *defines, "-c", str(ROOT / "runtime/gte.c"), "-o", str(obj)], check=True)
        objects.append(str(obj))
    binary = output / name
    subprocess.run([*common, str(ROOT / "tests/gte_diff.c"), *objects, *link, "-o", str(binary)], check=True)
    return binary


def build_record(host, output, compiler):
    output.mkdir(parents=True, exist_ok=True)
    text = (host / "CMakeFiles/ff8.dir/flags.make").read_text()
    # Older frozen hosts still use only the generic API.
    symbols = ["gte_cmd", "host_present"]
    specialized = " gte_rtps\n" in subprocess.check_output(
        ["nm", "--defined-only", str(host / "ff8")], text=True)
    if specialized:
        import sys
        sys.path.insert(0, str(ROOT))
        from recomp.emit import GTE_COMMANDS
        symbols += ["gte_" + name for name in GTE_COMMANDS.values()]
    obj = output / "trace.o"
    subprocess.run([compiler, "-DFF8_GTE_CAPTURE_FRAMES",
                    *(["-DFF8_GTE_SPECIALIZED_TRACE"] if specialized else []), *flags(text, "C_DEFINES"), *flags(text, "C_INCLUDES"),
                    *flags(text, "C_FLAGS"), "-c", str(ROOT / "tools/gte_trace.c"), "-o", str(obj)], check=True)
    command = shlex.split((host / "CMakeFiles/ff8.dir/link.txt").read_text())
    command[command.index("-o") + 1] = str(output / "record")
    command += [str(obj), "-Wl," + ",".join("--wrap=" + symbol for symbol in symbols)]
    subprocess.run(command, cwd=host, check=True)
    return output / "record"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("record", "replay"))
    parser.add_argument("--host-build", type=Path)
    parser.add_argument("--output", type=Path, default=ROOT / "build/gte")
    parser.add_argument("--compiler", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    if args.mode == "record":
        if not args.host_build:
            parser.error("record requires --host-build")
        binary = build_record(args.host_build.resolve(), args.output.resolve(), args.compiler)
    else:
        binary = build_replay(args.output.resolve(), args.compiler)
    print(binary)


if __name__ == "__main__":
    main()
