"""Reject guest image bytes and generated-catalog inputs in a distributable player."""
import argparse
import json
from pathlib import Path, PureWindowsPath
import re
import shlex
import struct
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
WINDOW_BYTES = 64
MINIMUM_DISTINCT_BYTES = 8


def guest_windows(text):
    counts = [0] * 256
    distinct = 0
    for i, byte in enumerate(text):
        if not counts[byte]:
            distinct += 1
        counts[byte] += 1
        if i >= WINDOW_BYTES:
            old = text[i - WINDOW_BYTES]
            counts[old] -= 1
            if not counts[old]:
                distinct -= 1
        if i >= WINDOW_BYTES - 1 and distinct >= MINIMUM_DISTINCT_BYTES:
            yield text[i - WINDOW_BYTES + 1:i + 1]


def check(binary, text):
    """Keep the host EXE check API; callers may also pass any extracted image."""
    return _check_image(binary, text)


def _check_image(binary, text, binary_windows=None):
    windows = set(guest_windows(text))
    if binary_windows is None:
        for pos in range(len(binary) - WINDOW_BYTES + 1):
            if binary[pos:pos + WINDOW_BYTES] in windows:
                raise ValueError(f"guest text window found at binary offset {pos}")
    else:
        matches = windows.intersection(binary_windows)
        if matches:
            pos = min(binary.find(window) for window in matches)
            raise ValueError(f"guest text window found at binary offset {pos}")
    return {"passed": True, "window_bytes": WINDOW_BYTES,
            "minimum_distinct_bytes": MINIMUM_DISTINCT_BYTES,
            "unique_guest_windows": len(windows), "binary_bytes": len(binary)}


def check_discs(binary, discs, work=ROOT / "build/host/legal-extracted"):
    """Use the recompiler's versioned extraction; never retain game byte fingerprints.

    The private binary-window index is built once, avoiding one complete binary
    scan per overlay. Each Image.data is checked separately to bound guest-window
    memory. Both indexes use exact bytes, not collision-prone hash fingerprints.
    Extracted
    payloads are private temporary files below work, removed on success or failure.
    Legacy extraction supplies complete top-table images, including compressed
    tails. The default recompiler catalog adds its actual loader-bounded images
    and battle effects. Scan both rather than treating the legacy catalog as the
    complete recompiler input. The extractors only read the owned input discs.
    """
    from recomp.disc import extract_disc
    from recomp.overlays import compile_catalog

    discs = [Path(disc) for disc in discs]
    work = Path(work)
    work.mkdir(parents=True, exist_ok=True)
    results = []
    binary_windows = set(guest_windows(binary))
    with tempfile.TemporaryDirectory(prefix="scan-", dir=work) as temporary:
        for ordinal, disc in enumerate(discs):
            _, overlays = extract_disc(disc, Path(temporary) / "legacy" / str(ordinal))
            rows = []
            for index, image, _ in overlays:
                try:
                    result = _check_image(binary, image.data, binary_windows)
                except ValueError as error:
                    raise ValueError(f"{disc}, overlay {index}: {error}") from error
                rows.append({"index": index, "image_bytes": len(image.data),
                             "unique_guest_windows": result["unique_guest_windows"]})
            if not rows:
                raise ValueError(f"{disc}: extraction produced no overlay images")
            results.append({"disc": str(disc), "overlays": rows})
        _, catalog = compile_catalog(discs, Path(temporary) / "catalog")
        catalog_rows = []
        for index, image, _ in catalog:
            try:
                result = _check_image(binary, image.data, binary_windows)
            except ValueError as error:
                raise ValueError(f"catalog overlay {index}: {error}") from error
            catalog_rows.append({"index": index, "image_bytes": len(image.data),
                                 "unique_guest_windows": result["unique_guest_windows"]})
        if not catalog_rows:
            raise ValueError("default recompiler catalog extraction produced no overlay images")
    legacy_count = sum(len(row["overlays"]) for row in results)
    return {"passed": True, "window_bytes": WINDOW_BYTES,
            "minimum_distinct_bytes": MINIMUM_DISTINCT_BYTES,
            "discs": results, "catalog": catalog_rows,
            "legacy_full_images": legacy_count, "catalog_loaded_images": len(catalog_rows),
            "overlay_images": legacy_count + len(catalog_rows)}


def _path(value, directory):
    # Cross-build databases may use Windows separators even when audited on Linux.
    path = Path(value.replace("\\", "/"))
    if PureWindowsPath(value).is_absolute() and not path.is_absolute():
        path = Path("/" + str(path))
    return (path if path.is_absolute() else directory / path).resolve()


