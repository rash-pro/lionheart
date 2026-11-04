"""Compile public disc identity facts, never executable bytes, into a host header."""
import json
from pathlib import Path
import sys


def header(game):
    exe = json.loads((game / "exe-identity.json").read_text())
    lines = ["/* Identity facts only. See JSON metadata in games/ff8-usa. */",
             f"#define HOST_EXE_BASE {exe['base']}u",
             f"#define HOST_EXE_SIZE {exe['size']}u",
             f"#define HOST_EXE_FNV 0x{exe['fnv1a']}u",
             f"#define HOST_EXE_FILE_SIZE {exe['file_size']}u",
             f'#define HOST_EXE_SHA1 "{exe["sha1"]}"',
             "#ifdef HOST_DISC_TABLE",
             "static const host_disc_metadata_t host_disc_metadata[4] = {"]
    for n in range(1, 5):
        disc = json.loads((game / f"files-disc{n}.json").read_text())
        image = disc["image"]
        lines.append('    {"%s", "%s", "%s", %du, %du, "%s"},' % (
            disc["exe"]["name"], disc["exe"]["sha1"], image["name"],
            image["lba"], image["size"], image["sha1"]))
    return "\n".join([*lines, "};", "#endif", ""])


if __name__ == "__main__":
    Path(sys.argv[2]).write_text(header(Path(sys.argv[1])))
