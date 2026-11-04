"""List / extract the files packed in FF8DISCn.IMG (Final Fantasy VIII, PS1, USA).

    python tools/ff8img.py DISC.cue ls [-v]
    python tools/ff8img.py DISC.cue get INDEX OUT [--raw]

Format (see ARCHITECTURE.md): sector 0 of the IMG holds a table of
(u32 absolute disc LBA, u32 size in bytes) pairs, terminated by (0, 0).  Index N of the table is the file index
the game code uses.  Some files are LZS-compressed (u32 LE compressed size, then LZSS with a 4096-byte ring
buffer, flag bytes LSB first, 1 = literal, 0 = 2-byte reference).  `get` writes the decompressed data unless --raw.

Per-file roles / load addresses below are facts recovered from SLUS_008.92 (loader code at 0x80011a60 and
0x80021358, overlay table at 0x80053c5c) and from the overlays themselves; they are keyed by (index, size) so they
are only applied when the table matches the disc-1 table.  Everything else is guessed from content.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from psxdisc import Disc  # noqa: E402

SECTOR = 2048
IMG_NAME = "FF8DISC1.IMG"

MENU = ["menumain", "menucfg", "menupty", "menusts", "menuabl", "menushop", "menuext", "menuitem", "menumgc",
        "menugf", "menujnc2", "menusav", "menucrd", "menututo", "menutmag", "menutips", "menutest"]
# menu overlay load address (low bits of the table word are flags 1/2, masked off)
MENU_ADDR = [0x801EF800, 0x801E5800, 0x801E5800, 0x801E5800, 0x801E2800, 0x801E5800, 0x801E5800, 0x801E2800,
             0x801E5800, 0x801E5800, 0x801E5800, 0x801E2800, 0x801E5800, 0x801E2800, 0x801E5800, 0x801E5800,
             0x801E5800]

# index -> (size, name, load address, compressed, note)   [size = table size, used as a sanity key]
KNOWN = {
    0: (2450, "ovl_boot0", 0x80098000, False, "small raw code module; loaded by 0x80011c6c"),
    1: (2530, "ovl_small1", 0x80098000, True, "LZS -> 4784 B code; entry 0x80098fd4 called after load"),
    2: (102523, "ovl_field", 0x80098000, True, "LZS -> 191872 B; 'SmInitEventAll', 'sizeof(actor)'; main mode 1/2"),
    21: (2263040, "mngrp.bin", None, False, "menu group data (name from exe table, order inferred)"),
    22: (2812, "init.out?", None, False, "data, name inferred from exe table order only"),
    23: (683741, "ovl_mode8", 0x80098000, True, "LZS -> 1092672 B; launcher state 8, entry 0x80098304"),
    24: (80376, "ovl_mode4", 0x80098000, False, "raw; launcher state 4, entry 0x80098238; build date + debug strings"),
    25: (347208, "ovl_mode3", 0x80098000, False, "raw; launcher state 3, entry 0x80099d30"),
    26: (118482, "ovl_world", 0x80098000, True, "LZS -> 198197 B; world map ('\\DAT\\WMSET.OBJ')"),
}
for k, (nm, ad) in enumerate(zip(MENU, MENU_ADDR)):
    KNOWN[4 + k] = (None, nm + ".ovl", ad, False, "menu overlay; raw")


def read_table(disc):
    rec = disc.find(IMG_NAME)
    t = disc.read(rec["lba"], SECTOR)
    ents = []
    for i in range(SECTOR // 8):
        lba, size = struct.unpack_from("<II", t, i * 8)
        if lba == 0 and size == 0:
            break
        ents.append((lba, size))
    return rec["lba"], ents


def lzs_decompress(b):
    """FF7/FF8-style LZS: u32 compressed size, then LZSS (ring 4096, start 0xFEE, zero-filled)."""
    n = struct.unpack_from("<I", b)[0]
    end = min(len(b), 4 + n)
    ring = bytearray(4096)
    r, p, out = 0xFEE, 4, bytearray()
    while p < end:
        flags = b[p]
        p += 1
        for bit in range(8):
            if p >= end:
                break
            if flags >> bit & 1:
                c = b[p]
                p += 1
                out.append(c)
                ring[r] = c
                r = (r + 1) & 4095
            else:
                if p + 1 >= end:
                    p = end
                    break
                b1, b2 = b[p], b[p + 1]
                p += 2
                off, ln = b1 | ((b2 & 0xF0) << 4), (b2 & 15) + 3
                for k in range(ln):
                    c = ring[(off + k) & 4095]
                    out.append(c)
                    ring[r] = c
                    r = (r + 1) & 4095
    return bytes(out)


def looks_lzs(b):
    if len(b) < 8:
        return False
    return abs(struct.unpack_from("<I", b)[0] - (len(b) - 4)) <= 16


def code_density(b):
    n = len(b) // 4
    if n == 0:
        return 0.0
    w = struct.unpack_from(f"<{n}I", b)
    hits = sum(1 for x in w if x == 0x03E00008 or x >> 16 == 0x27BD)
    return hits * 1000 / n  # per mille words


def guess_type(b):
    if not b:
        return "empty"
    if looks_lzs(b):
        return "lzs"
    if code_density(b) > 3:
        return "code"
    h = b[:4]
    if h == b"AKAO":
        return "akao-sound"
    if h == bytes([2, 0, 0, 0]) and len(b) > 8 and struct.unpack_from("<I", b, 4)[0] == 0x14:
        return "stage-data?"
    if h == bytes([0x10, 0, 0, 0]):
        return "tim?"
    return "data"


def describe(disc, idx, lba, size, base):
    head = disc.read(lba, min(size, SECTOR * 4)) if size else b""
    known = KNOWN.get(idx)
    if known and known[0] not in (None, size):
        known = None
    comp = known[3] if known else looks_lzs(head)
    if known:
        typ = "data" if known[1] in ("mngrp.bin", "init.out?") else "code"
    else:
        typ = guess_type(head)
        if typ == "lzs":
            typ = "lzs-data?"
    return {"idx": idx, "lba": lba, "img_off": (lba - base) * SECTOR, "size": size, "comp": bool(comp),
            "type": typ, "name": known[1] if known else "", "load": known[2] if known else None,
            "note": known[4] if known else ""}


def extract(disc, lba, size, comp):
    raw = disc.read(lba, size)
    return lzs_decompress(raw) if comp else raw


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    disc = Disc(sys.argv[1])
    base, ents = read_table(disc)
    cmd = sys.argv[2]
    rows = [describe(disc, i, l, s, base) for i, (l, s) in enumerate(ents)]
    if cmd == "ls":
        verbose = "-v" in sys.argv
        print(f"# {IMG_NAME} at LBA {base}, {len(ents)} files; img_off = (lba-{base})*2048")
        print(f"{'idx':>3} {'lba':>6} {'img_off':>10} {'size':>8} comp {'type':<10} {'load':<10} name")
        for r in rows:
            load = f"0x{r['load']:08X}" if r["load"] else "-"
            print(f"{r['idx']:>3} {r['lba']:>6} {r['img_off']:>10} {r['size']:>8} {'LZS ' if r['comp'] else '-   '} "
                  f"{r['type']:<10} {load:<10} {r['name']}{('  # ' + r['note']) if verbose and r['note'] else ''}")
    elif cmd == "get":
        r = rows[int(sys.argv[3])]
        data = extract(disc, r["lba"], r["size"], r["comp"] and "--raw" not in sys.argv)
        Path(sys.argv[4]).write_bytes(data)
        print(f"wrote {len(data)} bytes ({'decompressed' if r['comp'] and '--raw' not in sys.argv else 'raw'})")
    else:
        print(__doc__)
        return 1


if __name__ == "__main__":
    sys.exit(main() or 0)
