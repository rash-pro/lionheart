"""Player legal checks use complete recompiler overlay images and compiler inputs."""
import json
from pathlib import Path
import random
import re
import shlex
import struct
from types import SimpleNamespace

import pytest

import recomp.disc
import recomp.overlays
from test_host_player import real_player
from tools.check_player_binary import _check_image, check, check_compile_commands, check_discs, guest_windows, main


@pytest.fixture(autouse=True)
def synthetic_default_catalog(monkeypatch):
    # Legacy-specific cases stay independent from the separately tested catalog
    # extractor. A zero-filled synthetic image has no qualifying guest windows.
    monkeypatch.setattr(recomp.overlays, "compile_catalog",
                        lambda *_: (None, [(128, SimpleNamespace(data=bytes(64)), {})]))


def database(tmp_path, *arguments, source="main.c", command=None):
    path = tmp_path / "compile_commands.json"
    unit = {"directory": str(tmp_path), "file": source}
    if command is None:
        unit["arguments"] = ["cc", *arguments, "-c", source]
    else:
        unit["command"] = command
    path.write_text(json.dumps([unit]))
    return path


@pytest.mark.parametrize("length", [0, 1, 63, 64, 65, 127, 200])
def test_binary_index_matches_direct_scan_and_first_match_offset(length):
    text = random.Random(1900 + length).randbytes(length)
    binary = bytes(9) + text + bytes(3) + text
    index = set(guest_windows(binary))
    if length < 64:
        assert _check_image(binary, text, index) == check(binary, text)
    else:
        for operation in (lambda: check(binary, text), lambda: _check_image(binary, text, index)):
            with pytest.raises(ValueError, match="binary offset 9$"):
                operation()
    clean = bytes(200)
    assert _check_image(clean, text, set(guest_windows(clean))) == check(clean, text)


def test_overlay_scans_every_disc_and_full_image(tmp_path, monkeypatch):
    calls = []
    payload = random.Random(16).randbytes(193)

    def extract(disc, work):
        calls.append((disc, work))
        work.mkdir(parents=True)
        (work / "private.bin").write_bytes(payload)
        # Byte 101 deliberately lies beyond any synthetic code range.
        image = SimpleNamespace(data=payload, code_ranges=((0, 4),))
        return None, [(len(calls) * 32, image, {})]

    monkeypatch.setattr(recomp.disc, "extract_disc", extract)
    discs = [Path(f"disc{i}.cue") for i in range(1, 5)]
    result = check_discs(bytes(200), discs, tmp_path / "extracted")
    assert result["legacy_full_images"] == 4 and result["catalog_loaded_images"] == 1
    assert result["overlay_images"] == 5
    assert [disc for disc, _ in calls] == discs
    assert all(str(work).startswith(str(tmp_path / "extracted")) and not work.exists()
               for _, work in calls)
    assert all(row["overlays"][0]["image_bytes"] == len(payload) for row in result["discs"])
    with pytest.raises(ValueError, match=r"disc1.cue, overlay 160:.*offset 9"):
        check_discs(bytes(9) + payload[101:165], discs, tmp_path / "extracted")
    assert not list((tmp_path / "extracted").iterdir())


def test_overlay_match_on_last_disc_is_detected(tmp_path, monkeypatch):
    payload = random.Random(17).randbytes(64)
    calls = []

    def extract(disc, work):
        calls.append(disc)
        return None, [(96, SimpleNamespace(data=payload if len(calls) == 4 else bytes(64)), {})]

    monkeypatch.setattr(recomp.disc, "extract_disc", extract)
    with pytest.raises(ValueError, match=r"disc4.bin, overlay 96"):
        check_discs(payload, [Path(f"disc{i}.bin") for i in range(1, 5)], tmp_path)
    assert len(calls) == 4


