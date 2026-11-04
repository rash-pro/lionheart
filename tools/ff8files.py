"""Import, verify, list and extract FF8 USA IMG metadata (no disc bytes are written by import).

Run --help or see tools/ff8files.py --help. Import needs PyYAML; list/get/verify use only the standard library.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ff8img import lzs_decompress
from psxdisc import Disc

ROOT = Path(__file__).resolve().parents[1]
METADATA = ROOT / "games/ff8-usa"
SOURCE = "https://github.com/roengstrom/ff8-decomp"
COMPRESSED_TOP = {1, 2, 23, 26}
FIELD_START = 0x28900
FIELD_LOOKUP_BYTES = 0x71C
LOCK = "config/SLUS_008.92_FF8DISC1.IMG.lock.yaml"


def sha1(data):
    return hashlib.sha1(data).hexdigest()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=True) + "\n")


class Image:
    """Bounded IMG reads: reject bad metadata before calling Disc.read (which assumes valid LBAs)."""

    def __init__(self, cue):
        self.disc = Disc(cue)
        images = [(p, r) for p, r in self.disc.walk() if re.fullmatch(r"/FF8DISC[1-4]\.IMG", p)]
        if len(images) != 1:
            self.close()
            raise ValueError("expected exactly one FF8DISC[1-4].IMG")
        path, self.record = images[0]
        self.number = int(path[8])
        if self.record["size"] % 2048:
            self.close()
            raise ValueError("IMG size is not sector aligned")
        if (self.record["lba"] + self.record["size"] // 2048) * 2352 > self.disc.bin.seek(0, 2):
            self.close()
            raise ValueError("truncated disc image")
        table = self.read(0, 2048)
        self.top = []
        for lba, size in struct.iter_unpack("<II", table):
            if (lba, size) == (0, 0):
                break
            self.top.append((lba, size))
        if len(self.top) != 133:
            self.close()
            raise ValueError(f"expected 133 top-table entries, got {len(self.top)}")
        self.modules = {}

    def close(self):
        self.disc.bin.close()

    def read(self, sector, size):
        if sector < 0 or size < 0 or sector * 2048 + size > self.record["size"]:
            raise ValueError(f"IMG read outside image: sector={sector}, size={size}")
        return self.disc.read(self.record["lba"] + sector, size)

    def absolute_read(self, lba, size):
        return self.read(lba - self.record["lba"], size)

    def stored_size(self, index):
        lba, size = self.top[index]
        if index in COMPRESSED_TOP:
            return struct.unpack_from("<I", self.absolute_read(lba, 4))[0] + 4
        return size

    def module(self, index):
        if index not in self.modules:
            lba, size = self.top[index]
            raw = self.absolute_read(lba, self.stored_size(index))
            # Recover tables from the whole stream. Overlay hashes use the actual loader read length.
            self.modules[index] = lzs_decompress(raw) if index in COMPRESSED_TOP else raw[:size]
        return self.modules[index]

    def layout(self):
        field = self.module(2)
        offset, next_lba = FIELD_START, None
        while offset + 8 <= len(field):
            lba, size = struct.unpack_from("<II", field, offset)
            if not self.record["lba"] <= lba < self.record["lba"] + self.record["size"] // 2048:
                break
            if next_lba is not None and lba != next_lba:
                break
            next_lba = lba + (size + 2047) // 2048
            offset += 8
        if (offset - FIELD_START) % 24:
            raise ValueError("field table is not a sequence of MIM/DAT/LZK triples")
        count = (offset - FIELD_START) // 24
        ids = struct.unpack_from(f"<{FIELD_LOOKUP_BYTES // 2}H", field, offset)
        if max(ids) >= count:
            raise ValueError("field lookup references an invalid triple")
        movie = offset + FIELD_LOOKUP_BYTES
        # Sound immediately follows the movie table. Its first two sizes are stable in all four discs.
        sound = None
        for pos in range(movie + 8, len(field) - 16, 8):
            lba, size, lba2, size2 = struct.unpack_from("<IIII", field, pos)
            if size == 31792 and size2 == 47792 and lba2 == lba + 16:
                if self.absolute_read(lba, 4) == b"AKAO":
                    sound = pos
                    break
        if sound is None:
            raise ValueError("could not identify field sound table")
        return {"field_count": count, "field_lookup_offset": offset, "field_ids": ids,
                "movie_offset": movie, "movie_slots": (sound - movie) // 8,
                "sound_offset": sound, "sound_slots": 88}

    def pair(self, locator, layout):
        kind = locator["table"]
        slot = locator.get("index", 0)
        if kind == "img":
            return self.record["lba"], 2048
        if kind == "top":
            lba, _ = self.top[slot]
            return lba, self.stored_size(slot)
        module, base = {"intro": (1, 0x10B8), "world": (26, 0x2CCA4),
                        "battle": (25, 0x499B4), "models": (2, 0x286A0),
                        "field": (2, FIELD_START), "movie": (2, layout["movie_offset"]),
                        "sound": (2, layout["sound_offset"])}[kind]
        lba, size = struct.unpack_from("<II", self.module(module), base + slot * 8)
        if kind == "sound":
            size = (size + 2047) // 2048 * 2048
        return lba, size


def locators(image, layout):
    yield {"table": "img"}
    for table, count in [("top", 133), ("intro", 36), ("world", 12), ("battle", 1118),
                         ("models", 76), ("field", layout["field_count"] * 3),
                         ("movie", layout["movie_slots"]), ("sound", layout["sound_slots"])]:
        for index in range(count):
            locator = {"table": table, "index": index}
            lba, size = image.pair(locator, layout)
            if size:
                image.read(lba - image.record["lba"], 0)  # validate pointers including aliases
                yield locator


def field_ids(layout):
    out = defaultdict(list)
    for global_id, local in enumerate(layout["field_ids"]):
        # Zero is both testno and the missing-field sentinel; only global ID 0 names testno.
        if local or global_id == 0:
            out[local].append(global_id)
    return out


def entry_id(locator):
    return f"{locator['table']}:{locator.get('index', 0)}"


def overlay_specs(config, reference):
    """Connect upstream binaries to disc paths by verified decompressed SHA-1, never by a name guess."""
    by_hash = defaultdict(list)
    for entry in reference["entries"]:
        if entry["path"].endswith(".ovl"):
            by_hash[entry["overlay_sha1"]].append(entry["path"])
    specs = {}
    for binary in config["binaries"]:
        if binary["name"] == "SLUS_008.92":
            continue
        paths = by_hash[binary["sha1"]]
        if binary["name"].startswith("effect_"):
            filename = "mag" + binary["name"].removeprefix("effect_") + ".ovl"
            paths = [p for p in paths if Path(p).name == filename]
        if not paths:
            raise ValueError(f"upstream binary has no verified disc-1 overlay: {binary['name']}")
        code = next((s for s in binary["segments"] if isinstance(s, dict) and s.get("type") == "code"), None)
        for path in paths:
            spec = {"name": binary["name"], "load_address": f"0x{code['vram']:08x}" if code else None,
                    "code_configured": code is not None,
                    "symbols": binary["options"].get("symbol_addrs_path", [])}
            if path in specs and specs[path] != spec:
                raise ValueError(f"ambiguous upstream overlay identity for {path}")
            specs[path] = spec
    if len(specs) != 368:
        raise ValueError(f"expected 368 overlay paths, got {len(specs)}")
    return specs


def raw_and_overlay(image, entry):
    raw = image.read(entry["sector"], entry["size"])
    if entry["path"].endswith(".ovl"):
        data = lzs_decompress(raw) if entry["compressed"] else raw
        return raw, data
    return raw, None


def reference_inventory(image, lock, provenance):
    if image.number != 1:
        raise ValueError("the upstream lock must be verified against disc 1")
    layout = image.layout()
    by_extent = defaultdict(list)
    for locator in locators(image, layout):
        by_extent[image.pair(locator, layout)].append(locator)
    entries = []
    for source in lock["entries"]:
        entry = dict(source)
        key = (image.record["lba"] + entry["sector"], entry["size"])
        if not by_extent[key]:
            raise ValueError(f"no table locator for {entry['path']}")
        locator = by_extent[key][0]
        entry["locator"] = locator
        entry["id"] = entry_id(locator)
        if locator["table"] == "field":
            entry["field_ids"] = field_ids(layout)[locator["index"] // 3]
        entry["path_source"] = "upstream"
        entry["differs_from_disc1"] = False
        entry["compressed"] = entry["path"].endswith(".ovl") and (
            locator["table"] in {"battle", "movie"} or
            locator["table"] == "top" and locator["index"] in COMPRESSED_TOP)
        entry["read_size"] = (image.top[locator["index"]][1]
                              if locator["table"] == "top" else entry["size"])
        raw, overlay = raw_and_overlay(image, entry)
        if sha1(raw) != entry["sha1"]:
            raise ValueError(f"disc-1 SHA-1 mismatch: {entry['path']}")
        if overlay is not None:
            entry.update(overlay_sha1=sha1(overlay), overlay_size=len(overlay))
            loaded = (lzs_decompress(raw[:entry["read_size"]])
                      if entry["compressed"] and entry["read_size"] < len(raw) else overlay)
            entry.update(loaded_sha1=sha1(loaded), loaded_size=len(loaded))
        entries.append(entry)
    result = inventory_header(image, provenance, layout)
    result["entries"] = entries
    result["missing_from_disc1"] = []
    # Hash the logical IMG as well, without retaining another 600 MB copy in memory.
    digest = hashlib.sha1()
    for sector in range(0, image.record["size"] // 2048, 512):
        digest.update(image.read(sector, min(512 * 2048, image.record["size"] - sector * 2048)))
    if digest.hexdigest() != lock["image"]["sha1"]:
        raise ValueError("disc-1 logical IMG SHA-1 mismatch")
    result["image"]["sha1"] = digest.hexdigest()
    return result


def inventory_header(image, provenance, layout):
    exe = next(r for p, r in image.disc.walk() if re.fullmatch(r"/SLUS_\d{3}\.\d{2}", p))
    return {"schema_version": 1, "source": provenance, "disc": image.number,
            "exe": {"name": exe["name"], "sha1": sha1(image.disc.read(exe["lba"], exe["size"]))},
            "image": {"name": image.record["name"], "lba": image.record["lba"],
                      "sector_size": 2048, "size": image.record["size"]},
            "tables": {k: v for k, v in layout.items() if k != "field_ids"}}


def other_inventory(image, reference):
    layout = image.layout()
    ref_by_path = {e["path"]: e for e in reference["entries"]}
    ref_locators = {}
    ref_field = {}
    reference_ids = reference["_field_ids"]
    # Give shared global IDs their verified disc-1 names, including changed field content.
    for entry in reference["entries"]:
        locator = entry["locator"]
        if locator["table"] == "field":
            local, component = divmod(locator["index"], 3)
            for global_id in reference_ids.get(local, []):
                ref_field[(global_id, component)] = entry
        elif locator["table"] != "movie":
            ref_locators[tuple(locator.items())] = entry
    ids = field_ids(layout)
    # Sector fingerprints find unchanged files independently of all the pointer tables.
    by_head = defaultdict(list)
    ref_heads = reference["_heads"]
    for path, head in ref_heads.items():
        by_head[head].append(ref_by_path[path])
    entries, used_paths, used_extents = [], set(), set()
    for locator in locators(image, layout):
        lba, size = image.pair(locator, layout)
        sector = lba - image.record["lba"]
        if (sector, size) in used_extents:
            continue
        used_extents.add((sector, size))
        raw = image.read(sector, size)
        digest = sha1(raw)
        head = sha1(raw[:min(size, 2048)])
        candidates = [e for e in by_head.get(head, []) if e["size"] == size and e["sha1"] == digest]
        original = None
        if locator["table"] == "field":
            local, component = divmod(locator["index"], 3)
            names = {ref_field[(g, component)]["path"] for g in ids[local] if (g, component) in ref_field}
            if len(names) == 1:
                original = ref_by_path[next(iter(names))]
            elif len(names) > 1:
                raise ValueError("global field ID aliases disagree on a disc-1 name")
        else:
            original = ref_locators.get(tuple(locator.items()))
        # Movies use a disc-local order. Equal ordinal numbers do not establish the same movie.
        content_named = False
        if (original is None and locator["table"] != "field" and len(candidates) == 1 and
                candidates[0]["path"] not in used_paths):
            original = candidates[0]
            content_named = True
        path_source = "content-disc1" if content_named else "table-disc1" if original else "generated"
        if original:
            path = original["path"]
        elif locator["table"] == "field":
            local, component = divmod(locator["index"], 3)
            tag = f"id_{ids[local][0]:04d}" if ids[local] else f"disc{image.number}_slot_{local:04d}"
            path = f"data/eng/field/mapdata/_unidentified/{tag}/{tag}.{['mim', 'dat', 'lzk'][component]}"
        elif locator["table"] == "movie":
            path = ("data/movie/movie.ovl" if locator["index"] == 0 else
                    f"data/movie/disc{image.number}_fmv_{locator['index'] - 1:03d}.bin")
            if locator["index"] == 0:
                original = ref_by_path[path]
                path_source = "table-disc1"
        else:
            raise ValueError(f"no path for table record {locator}")
        used_paths.add(path)
        compressed = path.endswith(".ovl") and (locator["table"] in {"battle", "movie"} or
                      locator["table"] == "top" and locator["index"] in COMPRESSED_TOP)
        entry = {"id": entry_id(locator), "path": path, "sector": sector, "size": size, "sha1": digest, "locator": locator,
                 "path_source": path_source, "differs_from_disc1": original is None or digest != original["sha1"],
                 "compressed": compressed,
                 "read_size": image.top[locator["index"]][1] if locator["table"] == "top" else size}
        if locator["table"] == "field":
            entry["field_ids"] = ids[locator["index"] // 3]
        if candidates:
            entry["content_matches_disc1"] = sorted({e["path"] for e in candidates})
        if path.endswith(".ovl"):
            data = lzs_decompress(raw) if compressed else raw
            entry.update(overlay_sha1=sha1(data), overlay_size=len(data))
            loaded = lzs_decompress(raw[:entry["read_size"]]) if compressed and entry["read_size"] < len(raw) else data
            entry.update(loaded_sha1=sha1(loaded), loaded_size=len(loaded))
        entries.append(entry)
    entries.sort(key=lambda e: (e["sector"], e["path"]))
    result = inventory_header(image, reference["source"], layout)
    result["entries"] = entries
    result["missing_from_disc1"] = sorted(set(ref_by_path) - used_paths)
    return result


def collect_overlays(inventories, specs, provenance):
    groups = {}
    for inventory in inventories:
        for entry in inventory["entries"]:
            if not entry["path"].endswith(".ovl"):
                continue
            spec = specs[entry["path"]]
            digest = entry["overlay_sha1"]
            group = groups.setdefault(digest, {"name": spec["name"], "names": [], "paths": [], "discs": [],
                                               "sha1": digest, "size": entry["overlay_size"],
                                               "code_configured": spec["code_configured"],
                                               "load_address": spec["load_address"], "load_addresses": [],
                                               "compressed": entry["compressed"], "locations": []})
            if entry["overlay_size"] != group["size"]:
                raise ValueError("overlay digest has inconsistent sizes")
            for key, value in [("names", spec["name"]), ("paths", entry["path"]), ("discs", inventory["disc"])]:
                if value not in group[key]:
                    group[key].append(value)
            if spec["load_address"] not in group["load_addresses"]:
                group["load_addresses"].append(spec["load_address"])
            if len(group["load_addresses"]) > 1:
                group["load_address"] = None
            if group["compressed"] != entry["compressed"]:
                group["compressed"] = None
            group["locations"].append({"disc": inventory["disc"], "name": spec["name"], "path": entry["path"],
                                       "sector": entry["sector"], "size": entry["size"],
                                       "read_size": entry["read_size"], "compressed": entry["compressed"],
                                       "loaded_size": entry["loaded_size"], "loaded_sha1": entry["loaded_sha1"],
                                       "load_address": spec["load_address"], "sha1_stored": entry["sha1"]})
    return {"schema_version": 1, "source": provenance,
            "size_unit": "decompressed guest overlay image bytes, including embedded data",
            "overlays": sorted(groups.values(), key=lambda e: (e["name"], e["sha1"]))}


def image_sha1(image):
    digest = hashlib.sha1()
    for sector in range(0, image.record["size"] // 2048, 512):
        digest.update(image.read(sector, min(512 * 2048, image.record["size"] - sector * 2048)))
    return digest.hexdigest()


def check_coverage(image, entries):
    """Account for every IMG sector; uncovered space must be verified zero padding."""
    padding, end = [], 0
    spans = sorted((e["sector"], e["sector"] + (e["size"] + 2047) // 2048) for e in entries)
    spans.append((image.record["size"] // 2048, image.record["size"] // 2048))
    for start, stop in spans:
        if start < end:
            raise ValueError(f"overlapping physical file allocations at IMG sector {start}")
        if start > end:
            data = image.read(end, (start - end) * 2048)
            if any(data):
                raise ValueError(f"unindexed nonzero data at IMG sector {end}")
            padding.append({"sector": end, "size": len(data), "sha1": sha1(data), "fill": 0})
        end = stop
    return padding


SYMBOL = re.compile(r"^\s*([\w.$]+)\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)\s*;")


def parse_symbols(text):
    symbols = defaultdict(list)
    for line in text.splitlines():
        if line.lstrip().startswith(("//", "#")):
            continue
        match = SYMBOL.match(line)
        if match:
            name, address = match.groups()
            key = f"0x{int(address, 0):08x}"
            if name not in symbols[key]:
                symbols[key].append(name)
        elif "=" in line.split("//")[0] and not line.lstrip().startswith(("/*", "*")):
            raise ValueError(f"unparsed symbol declaration: {line}")
    return dict(symbols)


def convert_symbols(upstream, config, destination, provenance):
    files = sorted((upstream / "config").glob("symbols.*.txt"))
    if not files:
        raise ValueError("upstream config/symbols.*.txt files are missing")
    parsed = {f"config/{p.name}": parse_symbols(p.read_text()) for p in files}
    index = {"source": provenance, "scope": "disc 1 only; other-disc addresses and names are unverified",
             "source_files": {f"config/{p.name}": sha1(p.read_bytes()) for p in files}, "sets": {}}
    for binary in config["binaries"]:
        dependencies = binary["options"].get("symbol_addrs_path", [])
        merged = defaultdict(list)
        for path in dependencies:
            if path not in parsed:
                raise ValueError(f"missing upstream symbols: {path}")
            for address, names in parsed[path].items():
                merged[address] = list(dict.fromkeys(names + merged[address]))
        name = "main" if binary["name"] == "SLUS_008.92" else binary["name"]
        write_json(destination / f"{name}.json", {a: names[0] for a, names in sorted(merged.items())})
        index["sets"][name] = {"binary_sha1": binary["sha1"], "sources": dependencies,
                               "count": len(merged), "aliases": {a: ns for a, ns in merged.items() if len(ns) > 1}}
    # Preserve standalone declarations too (extern/shared may not be direct dependencies of a binary).
    for path, symbols in parsed.items():
        name = Path(path).name.removeprefix("symbols.").removesuffix(".txt")
        if name not in index["sets"]:
            write_json(destination / f"{name}.json", {a: names[0] for a, names in sorted(symbols.items())})
            index["sets"][name] = {"sources": [path], "count": len(symbols),
                                   "aliases": {a: ns for a, ns in symbols.items() if len(ns) > 1}}
    write_json(destination / "index.json", index)


def import_all(args):
    import yaml
    upstream = Path(args.upstream)
    commit = args.commit
    if args.unverified_cache and (commit or not args.inventory_only):
        raise ValueError("--unverified-cache requires --inventory-only and no --commit")
    if not commit and not args.unverified_cache:
        commit = subprocess.check_output(["git", "-C", str(upstream), "rev-parse", "HEAD"], text=True).strip()
    if not args.unverified_cache and not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("--commit must be a full 40-character commit SHA")
    lock_bytes = (upstream / LOCK).read_bytes()
    config_bytes = (upstream / "config/ff8.yaml").read_bytes()
    lock, config = yaml.safe_load(lock_bytes), yaml.safe_load(config_bytes)
    provenance = {"repository": SOURCE, "commit": commit, "license": "CC0-1.0",
                  "lock_sha1": sha1(lock_bytes), "config_sha1": sha1(config_bytes)}
    if args.unverified_cache:
        provenance["status"] = "unverified local cache; upstream commit and symbols unavailable"
    destination = Path(args.output)
    # Fail before importing discs if the symbol inputs are incomplete.
    missing = {s for b in config["binaries"] for s in b["options"].get("symbol_addrs_path", [])
               if not (upstream / s).is_file()}
    if missing and not args.inventory_only:
        raise ValueError(f"missing {len(missing)} upstream symbol files (first: {sorted(missing)[0]})")
    if len(args.cues) != 4:
        raise ValueError("import requires disc 1, 2, 3, 4 CUEs in that order")
    inventories = []
    reference = None
    for number, cue in enumerate(args.cues, 1):
        image = Image(cue)
        try:
            if image.number != number:
                raise ValueError(f"expected disc {number}, got {image.number}")
            if reference is None:
                reference = reference_inventory(image, lock, provenance)
                specs = overlay_specs(config, reference)
                reference["_field_ids"] = field_ids(image.layout())
                reference["_heads"] = {e["path"]: sha1(image.read(e["sector"], min(e["size"], 2048)))
                                       for e in reference["entries"]}
                inventory = reference
            else:
                inventory = other_inventory(image, reference)
            inventory["padding"] = check_coverage(image, inventory["entries"])
            inventory["image"]["sha1"] = image_sha1(image)
            inventories.append(inventory)
            public = {k: v for k, v in inventory.items() if not k.startswith("_")}
            write_json(destination / f"files-disc{number}.json", public)
            print(f"disc {number}: {len(inventory['entries'])} files, "
                  f"{sum(e['differs_from_disc1'] for e in inventory['entries'])} changed/new, "
                  f"{sum(e['path_source'] == 'generated' for e in inventory['entries'])} generated names", flush=True)
        finally:
            image.close()
    overlays = collect_overlays(inventories, specs, provenance)
    write_json(destination / "overlays-all.json", overlays)
    if not args.inventory_only:
        convert_symbols(upstream, config, destination / "symbols", provenance)
    else:
        write_json(destination / "symbols/index.json", {
            "source": provenance, "status": "not_imported", "sets": {},
            "reason": "symbol conversion was skipped; no address-to-name maps have been imported",
            "missing_source_files": sorted(missing)})
    print(f"unique overlays: {len(overlays['overlays'])}; guest image bytes: "
          f"{sum(e['size'] for e in overlays['overlays'])}")


def load_inventory(image, directory):
    inventory = json.loads((directory / f"files-disc{image.number}.json").read_text())
    if inventory["image"]["lba"] != image.record["lba"] or inventory["image"]["size"] != image.record["size"]:
        raise ValueError("disc IMG does not match inventory layout")
    return inventory


def select_entry(inventory, query, directory):
    normalized = query.strip("/").lower()
    if normalized.startswith("@"):
        matches = [e for e in inventory["entries"] if entry_id(e["locator"]) == normalized[1:]]
        if len(matches) != 1:
            raise ValueError(f"expected one table record for {query!r}, found {len(matches)}")
        return matches[0]
    exact = [e for e in inventory["entries"] if e["path"].lower() == normalized]
    if exact:
        if len({e["sha1"] for e in exact}) > 1:
            raise ValueError("path has different contents at multiple locations; select @table:index")
        return exact[0]
    catalog = json.loads((directory / "overlays-all.json").read_text())
    paths = {loc["path"] for overlay in catalog["overlays"] for loc in overlay["locations"]
             if loc["disc"] == inventory["disc"] and
             (normalized == loc["name"].lower() or
              normalized == Path(loc["path"]).name.lower() or
              normalized == Path(loc["path"]).stem.lower())}
    matches = [e for e in inventory["entries"] if e["path"] in paths]
    if len(matches) != 1:
        raise ValueError(f"expected one file for {query!r}, found {len(matches)}")
    return matches[0]


def verify_inventory(image, inventory, directory):
    layout = image.layout()
    count, overlays = 0, 0
    for entry in inventory["entries"]:
        lba, size = image.pair(entry["locator"], layout)
        if (lba - image.record["lba"], size) != (entry["sector"], entry["size"]):
            raise ValueError(f"table location mismatch: {entry['path']}")
        raw, data = raw_and_overlay(image, entry)
        if sha1(raw) != entry["sha1"]:
            raise ValueError(f"file SHA-1 mismatch: {entry['path']}")
        if data is not None:
            if sha1(data) != entry["overlay_sha1"] or len(data) != entry["overlay_size"]:
                raise ValueError(f"overlay SHA-1/size mismatch: {entry['path']}")
            loaded = (lzs_decompress(raw[:entry["read_size"]])
                      if entry["compressed"] and entry["read_size"] < len(raw) else data)
            if sha1(loaded) != entry["loaded_sha1"] or len(loaded) != entry["loaded_size"]:
                raise ValueError(f"loader-visible overlay SHA-1/size mismatch: {entry['path']}")
            overlays += 1
        count += 1
    catalog = json.loads((directory / "overlays-all.json").read_text())
    entries = {e["path"]: e for e in inventory["entries"] if e["path"].endswith(".ovl")}
    seen = set()
    for overlay in catalog["overlays"]:
        for loc in overlay["locations"]:
            if loc["disc"] != image.number:
                continue
            entry = entries[loc["path"]]
            expected = (entry["sector"], entry["size"], entry["read_size"], entry["compressed"],
                        entry["sha1"], entry["overlay_sha1"], entry["overlay_size"],
                        entry["loaded_sha1"], entry["loaded_size"])
            actual = (loc["sector"], loc["size"], loc["read_size"], loc["compressed"], loc["sha1_stored"],
                      overlay["sha1"], overlay["size"], loc["loaded_sha1"], loc["loaded_size"])
            if actual != expected or loc["path"] in seen:
                raise ValueError(f"overlay catalog mismatch: {loc['path']}")
            seen.add(loc["path"])
    if seen != set(entries):
        raise ValueError("overlay catalog coverage mismatch")
    if check_coverage(image, inventory["entries"]) != inventory["padding"]:
        raise ValueError("IMG padding coverage mismatch")
    if image_sha1(image) != inventory["image"]["sha1"]:
        raise ValueError("logical IMG SHA-1 mismatch")
    actual_exe = image.disc.find(inventory["exe"]["name"])
    if sha1(image.disc.read(actual_exe["lba"], actual_exe["size"])) != inventory["exe"]["sha1"]:
        raise ValueError("main executable SHA-1 mismatch")
    return count, overlays


def scan_disc1_matches(image, inventory, reference_cue, directory):
    """Independently locate unchanged files by content at every sector boundary, without using target tables."""
    reference = Image(reference_cue)
    try:
        if reference.number != 1:
            raise ValueError("--scan-disc1 must name disc 1")
        ref = load_inventory(reference, directory)
        fingerprints = defaultdict(dict)
        for entry in ref["entries"]:
            if entry["size"] < 2048:
                continue
            head = reference.read(entry["sector"], 2048)
            # Exclude padding-like prefixes to avoid hashing large candidates at every blank sector.
            if len(set(head)) < 8:
                continue
            fingerprints[sha1(head)][(entry["size"], entry["sha1"])] = True
    finally:
        reference.close()
    found = set()
    for sector in range(image.record["size"] // 2048):
        head = sha1(image.read(sector, 2048))
        for size, digest in fingerprints.get(head, {}):
            if sector * 2048 + size <= image.record["size"] and sha1(image.read(sector, size)) == digest:
                found.add((sector, size, digest))
    expected = {(e["sector"], e["size"], e["sha1"]) for e in inventory["entries"]
                if (e["size"], e["sha1"]) in fingerprints.get(sha1(image.read(e["sector"], min(e["size"], 2048))), {})}
    if not expected <= found:
        raise ValueError("sector-boundary content search missed an inventory entry")
    inventory_extents = {(e["sector"], e["size"], e["sha1"]) for e in inventory["entries"]}
    # Matches wholly within another file are reported separately; they are not additional indexed files.
    print(f"disc {image.number}: sector scan found {len(found & inventory_extents)} indexed extents identical to disc 1; "
          f"{len(found - inventory_extents)} additional content matches inside/past indexed extents")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    imp = sub.add_parser("import", help="regenerate metadata from a pinned ff8-decomp checkout and four discs")
    imp.add_argument("--upstream", required=True)
    imp.add_argument("--commit", help="full source SHA (default: upstream checkout HEAD)")
    imp.add_argument("--output", default=str(METADATA))
    imp.add_argument("--inventory-only", action="store_true", help="skip symbol conversion when inputs are unavailable")
    imp.add_argument("--unverified-cache", action="store_true", help="inventory-only fallback; explicitly record an unknown commit")
    imp.add_argument("cues", nargs="+")
    for command in ["list", "get", "verify"]:
        cmd = sub.add_parser(command)
        cmd.add_argument("cue")
        cmd.add_argument("--metadata", type=Path, default=METADATA)
        if command == "list":
            cmd.add_argument("pattern", nargs="?", default="")
            cmd.add_argument("--overlays", action="store_true")
            cmd.add_argument("--json", action="store_true")
        if command == "get":
            cmd.add_argument("query", help="full disc path or overlay name (e.g. field, card, effect_001, mag001)")
            cmd.add_argument("out", type=Path)
            cmd.add_argument("--raw", action="store_true", help="copy stored bytes, including compressed data")
            cmd.add_argument("--loaded", action="store_true", help="decode only the bytes requested by the loader")
            cmd.add_argument("--decompress", action="store_true", help="LZS-decompress a non-overlay file")
        if command == "verify":
            cmd.add_argument("--scan-disc1", help="also search every IMG sector boundary for unchanged disc-1 content")
    args = parser.parse_args(argv)
    try:
        if args.command == "import":
            import_all(args)
            return 0
        image = Image(args.cue)
        try:
            inventory = load_inventory(image, args.metadata)
            if args.command == "list":
                entries = [e for e in inventory["entries"] if args.pattern.lower() in e["path"].lower() and
                           (not args.overlays or e["path"].endswith(".ovl"))]
                if args.json:
                    print(json.dumps(entries, indent=2))
                else:
                    for entry in entries:
                        print(f"{entry['sector']:7d} {entry['size']:9d} {entry['sha1']} {entry['path']}")
            elif args.command == "verify":
                count, overlays = verify_inventory(image, inventory, args.metadata)
                print(f"disc {image.number}: verified {count} file SHA-1s/table locations and {overlays} overlay images/catalog records")
                if args.scan_disc1:
                    scan_disc1_matches(image, inventory, args.scan_disc1, args.metadata)
            else:
                entry = select_entry(inventory, args.query, args.metadata)
                raw, overlay = raw_and_overlay(image, entry)
                if sha1(raw) != entry["sha1"]:
                    raise ValueError(f"file SHA-1 mismatch: {entry['path']}")
                if sum([args.raw, args.loaded, args.decompress]) > 1:
                    raise ValueError("--raw, --loaded and --decompress are mutually exclusive")
                data = raw if args.raw else overlay if overlay is not None else raw
                if args.loaded:
                    if overlay is None:
                        raise ValueError("--loaded requires a code overlay")
                    data = lzs_decompress(raw[:entry["read_size"]]) if entry["compressed"] else raw
                    if sha1(data) != entry["loaded_sha1"] or len(data) != entry["loaded_size"]:
                        raise ValueError("loader-visible overlay hash/size mismatch")
                if args.decompress and overlay is None:
                    if len(raw) < 4 or struct.unpack_from("<I", raw)[0] + 4 > len(raw):
                        raise ValueError("file does not contain a complete LZS stream")
                    data = lzs_decompress(raw)
                if overlay is not None and sha1(overlay) != entry["overlay_sha1"]:
                    raise ValueError("decompressed overlay hash mismatch")
                # Game data must stay in ignored work/build trees or outside this repository.
                out = args.out.resolve()
                if out.is_relative_to(ROOT) and not (out.is_relative_to(ROOT / "work") or out.is_relative_to(ROOT / "build")):
                    raise ValueError("extraction inside the repository is restricted to work/ or build/")
                args.out.write_bytes(data)
                print(f"wrote {len(data)} bytes; SHA-1 {sha1(data)}")
        finally:
            image.close()
        return 0
    except (OSError, ValueError, KeyError, StopIteration, struct.error, subprocess.CalledProcessError) as error:
        print(f"ff8files: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
