#!/usr/bin/env python3
"""
Clean-room MIT local HD 3D model pack helper & FF8 Remastered (.zzz) inspector
for Lionheart.

Zero Square Enix assets or proprietary code are included. This utility lets users:
  1. Generate a synthetic glTF 2.0 (.glb) + manifest.json test pack (--make-test-pack)
     to immediately verify Lionheart's Vulkan HD 3D model pipeline end-to-end.
  2. Validate any community or local HD model pack directory (--validate-pack).
  3. Inspect or extract model assets from their own legally-owned Steam copy of
     Final Fantasy VIII Remastered's local main.zzz archive (--list-zzz / --extract-zzz).
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import struct
import sys
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple


@dataclass
class ZzzEntry:
    path: str
    offset: int
    size: int


def _validate_safe_relpath(raw_path: str) -> str:
    norm = raw_path.replace("\\", "/").lstrip("/")
    if not norm or ":" in norm:
        raise ValueError(f"Unsafe archive entry path: {raw_path!r}")
    parts = norm.split("/")
    if any(p in ("", ".", "..") for p in parts):
        raise ValueError(f"Unsafe archive entry path: {raw_path!r}")
    return "/".join(parts)


def parse_zzz(path: Path) -> List[ZzzEntry]:
    """Parse a clean-room FF8 Remastered .zzz archive index.

    Public flat archive layout:
      uint32_le file_count
      repeat file_count:
        uint32_le path_len
        uint8[path_len] utf8_path
        uint64_le data_offset
        uint32_le data_size
    """
    file_size = path.stat().st_size
    if file_size < 4:
        raise ValueError("Truncated .zzz header")

    entries: List[ZzzEntry] = []
    with path.open("rb") as f:
        (count,) = struct.unpack("<I", f.read(4))
        if count > 1_000_000:
            raise ValueError(f"Unreasonable .zzz entry count: {count}")

        for _ in range(count):
            plen_raw = f.read(4)
            if len(plen_raw) != 4:
                raise ValueError("Truncated .zzz entry path length")
            (plen,) = struct.unpack("<I", plen_raw)
            if plen == 0 or plen > 4096:
                raise ValueError(f"Invalid .zzz path length: {plen}")
            raw_name = f.read(plen)
            if len(raw_name) != plen:
                raise ValueError("Truncated .zzz entry path")
            meta_raw = f.read(12)
            if len(meta_raw) != 12:
                raise ValueError("Truncated .zzz entry metadata")
            offset, size = struct.unpack("<QI", meta_raw)
            rel = _validate_safe_relpath(raw_name.decode("utf-8", errors="strict"))
            entries.append(ZzzEntry(path=rel, offset=offset, size=size))

        header_end = f.tell()

    for e in entries:
        if e.offset < header_end or e.offset > file_size or e.size > file_size - e.offset:
            raise ValueError(
                f".zzz entry {e.path!r} out of bounds (offset={e.offset}, size={e.size}, file_size={file_size})"
            )
    return entries


def create_synthetic_zzz(path: Path, files: Dict[str, bytes]) -> None:
    """Write a valid .zzz archive from a mapping of relative paths to raw bytes."""
    encoded: List[Tuple[bytes, bytes]] = []
    header_size = 4
    for rel, payload in files.items():
        safe = _validate_safe_relpath(rel)
        name_bytes = safe.encode("utf-8")
        encoded.append((name_bytes, payload))
        header_size += 4 + len(name_bytes) + 12

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as f:
        f.write(struct.pack("<I", len(encoded)))
        cursor = header_size
        for name_bytes, payload in encoded:
            f.write(struct.pack("<I", len(name_bytes)))
            f.write(name_bytes)
            f.write(struct.pack("<QI", cursor, len(payload)))
            cursor += len(payload)
        for _, payload in encoded:
            f.write(payload)


def suggest_canonical_id(entry_path: str) -> Optional[str]:
    """Map common FF8 Remastered archive paths to Lionheart canonical model IDs."""
    low = entry_path.lower()
    m_field = re.search(r"(?:^|/)(d\d{3})(?:[._/]|$)", low)
    if m_field:
        return f"field/{m_field.group(1)}"
    m_battle = re.search(r"(?:^|/)(c0m\d{3})(?:[._/]|$)", low)
    if m_battle:
        return f"battle/{m_battle.group(1)}"
    m_world = re.search(r"(?:^|/)(c0w\d{3})(?:[._/]|$)", low)
    if m_world:
        return f"world/{m_world.group(1)}"
    return None


def extract_zzz(
    zzz_path: Path, out_dir: Path, pattern: Optional[str] = None
) -> List[str]:
    entries = parse_zzz(zzz_path)
    rx = re.compile(pattern, re.IGNORECASE) if pattern else None
    selected = [e for e in entries if rx is None or rx.search(e.path)]

    out_root = out_dir.resolve()
    out_root.mkdir(parents=True, exist_ok=True)
    extracted: List[str] = []

    with zzz_path.open("rb") as src:
        for e in selected:
            target = (out_root / e.path).resolve()
            if out_root not in target.parents:
                raise ValueError(f"Refusing extraction outside output directory: {e.path}")
            target.parent.mkdir(parents=True, exist_ok=True)
            src.seek(e.offset)
            remaining = e.size
            with target.open("wb") as dst:
                while remaining > 0:
                    chunk = src.read(min(remaining, 65536))
                    if not chunk:
                        raise ValueError(f"Unexpected EOF extracting {e.path}")
                    dst.write(chunk)
                    remaining -= len(chunk)
            extracted.append(e.path)
    return extracted


def _png_chunk(tag: bytes, data: bytes) -> bytes:
    crc = zlib.crc32(tag + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)


def encode_rgba_png(width: int, height: int, rgba: bytes) -> bytes:
    if len(rgba) != width * height * 4:
        raise ValueError("RGBA buffer length mismatch")
    rows = bytearray()
    stride = width * 4
    for y in range(height):
        rows.append(0)  # filter type 0
        rows.extend(rgba[y * stride : (y + 1) * stride])
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    idat = zlib.compress(bytes(rows), level=6)
    return (
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", ihdr)
        + _png_chunk(b"IDAT", idat)
        + _png_chunk(b"IEND", b"")
    )


def _make_procedural_texture(
    size: int,
    primary: Tuple[int, int, int, int],
    accent: Tuple[int, int, int, int],
    alpha_cutout: bool = False,
) -> bytes:
    buf = bytearray(size * size * 4)
    for y in range(size):
        for x in range(size):
            idx = (y * size + x) * 4
            checker = ((x // 4) ^ (y // 4)) & 1
            col = primary if checker == 0 else accent
            if alpha_cutout:
                # Feather/strand cutout along bottom edge for MASK material testing.
                edge = (y > size * 3 // 4) and ((x % 4) < 2)
                a = 0 if edge else 255
                buf[idx : idx + 4] = bytes((col[0], col[1], col[2], a))
            else:
                buf[idx : idx + 4] = bytes(col)
    return encode_rgba_png(size, size, bytes(buf))


def _append_box(
    positions: List[Tuple[float, float, float]],
    normals: List[Tuple[float, float, float]],
    uvs: List[Tuple[float, float]],
    joints: List[Tuple[int, int, int, int]],
    weights: List[Tuple[float, float, float, float]],
    indices: List[int],
    center: Tuple[float, float, float],
    half_extents: Tuple[float, float, float],
    joint_a: int,
    joint_b: int,
    weight_a: float,
) -> None:
    cx, cy, cz = center
    hx, hy, hz = half_extents
    faces = [
        # (normal, 4 corners CCW)
        ((0.0, 0.0, 1.0), [(-hx, -hy, hz), (hx, -hy, hz), (hx, hy, hz), (-hx, hy, hz)]),
        ((0.0, 0.0, -1.0), [(hx, -hy, -hz), (-hx, -hy, -hz), (-hx, hy, -hz), (hx, hy, -hz)]),
        ((1.0, 0.0, 0.0), [(hx, -hy, hz), (hx, -hy, -hz), (hx, hy, -hz), (hx, hy, hz)]),
        ((-1.0, 0.0, 0.0), [(-hx, -hy, -hz), (-hx, -hy, hz), (-hx, hy, hz), (-hx, hy, -hz)]),
        ((0.0, 1.0, 0.0), [(-hx, hy, hz), (hx, hy, hz), (hx, hy, -hz), (-hx, hy, -hz)]),
        ((0.0, -1.0, 0.0), [(-hx, -hy, -hz), (hx, -hy, -hz), (hx, -hy, hz), (-hx, -hy, hz)]),
    ]
    quad_uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)]
    wb = max(0.0, 1.0 - weight_a)
    for nrm, corners in faces:
        base = len(positions)
        for (dx, dy, dz), uv in zip(corners, quad_uvs):
            positions.append((cx + dx, cy + dy, cz + dz))
            normals.append(nrm)
            uvs.append(uv)
            joints.append((joint_a, joint_b, 0, 0))
            weights.append((weight_a, wb, 0.0, 0.0))
        indices.extend([base + 0, base + 1, base + 2, base + 0, base + 2, base + 3])


def write_synthetic_character_glb(
    path: Path,
    primary_rgba: Tuple[int, int, int, int] = (52, 92, 168, 255),
    accent_rgba: Tuple[int, int, int, int] = (210, 220, 235, 255),
    crest_rgba: Tuple[int, int, int, int] = (230, 185, 65, 255),
) -> None:
    """Create a valid, textured, 2-bone skinned glTF 2.0 (.glb) test character.

    Contains two primitives:
      0: OPAQUE articulated torso + limbs (bones 0 & 1)
      1: MASK alpha-tested crest/hair fringe (bone 1)
    """
    # Primitive 0: OPAQUE body
    p0_pos: List[Tuple[float, float, float]] = []
    p0_nrm: List[Tuple[float, float, float]] = []
    p0_uv: List[Tuple[float, float]] = []
    p0_jnt: List[Tuple[int, int, int, int]] = []
    p0_wgt: List[Tuple[float, float, float, float]] = []
    p0_idx: List[int] = []
    # Legs / pelvis on bone 0
    _append_box(p0_pos, p0_nrm, p0_uv, p0_jnt, p0_wgt, p0_idx, (-8.0, 28.0, 0.0), (6.0, 20.0, 6.0), 0, 1, 1.0)
    _append_box(p0_pos, p0_nrm, p0_uv, p0_jnt, p0_wgt, p0_idx, (8.0, 28.0, 0.0), (6.0, 20.0, 6.0), 0, 1, 1.0)
    # Torso blended between bone 0 and bone 1
    _append_box(p0_pos, p0_nrm, p0_uv, p0_jnt, p0_wgt, p0_idx, (0.0, -4.0, 0.0), (14.0, 18.0, 8.0), 0, 1, 0.6)
    # Head on bone 1
    _append_box(p0_pos, p0_nrm, p0_uv, p0_jnt, p0_wgt, p0_idx, (0.0, -34.0, 0.0), (11.0, 11.0, 11.0), 1, 0, 1.0)

    # Primitive 1: MASK alpha-tested crest/hair fringe on bone 1
    p1_pos: List[Tuple[float, float, float]] = []
    p1_nrm: List[Tuple[float, float, float]] = []
    p1_uv: List[Tuple[float, float]] = []
    p1_jnt: List[Tuple[int, int, int, int]] = []
    p1_wgt: List[Tuple[float, float, float, float]] = []
    p1_idx: List[int] = []
    _append_box(p1_pos, p1_nrm, p1_uv, p1_jnt, p1_wgt, p1_idx, (0.0, -48.0, 0.0), (12.0, 4.0, 12.0), 1, 0, 1.0)

    tex0_png = _make_procedural_texture(16, primary_rgba, accent_rgba, alpha_cutout=False)
    tex1_png = _make_procedural_texture(16, crest_rgba, accent_rgba, alpha_cutout=True)

    bin_buf = bytearray()
    buffer_views: List[dict] = []
    accessors: List[dict] = []

    def add_view(raw: bytes) -> int:
        while len(bin_buf) % 4 != 0:
            bin_buf.append(0)
        off = len(bin_buf)
        bin_buf.extend(raw)
        idx = len(buffer_views)
        buffer_views.append({"buffer": 0, "byteOffset": off, "byteLength": len(raw)})
        return idx

    def pack_prim(
        pos: Sequence[Tuple[float, float, float]],
        nrm: Sequence[Tuple[float, float, float]],
        uv: Sequence[Tuple[float, float]],
        jnt: Sequence[Tuple[int, int, int, int]],
        wgt: Sequence[Tuple[float, float, float, float]],
        idx: Sequence[int],
        mat_idx: int,
    ) -> dict:
        bv_pos = add_view(b"".join(struct.pack("<3f", *v) for v in pos))
        bv_nrm = add_view(b"".join(struct.pack("<3f", *v) for v in nrm))
        bv_uv = add_view(b"".join(struct.pack("<2f", *v) for v in uv))
        bv_jnt = add_view(b"".join(struct.pack("<4B", *v) for v in jnt))
        bv_wgt = add_view(b"".join(struct.pack("<4f", *v) for v in wgt))
        bv_idx = add_view(b"".join(struct.pack("<H", i) for i in idx))

        a_pos = len(accessors)
        accessors.append({"bufferView": bv_pos, "componentType": 5126, "count": len(pos), "type": "VEC3"})
        a_nrm = len(accessors)
        accessors.append({"bufferView": bv_nrm, "componentType": 5126, "count": len(nrm), "type": "VEC3"})
        a_uv = len(accessors)
        accessors.append({"bufferView": bv_uv, "componentType": 5126, "count": len(uv), "type": "VEC2"})
        a_jnt = len(accessors)
        accessors.append({"bufferView": bv_jnt, "componentType": 5121, "count": len(jnt), "type": "VEC4"})
        a_wgt = len(accessors)
        accessors.append({"bufferView": bv_wgt, "componentType": 5126, "count": len(wgt), "type": "VEC4"})
        a_idx = len(accessors)
        accessors.append({"bufferView": bv_idx, "componentType": 5123, "count": len(idx), "type": "SCALAR"})

        return {
            "attributes": {
                "POSITION": a_pos,
                "NORMAL": a_nrm,
                "TEXCOORD_0": a_uv,
                "JOINTS_0": a_jnt,
                "WEIGHTS_0": a_wgt,
            },
            "indices": a_idx,
            "material": mat_idx,
        }

    prim0 = pack_prim(p0_pos, p0_nrm, p0_uv, p0_jnt, p0_wgt, p0_idx, 0)
    prim1 = pack_prim(p1_pos, p1_nrm, p1_uv, p1_jnt, p1_wgt, p1_idx, 1)
    bv_img0 = add_view(tex0_png)
    bv_img1 = add_view(tex1_png)
    while len(bin_buf) % 4 != 0:
        bin_buf.append(0)

    doc = {
        "asset": {"version": "2.0", "generator": "lionheart hd_models_import.py"},
        "buffers": [{"byteLength": len(bin_buf)}],
        "bufferViews": buffer_views,
        "accessors": accessors,
        "images": [
            {"bufferView": bv_img0, "mimeType": "image/png"},
            {"bufferView": bv_img1, "mimeType": "image/png"},
        ],
        "textures": [{"source": 0}, {"source": 1}],
        "materials": [
            {
                "name": "body_opaque",
                "alphaMode": "OPAQUE",
                "doubleSided": False,
                "pbrMetallicRoughness": {
                    "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                    "baseColorTexture": {"index": 0},
                },
            },
            {
                "name": "crest_mask",
                "alphaMode": "MASK",
                "alphaCutoff": 0.5,
                "doubleSided": True,
                "pbrMetallicRoughness": {
                    "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                    "baseColorTexture": {"index": 1},
                },
            },
        ],
        "meshes": [{"name": "synthetic_character", "primitives": [prim0, prim1]}],
    }

    json_bytes = json.dumps(doc, separators=(",", ":")).encode("utf-8")
    while len(json_bytes) % 4 != 0:
        json_bytes += b" "

    total_len = 12 + 8 + len(json_bytes) + 8 + len(bin_buf)
    glb = bytearray()
    glb.extend(b"glTF")
    glb.extend(struct.pack("<II", 2, total_len))
    glb.extend(struct.pack("<II", len(json_bytes), 0x4E4F534A))
    glb.extend(json_bytes)
    glb.extend(struct.pack("<II", len(bin_buf), 0x004E4942))
    glb.extend(bin_buf)

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(glb))


def make_test_pack(out_dir: Path) -> Dict[str, object]:
    """Generate a complete synthetic Lionheart HD model test pack."""
    out_dir.mkdir(parents=True, exist_ok=True)
    write_synthetic_character_glb(
        out_dir / "field/d000.glb",
        primary_rgba=(44, 88, 165, 255),
        accent_rgba=(215, 224, 238, 255),
        crest_rgba=(235, 195, 72, 255),
    )
    write_synthetic_character_glb(
        out_dir / "battle/c0m001.glb",
        primary_rgba=(168, 48, 62, 255),
        accent_rgba=(238, 220, 210, 255),
        crest_rgba=(240, 200, 90, 255),
    )
    write_synthetic_character_glb(
        out_dir / "world/squall.glb",
        primary_rgba=(48, 145, 96, 255),
        accent_rgba=(220, 238, 226, 255),
        crest_rgba=(235, 205, 80, 255),
    )

    manifest: Dict[str, object] = {
        "pack_name": "Lionheart Synthetic Test Pack",
        "version": 1,
        "models": {
            "field/d000": {
                "file": "field/d000.glb",
                "scale": 1.0,
                "offset": [0.0, 0.0, 0.0],
                "rotation_euler": [0.0, 0.0, 0.0],
                "flip_winding": False,
                "double_sided": False,
                "bone_remap": [0, 1],
                "vram_texture_hashes": [],
            },
            "battle/c0m001": {
                "file": "battle/c0m001.glb",
                "scale": 1.0,
                "offset": [0.0, 0.0, 0.0],
                "rotation_euler": [0.0, 0.0, 0.0],
                "flip_winding": False,
                "double_sided": False,
                "bone_remap": [0, 1],
                "vram_texture_hashes": [],
            },
            "world/squall": {
                "file": "world/squall.glb",
                "scale": 0.75,
                "offset": [0.0, 0.0, 0.0],
                "rotation_euler": [0.0, 0.0, 0.0],
                "flip_winding": False,
                "double_sided": False,
                "bone_remap": [0, 1],
                "vram_texture_hashes": [],
            },
        },
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def validate_glb(path: Path) -> Tuple[bool, str]:
    data = path.read_bytes()
    if len(data) < 20 or data[:4] != b"glTF":
        return False, "invalid glTF magic"
    version, declared_len = struct.unpack("<II", data[4:12])
    if version != 2 or declared_len > len(data):
        return False, f"unsupported GLB version/length ({version}, {declared_len})"

    json_bytes = None
    bin_bytes = None
    off = 12
    while off + 8 <= len(data):
        clen, ctype = struct.unpack("<II", data[off : off + 8])
        off += 8
        if off + clen > len(data):
            return False, "truncated GLB chunk"
        if ctype == 0x4E4F534A and json_bytes is None:
            json_bytes = data[off : off + clen]
        elif ctype == 0x004E4942 and bin_bytes is None:
            bin_bytes = data[off : off + clen]
        off += clen

    if json_bytes is None or bin_bytes is None:
        return False, "missing JSON or BIN chunk"
    try:
        doc = json.loads(json_bytes.decode("utf-8"))
    except Exception as exc:
        return False, f"invalid JSON: {exc}"
    meshes = doc.get("meshes", [])
    if not isinstance(meshes, list) or not meshes:
        return False, "no meshes in glTF"
    prim_count = sum(len(m.get("primitives", [])) for m in meshes if isinstance(m, dict))
    if prim_count == 0:
        return False, "no primitives in glTF"
    return True, f"ok ({prim_count} primitive(s), {len(doc.get('images', []))} image(s))"


def validate_pack(pack_dir: Path) -> Tuple[bool, List[str]]:
    messages: List[str] = []
    if not pack_dir.is_dir():
        return False, [f"Pack directory not found: {pack_dir}"]

    ok = True
    checked_files: Dict[Path, str] = {}
    manifest_path = pack_dir / "manifest.json"
    if manifest_path.is_file():
        try:
            doc = json.loads(manifest_path.read_text(encoding="utf-8"))
        except Exception as exc:
            return False, [f"Invalid manifest.json: {exc}"]
        models = doc.get("models", {})
        if not isinstance(models, dict):
            return False, ["manifest.json 'models' must be a JSON object"]
        for model_id, entry in models.items():
            rel = entry if isinstance(entry, str) else entry.get("file") if isinstance(entry, dict) else None
            if not rel:
                ok = False
                messages.append(f"[FAIL] {model_id}: missing 'file'")
                continue
            fpath = pack_dir / rel
            if not fpath.is_file():
                ok = False
                messages.append(f"[FAIL] {model_id}: file not found ({rel})")
                continue
            valid, detail = validate_glb(fpath)
            checked_files[fpath.resolve()] = model_id
            messages.append(f"[{'OK' if valid else 'FAIL'}] {model_id} -> {rel}: {detail}")
            if not valid:
                ok = False

    for glb in sorted(pack_dir.rglob("*.glb")):
        if glb.resolve() in checked_files:
            continue
        rel = glb.relative_to(pack_dir).as_posix()
        valid, detail = validate_glb(glb)
        messages.append(f"[{'OK' if valid else 'FAIL'}] (convention) {rel}: {detail}")
        if not valid:
            ok = False

    if not messages:
        return False, ["No .glb models found in pack directory"]
    return ok, messages


def run_self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="lionheart_hd_models_") as tmp:
        root = Path(tmp)
        # 1. Synthetic .zzz round-trip test
        zzz_path = root / "synthetic.zzz"
        payloads = {
            "data/field/d000.mch": b"FAKE_FIELD_MODEL_000",
            "data/battle/c0m001.dat": b"FAKE_BATTLE_MODEL_001_BYTES",
            "data/world/c0w000.bin": b"\x00\x01\x02\x03WORLD",
        }
        create_synthetic_zzz(zzz_path, payloads)
        entries = parse_zzz(zzz_path)
        assert len(entries) == 3, f"expected 3 entries, got {len(entries)}"
        assert suggest_canonical_id(entries[0].path) == "field/d000"
        assert suggest_canonical_id(entries[1].path) == "battle/c0m001"
        assert suggest_canonical_id(entries[2].path) == "world/c0w000"

        extracted_dir = root / "extracted"
        extracted = extract_zzz(zzz_path, extracted_dir, pattern=r"c0m001|d000")
        assert len(extracted) == 2
        assert (extracted_dir / "data/field/d000.mch").read_bytes() == payloads["data/field/d000.mch"]
        assert (extracted_dir / "data/battle/c0m001.dat").read_bytes() == payloads["data/battle/c0m001.dat"]

        # Reject path traversal in crafted .zzz
        bad_zzz = root / "traversal.zzz"
        bad_name = b"../escape.bin"
        hdr_sz = 4 + 4 + len(bad_name) + 12
        bad_zzz.write_bytes(
            struct.pack("<II", 1, len(bad_name)) + bad_name + struct.pack("<QI", hdr_sz, 4) + b"NOPE"
        )
        try:
            parse_zzz(bad_zzz)
            raise AssertionError("Expected path traversal .zzz to be rejected")
        except ValueError:
            pass

        # 2. Synthetic test pack generation & validation
        pack_dir = root / "pack"
        make_test_pack(pack_dir)
        ok, msgs = validate_pack(pack_dir)
        assert ok, f"pack validation failed: {msgs}"
        assert len(msgs) == 3
    print("hd_models_import self-test ok")
    return 0


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Lionheart HD 3D model pack helper & local FF8 Remastered (.zzz) tool"
    )
    parser.add_argument("--list-zzz", type=Path, help="List entries in a local FF8 Remastered .zzz archive")
    parser.add_argument("--extract-zzz", type=Path, help="Extract entries from a local .zzz archive")
    parser.add_argument("--out", type=Path, help="Output directory for --extract-zzz")
    parser.add_argument("--filter", type=str, default=None, help="Optional regex filter for --list-zzz / --extract-zzz")
    parser.add_argument("--make-test-pack", type=Path, help="Create a synthetic glTF 2.0 (.glb) test pack in DIR")
    parser.add_argument("--validate-pack", type=Path, help="Validate manifest.json and .glb files in DIR")
    parser.add_argument("--self-test", action="store_true", help="Run built-in unit self-test on synthetic data")
    args = parser.parse_args(argv)

    if args.self_test:
        return run_self_test()

    if args.make_test_pack:
        make_test_pack(args.make_test_pack)
        ok, msgs = validate_pack(args.make_test_pack)
        for m in msgs:
            print(m)
        print(f"Created synthetic HD model test pack in {args.make_test_pack}")
        return 0 if ok else 1

    if args.validate_pack:
        ok, msgs = validate_pack(args.validate_pack)
        for m in msgs:
            print(m)
        return 0 if ok else 1

    if args.list_zzz:
        entries = parse_zzz(args.list_zzz)
        rx = re.compile(args.filter, re.IGNORECASE) if args.filter else None
        shown = 0
        for e in entries:
            if rx and not rx.search(e.path):
                continue
            cid = suggest_canonical_id(e.path)
            tag = f"  -> {cid}" if cid else ""
            print(f"{e.size:10d}  {e.path}{tag}")
            shown += 1
        print(f"Total: {shown} / {len(entries)} entries")
        return 0

    if args.extract_zzz:
        if not args.out:
            parser.error("--extract-zzz requires --out DIR")
        extracted = extract_zzz(args.extract_zzz, args.out, args.filter)
        print(f"Extracted {len(extracted)} file(s) to {args.out}")
        return 0

    parser.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