def test_empty_overlay_extraction_fails_closed(tmp_path, monkeypatch):
    monkeypatch.setattr(recomp.disc, "extract_disc", lambda *_: (None, []))
    with pytest.raises(ValueError, match="no overlay images"):
        check_discs(bytes(64), [Path("disc.cue")], tmp_path)


def test_default_catalog_adds_effect_images_and_uses_all_discs(tmp_path, monkeypatch):
    payload = random.Random(19).randbytes(193)
    discs = [Path(f"disc{i}.cue") for i in range(1, 5)]
    calls = []
    monkeypatch.setattr(recomp.disc, "extract_disc",
                        lambda *_: (None, [(0, SimpleNamespace(data=bytes(64)), {})]))

    def catalog(cues, work):
        calls.append((list(cues), work))
        work.mkdir(parents=True)
        (work / "effect.bin").write_bytes(payload)
        return None, [(128, SimpleNamespace(data=payload), {}),
                      (129, SimpleNamespace(data=bytes(80)), {})]

    monkeypatch.setattr(recomp.overlays, "compile_catalog", catalog)
    result = check_discs(bytes(200), discs, tmp_path / "extracted")
    assert result["legacy_full_images"] == 4 and result["catalog_loaded_images"] == 2
    assert result["overlay_images"] == 6
    assert [row["index"] for row in result["catalog"]] == [128, 129]
    assert calls[0][0] == discs and not calls[0][1].exists()
    with pytest.raises(ValueError, match=r"catalog overlay 128:.*offset 9"):
        check_discs(bytes(9) + payload[101:165], discs, tmp_path / "extracted")
    assert not list((tmp_path / "extracted").iterdir())


def test_empty_default_catalog_extraction_fails_closed(tmp_path, monkeypatch):
    monkeypatch.setattr(recomp.disc, "extract_disc",
                        lambda *_: (None, [(0, SimpleNamespace(data=bytes(64)), {})]))
    monkeypatch.setattr(recomp.overlays, "compile_catalog", lambda *_: (None, []))
    with pytest.raises(ValueError, match="default recompiler catalog extraction produced no overlay images"):
        check_discs(bytes(64), [Path("disc.cue")], tmp_path)


@pytest.mark.parametrize("flag", ["-I", "-isystem", "-iquote", "-idirafter", "-include",
                                 "-imacros", "-isystem-after", "-iframework", "-F",
                                 "--include-directory", "/I", "/FI", "/external:I"])
@pytest.mark.parametrize("attached", [False, True])
def test_catalog_include_paths_are_rejected(tmp_path, flag, attached):
    path = "generated-exact/headers" if flag not in ("-include", "-imacros", "/FI") else "generated-exact/catalog.h"
    arguments = [flag + ("=" if flag == "--include-directory" else "") + path] if attached else [flag, path]
    with pytest.raises(ValueError, match="generated catalog include"):
        check_compile_commands(database(tmp_path, *arguments))


@pytest.mark.parametrize("source", ["generated-exact/catalog.c", "gen/overlay_2_001.c", "dispatch_table.c",
                                    "interpreter_table.c", "functions_001.c"])
def test_catalog_sources_are_rejected(tmp_path, source):
    with pytest.raises(ValueError, match="generated catalog source"):
        check_compile_commands(database(tmp_path, source=source))


def test_named_catalog_directory_and_symlink_are_rejected(tmp_path):
    catalog = tmp_path / "catalog"
    catalog.mkdir()
    link = tmp_path / "alias"
    link.symlink_to(catalog, target_is_directory=True)
    with pytest.raises(ValueError, match="generated catalog include"):
        check_compile_commands(database(tmp_path, "-Ialias"), [catalog])
    (tmp_path / "CMakeCache.txt").write_text(f"GENERATED_DIR:PATH={catalog}\n")
    with pytest.raises(ValueError, match="generated catalog source"):
        check_compile_commands(database(tmp_path, source="alias/module.c"))


