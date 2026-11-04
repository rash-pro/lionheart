"""Read files from a PS1 disc image (raw MODE2/2352 .bin, as described by a .cue) without extracting it.

python tools/psxdisc.py DISC.cue ls
python tools/psxdisc.py DISC.cue get PATH OUT
"""
import struct
import sys
from pathlib import Path

RAW = 2352
DATA = 24  # sync (12) + header (4) + subheader (8) before the 2048 user bytes of a Mode 2 Form 1 sector


class Disc:
    def __init__(self, cue):
        cue = Path(cue)
        line = next(l for l in cue.read_text().splitlines() if l.strip().startswith("FILE"))
        self.bin = open(cue.parent / line.split('"')[1], "rb")
        self.root = self._record(self.sector(16)[156:190])

    def sector(self, lba):
        self.bin.seek(lba * RAW)
        return self.bin.read(RAW)[DATA:DATA + 2048]

    def read(self, lba, size):
        out = bytearray()
        while len(out) < size:
            out += self.sector(lba)
            lba += 1
        return bytes(out[:size])

    @staticmethod
    def _record(raw):
        lba, size = struct.unpack_from("<I", raw, 2)[0], struct.unpack_from("<I", raw, 10)[0]
        name = raw[33:33 + raw[32]].decode("ascii", "replace").split(";")[0]
        return {"name": name, "lba": lba, "size": size, "dir": bool(raw[25] & 2)}

    def listdir(self, entry):
        data, pos, out = self.read(entry["lba"], entry["size"]), 0, []
        while pos < len(data):
            length = data[pos]
            if length == 0:  # records never cross a sector boundary
                pos = (pos // 2048 + 1) * 2048
                continue
            rec = self._record(data[pos:pos + length])
            if rec["name"] not in ("\x00", "\x01"):
                out.append(rec)
            pos += length
        return out

    def walk(self, entry=None, prefix=""):
        for rec in self.listdir(entry or self.root):
            path = f"{prefix}/{rec['name']}"
            yield path, rec
            if rec["dir"]:
                yield from self.walk(rec, path)

    def find(self, path):
        want = path.strip("/").upper()
        return next(rec for p, rec in self.walk() if p.strip("/").upper() == want)


if __name__ == "__main__":
    disc = Disc(sys.argv[1])
    if sys.argv[2] == "ls":
        for path, rec in disc.walk():
            print(f"{rec['lba']:>8} {rec['size']:>11} {path}{'/' if rec['dir'] else ''}")
    elif sys.argv[2] == "get":
        rec = disc.find(sys.argv[3])
        Path(sys.argv[4]).write_bytes(disc.read(rec["lba"], rec["size"]))
