"""Static command fields must select direct APIs without a runtime decoder."""
from recomp.decode import decode
from recomp.emit import effects


def test_command_fields_and_ignored_bits():
    cases = [
        (0x4A080030, "gte_rtpt(c, 12, 0);"),
        (0x4A000401, "gte_rtps(c, 0, 1);"),
        (0x4A0FF406, "gte_nclip(c);"),
        (0x4A0FF42D, "gte_avsz3(c);"),
        (0x4A0FF42E, "gte_avsz4(c);"),
        (0x4A0FE412, "gte_mvmva(c, 12, 1, 3, 3, 3);"),
        (0x4A000012, "gte_mvmva(c, 0, 0, 0, 0, 0);"),
        (0x4A00003A, "gte_cmd(c, 0x4a00003au);"),
    ]
    for word, expected in cases:
        lines, dst, value, load, loaded = effects(decode(word, 0x80010000))
        assert lines[1:] == [expected]
        assert (dst, value, load, loaded) == (0, "0", 0, "0")
