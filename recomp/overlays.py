"""Compile the verified four-disc catalog, including battle-effect images.

All extracted bytes and generated address evidence remain in the output tree.
Input disc images are opened read-only.
"""
import hashlib
import json
import struct
from pathlib import Path

from tools.ff8files import Image as DiscImage, load_inventory
from tools.ff8img import lzs_decompress
from .discover import Image, discover
from .disc import ENTRIES, main_image

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "games/ff8-usa/overlays-all.json"
LEGACY_TOP = set(ENTRIES) | set(range(4, 21)) | {2, 26}


def infer_effect_base(data):
    """Absolute local jumps in the executable prefix anchor the image base.

    Assets may contain instruction-shaped words. Only the first 4 KiB, a
    nearby target, and an independently decodable nonzero target are evidence.
    Reject ties rather than guessing a missing configuration address.
    """
    votes = {}
    for base in (0x80190000, 0x801A0000, 0x801B0000):
        evidence = []
        for offset in range(0, min(len(data), 4096) - 3, 4):
            word = struct.unpack_from("<I", data, offset)[0]
            if word >> 26 not in (2, 3):
                continue
            target = 0x80000000 | ((word & 0x3FFFFFF) << 2)
            dest = target - base
            if not 0 <= dest <= len(data) - 4:
                continue
            ins = Image(data, base, base).instruction(target)
            entry_trampoline = offset == 0 and ins.word >> 16 == 0x27BD and ins.signed_imm < 0
            if abs(dest - offset) >= 0x4000 and not entry_trampoline:
                continue
            if ins.word and ins.name not in ("reserved", "cop_unusable"):
                evidence.append({"offset": hex(offset), "target": hex(target), "target_offset": hex(dest)})
        votes[base] = evidence
    ranked = sorted(votes, key=lambda base: len(votes[base]), reverse=True)
    if not votes[ranked[0]] or len(votes[ranked[0]]) == len(votes[ranked[1]]):
        raise ValueError("effect load address is ambiguous")
    base = ranked[0]
    return base, {"method": "absolute local j/jal in first 4 KiB; nearby targets or an entry trampoline to a prologue; reject ties",
                  "base": hex(base), "votes": {hex(b): len(v) for b, v in votes.items()},
                  "examples": votes[base][:8]}


def compile_catalog(cues, output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    catalog = json.loads(CATALOG.read_text())["overlays"]
    discs, inventories = {}, {}
    evidence, result, decoded = {}, [], {}
    try:
        main = None
        for cue in cues:
            disc = DiscImage(cue)
            if disc.number in discs:
                disc.close()
                raise ValueError("the same disc was supplied more than once")
            discs[disc.number] = disc
            inv = load_inventory(disc, CATALOG.parent)
            inventories[disc.number] = inv
            row = disc.disc.find(inv["exe"]["name"])
            exe = disc.disc.read(row["lba"], row["size"])
            path = output / "SLUS_008.92"
            if main is not None and path.read_bytes() != exe:
                raise ValueError("disc EXEs differ")
            path.write_bytes(exe)
            main = main_image(path)
        # Preserve legacy set numbers for the original top-table overlays.
        extra = 128
        for group in catalog:
            locations = [l for l in group["locations"] if l["disc"] in discs]
            if not locations:
                continue
            verified = {}
            for loc in locations:
                raw = discs[loc["disc"]].read(loc["sector"], loc["size"])
                key = (loc["sha1_stored"], loc["read_size"])
                if key in decoded:
                    data, loaded = decoded[key]
                else:
                    data = lzs_decompress(raw) if loc["compressed"] else raw
                    loaded = lzs_decompress(raw[:loc["read_size"]]) if loc["compressed"] and loc["read_size"] < len(raw) else data
                    decoded[key] = data, loaded
                if hashlib.sha1(raw).hexdigest() != loc["sha1_stored"] or len(data) != group["size"] or hashlib.sha1(data).hexdigest() != group["sha1"]:
                    raise ValueError(f"catalog bytes disagree: {loc['path']} disc {loc['disc']}")
                if len(loaded) != loc["loaded_size"] or hashlib.sha1(loaded).hexdigest() != loc["loaded_sha1"]:
                    raise ValueError(f"loader bytes disagree: {loc['path']}")
                verified[loc["loaded_sha1"]] = (loc, loaded, data)
            # Loader-bounded tails differ on the compressed top-table images.
            # Emit those actual RAM bytes, while retaining the complete catalog identity.
            for loc, loaded, data in verified.values():
                base = int(loc["load_address"], 16) if loc["load_address"] else None
                if base is None:
                    try:
                        base, proof = infer_effect_base(data)
                    except ValueError as error:
                        raise ValueError(f"{group['name']}: {error}") from error
                    evidence[group["sha1"]] = dict(name=group["name"], **proof)
                entries = [e for e in inventories[loc["disc"]]["entries"] if e["path"] == loc["path"]]
                locator = entries[0]["locator"]
                top = locator["index"] if locator["table"] == "top" else None
                index = (loc["disc"] - 1) * 32 + top if top in LEGACY_TOP else extra
                if index == extra:
                    extra += 1
                image = Image(loaded, base, ENTRIES.get(top, base))
                image.metadata = {"name": group["name"], "names": group["names"], "discs": sorted({l["disc"] for l in locations}),
                                  "paths": group["paths"], "catalog_sha1": group["sha1"], "catalog_size": group["size"],
                                  "disc": loc["disc"] if len({l["disc"] for l in locations}) == 1 else None,
                                  "poll_forward": top in (0, 1)}
                seeds = [base + o for o in range(0, len(loaded) - 3, 4)
                         if struct.unpack_from("<I", loaded, o)[0] >> 16 == 0x27BD and struct.unpack_from("<h", loaded, o)[0] < 0]
                # Effect callbacks also appear as absolute pointers in data tables.
                # Requiring a function prologue keeps embedded pixels out of traversal.
                functions = discover(image, seeds, scan_calls=False)
                result.append((index, image, functions))
                (output / f"set{index}.bin").write_bytes(loaded)
        if len({i for i, _, _ in result}) != len(result):
            raise ValueError("catalog set index collision")
        (output / "load-address-evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        return main, sorted(result, key=lambda row: row[0])
    finally:
        for disc in discs.values():
            disc.close()
