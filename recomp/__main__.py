import argparse
from pathlib import Path

from .discover import Image, discover
from .emit import emit


def main_functions(image, timing):
    return discover(image, image.timing_entry_points if timing != "exact" else ())


def main():
    parser = argparse.ArgumentParser(description="Recompile a PS-X EXE to C99")
    parser.add_argument("exe", type=Path, nargs="?")
    parser.add_argument("--disc", type=Path, action="append", help="USA disc image; repeat up to four times")
    parser.add_argument("--assets-dir", type=Path, help="write extracted assets here instead of OUTPUT/extracted")
    parser.add_argument("-o", "--output", type=Path, default=Path("build/gen"))
    parser.add_argument("--mode", choices=("trace", "fast"), default="fast")
    parser.add_argument("--timing", choices=("exact", "block", "loose"), default="exact",
                        help="experimental timing policy; exact preserves the reference emitter")
    parser.add_argument("--host-mode", choices=("precise", "fast"), default="precise",
                        help="precise native interrupt boundaries (default), or batched fast host code")
    parser.add_argument("--legacy-overlays", action="store_true", help="original 24 top-table sets per disc (comparison baseline)")
    args = parser.parse_args()
    try:
        from .disc import extract_disc, main_image
        if args.disc:
            if args.exe:
                parser.error("use either EXE or --disc")
            if len(args.disc) > 4:
                parser.error("--disc may be supplied at most four times")
            if not args.legacy_overlays:
                from .overlays import compile_catalog
                image, overlays = compile_catalog(args.disc, args.assets_dir or args.output / "extracted")
                manifest = emit(image, main_functions(image, args.timing), args.output, mode=args.mode, overlays=overlays, host_mode=args.host_mode, timing=args.timing)
                print(f"Generated {manifest['functions']} main functions and {len(overlays)} catalog sets in {args.output}")
                return
            overlays = []
            seen = set()
            for cue in args.disc:
                current, sets = extract_disc(cue, args.assets_dir or args.output / "extracted")
                if seen.intersection(index for index, _, _ in sets):
                    parser.error("the same disc was supplied more than once")
                seen.update(index for index, _, _ in sets)
                if overlays and current.data != image.data:
                    raise ValueError("disc EXEs differ")
                image = current
                overlays.extend(sets)
            overlays.sort(key=lambda item: item[0])
        elif args.exe:
            image, overlays = main_image(args.exe), []
        else:
            parser.error("EXE or --disc is required")
        manifest = emit(image, main_functions(image, args.timing), args.output, mode=args.mode, overlays=overlays, host_mode=args.host_mode, timing=args.timing)
    except (OSError, ValueError) as error:
        parser.exit(1, f"recomp: {error}\n")
    print(f"Generated {manifest['functions']} functions, {manifest['instructions']} instructions, "
          f"{len(manifest['jump_tables'])} jump tables in {args.output}")


if __name__ == "__main__":
    main()