def _arguments(arguments, directory, responses, active=()):
    for argument in arguments:
        if not isinstance(argument, str):
            raise ValueError("compile command arguments must be strings")
        if argument.startswith("@"):
            response = _path(argument[1:], directory)
            if response in active or len(active) >= 32:
                raise ValueError(f"recursive compiler response file: {response}")
            try:
                contents = shlex.split(response.read_text(), posix=True)
            except (OSError, ValueError) as error:
                raise ValueError(f"cannot read compiler response file {response}: {error}") from error
            responses.add(response)
            yield from _arguments(contents, directory, responses, (*active, response))
        elif argument == "-Xclang":
            # clang forwards each following option separately to cc1.
            continue
        elif argument.startswith("-Wp,"):
            yield from argument[4:].split(",")
        else:
            yield argument


_INCLUDE_OPTIONS = ("-I", "-isystem", "-iquote", "-idirafter", "-include", "-imacros",
                    "-isystem-after", "-iframework", "-F", "--include-directory", "/I", "/FI",
                    "/external:I")
_GUEST_SOURCE = re.compile(r"(?:functions_|overlay_|dispatch_table|interpreter_table)")
_SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".s", ".m", ".mm"}


def check_compile_commands(database, generated_dirs=()):
    """Reject source/include inputs from generated directories, including rsp files."""
    database = Path(database).resolve()
    try:
        units = json.loads(database.read_text())
    except (OSError, ValueError) as error:
        raise ValueError(f"cannot read compile commands {database}: {error}") from error
    if not isinstance(units, list) or not units:
        raise ValueError("compile commands must contain translation units")
    directories = [_path(str(path), database.parent) for path in generated_dirs]
    cache = database.parent / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text().splitlines():
            if re.match(r"GENERATED_DIR:[^=]+=", line):
                directories.append(_path(line.split("=", 1)[1], database.parent))
    responses = set()
    source_count = include_count = 0

    def inspect(value, directory, kind):
        path = _path(value, directory)
        # Retain the host source-name guard as a second independent boundary.
        if (any("generated" in part.lower() for part in path.parent.parts) or
                (kind == "include" and "generated" in path.name.lower()) or
                any(path == root or root in path.parents for root in directories) or
                (kind == "source" and _GUEST_SOURCE.match(path.name))):
            raise ValueError(f"generated catalog {kind} in player compile command: {path}")

    for unit in units:
        if not isinstance(unit, dict) or not isinstance(unit.get("file"), str):
            raise ValueError("compile command is missing its source file")
        directory = _path(unit.get("directory", "."), database.parent)
        inspect(unit["file"], directory, "source")
        source_count += 1
        arguments = unit.get("arguments")
        if arguments is None:
            try:
                arguments = shlex.split(unit["command"], posix=True)
            except (KeyError, TypeError, ValueError) as error:
                raise ValueError("compile command needs arguments or command") from error
        if not isinstance(arguments, list) or not arguments:
            raise ValueError("compile command arguments must be a nonempty list")
        arguments = list(_arguments(arguments, directory, responses))
        i = 1  # compiler executable is not a build input
        while i < len(arguments):
            argument = arguments[i]
            option = next((flag for flag in sorted(_INCLUDE_OPTIONS, key=len, reverse=True)
                           if argument == flag or argument.startswith(flag + "=") or
                           (flag not in ("--include-directory",) and argument.startswith(flag)
                            and len(argument) > len(flag))), None)
            if option is not None:
                value = argument[len(option):].removeprefix("=")
                if not value:
                    i += 1
                    if i >= len(arguments):
                        raise ValueError(f"missing path for compiler option {option}")
                    value = arguments[i]
                inspect(value, directory, "include")
                include_count += 1
            elif not argument.startswith("-") and Path(argument).suffix.lower() in _SOURCE_SUFFIXES:
                inspect(argument, directory, "source")
                source_count += 1
            i += 1
    return {"passed": True, "translation_units": len(units), "source_paths_checked": source_count,
            "include_paths_checked": include_count, "response_files_checked": len(responses)}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("exe", type=Path)
    parser.add_argument("--disc", type=Path, action="append", default=[],
                        help="owned disc to extract and scan (repeat for discs 1 through 4)")
    parser.add_argument("--compile-commands", type=Path)
    parser.add_argument("--generated-dir", type=Path, action="append", default=[])
    parser.add_argument("--extract-dir", type=Path, default=ROOT / "build/host/legal-extracted")
    args = parser.parse_args(argv)
    binary = args.binary.read_bytes()
    exe = args.exe.read_bytes()
    if len(exe) < 2048 or exe[:8] != b"PS-X EXE":
        parser.error("not a PS-X EXE")
    size = struct.unpack_from("<I", exe, 28)[0]
    if size > len(exe) - 2048:
        parser.error("truncated PS-X EXE")
    result = check(binary, exe[2048:2048 + size])
    if args.disc:
        result["overlays"] = check_discs(binary, args.disc, args.extract_dir)
    if args.compile_commands:
        result["compile_commands"] = check_compile_commands(args.compile_commands, args.generated_dir)
    print(json.dumps(result))


if __name__ == "__main__":
    sys.path.insert(0, str(ROOT))
    main()