def test_response_and_forwarded_include_options_are_checked(tmp_path):
    (tmp_path / "outer.rsp").write_text("@inner.rsp")
    (tmp_path / "inner.rsp").write_text("-Xclang -include -Xclang 'generated-exact/with spaces.h'")
    with pytest.raises(ValueError, match="generated catalog include"):
        check_compile_commands(database(tmp_path, "@outer.rsp"))
    with pytest.raises(ValueError, match="generated catalog include"):
        check_compile_commands(database(tmp_path, "-Wp,-I,generated-exact"))


def test_unreadable_and_recursive_responses_fail_closed(tmp_path):
    with pytest.raises(ValueError, match="cannot read compiler response file"):
        check_compile_commands(database(tmp_path, "@absent.rsp"))
    (tmp_path / "cycle.rsp").write_text("@cycle.rsp")
    with pytest.raises(ValueError, match="recursive compiler response file"):
        check_compile_commands(database(tmp_path, "@cycle.rsp"))


def test_clean_commands_and_response_count(tmp_path):
    (tmp_path / "headers.rsp").write_text('-I "include with spaces" -isystem /usr/include -include config.h')
    result = check_compile_commands(database(tmp_path, "@headers.rsp", source="player_catalog.c"))
    assert result["passed"] and result["include_paths_checked"] == 3
    assert result["response_files_checked"] == 1
    assert check_compile_commands(database(tmp_path, command="cc -I'include with spaces' -c main.c"))["passed"]


def test_cli_keeps_exe_check_and_adds_disc_and_compile_checks(tmp_path, monkeypatch, capsys):
    payload = random.Random(18).randbytes(80)
    exe = bytearray(2048) + payload
    exe[:8] = b"PS-X EXE"
    struct.pack_into("<I", exe, 28, len(payload))
    exe_path, binary = tmp_path / "game.exe", tmp_path / "player"
    exe_path.write_bytes(exe)
    binary.write_bytes(bytes(200))
    monkeypatch.setattr(recomp.disc, "extract_disc", lambda *_: (None, [(0, SimpleNamespace(data=payload), {})]))
    main([str(binary), str(exe_path), "--disc", "disc.cue", "--compile-commands", str(database(tmp_path)),
          "--extract-dir", str(tmp_path / "extracted")])
    receipt = json.loads(capsys.readouterr().out)
    assert receipt["passed"] and receipt["overlays"]["legacy_full_images"] == 1
    assert receipt["overlays"]["catalog_loaded_images"] == 1
    assert receipt["compile_commands"]["passed"]
    assert "sha" not in json.dumps(receipt) and "hash" not in json.dumps(receipt)
    assert check(binary.read_bytes(), payload)["passed"]


def test_windows_drive_paths_match_explicit_catalog_root(tmp_path):
    path = database(tmp_path, "-IC:\\game\\catalog", source="main.c")
    with pytest.raises(ValueError, match="generated catalog include"):
        check_compile_commands(path, ["C:\\game\\catalog"])


def test_player_compiles_imgui_without_its_unused_default_font(real_player):
    database = real_player.parent / "compile_commands.json"
    units = json.loads(database.read_text())
    draw_units = [unit for unit in units if Path(unit["file"]).name == "imgui_draw.cpp"]
    assert len(draw_units) == 1
    arguments = draw_units[0].get("arguments") or shlex.split(draw_units[0]["command"])
    assert "-DIMGUI_DISABLE_DEFAULT_FONT" in arguments
    # Compare with the independently vendored upstream font bytes, not any game
    # window or a game-derived allowlist. The application supplies Chakra/Spleen.
    source = (Path(__file__).resolve().parents[1] / "third_party/imgui/imgui_draw.cpp").read_text()
    match = re.search(r"proggy_clean_ttf_compressed_data\[(\d+)\]\s*=\s*\{([^}]*)\}", source)
    assert match
    font = bytes(map(int, re.findall(r"\b\d+\b", match[2])))
    assert len(font) == int(match[1])
    assert font not in real_player.read_bytes()
