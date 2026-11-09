#!/usr/bin/env python3
"""Validate the BSC Geometry Pool v3-only export and its numeric references."""
from __future__ import annotations
import argparse
import json
import math
import struct
import tempfile
from pathlib import Path

ROOT_FILES = ("manifest.json", "blocks.json", "entities.json",
              "materials.json", "coverage.json", "geometry_catalog.json")
KINDS = ("geometry", "appearance", "materials", "models")
MAX_ENTRIES = 1_000_000
MAX_ITEM = 64 * 1024 * 1024


class Pool:
    def __init__(self, data: bytes):
        if len(data) < 28 or data[:8] != b"BSCPOOL3":
            raise ValueError("invalid Geometry Pool v3 header")
        version, *counts = struct.unpack_from("<IIIII", data, 8)
        if version != 3 or any(c > MAX_ENTRIES for c in counts):
            raise ValueError("unsupported pool version/counts")
        total = sum(counts)
        end = 28 + 12 * total
        if end > len(data):
            raise ValueError("truncated Geometry Pool v3 directory")
        self.entries: dict[str, list[bytes]] = {}
        cursor = end
        n = 28
        for kind, count in zip(KINDS, counts):
            items = []
            for _ in range(count):
                offset, size = struct.unpack_from("<QI", data, n)
                n += 12
                if size > MAX_ITEM or offset != cursor or offset + size > len(data):
                    raise ValueError(f"invalid {kind} binary offset/length")
                items.append(data[offset:offset + size])
                cursor = offset + size
            self.entries[kind] = items
        if cursor != len(data):
            raise ValueError("trailing bytes in Geometry Pool v3")

    def get(self, kind: str, index: object) -> bytes:
        if isinstance(index, bool) or not isinstance(index, int) or index < 0:
            raise ValueError(f"invalid {kind} u32 reference: {index!r}")
        try:
            return self.entries[kind][index]
        except IndexError as exc:
            raise ValueError(f"out-of-range {kind} u32 reference: {index}") from exc


def decode_geometry(data: bytes) -> tuple[int, int]:
    if len(data) < 8:
        raise ValueError("truncated geometry")
    vertices, triangles = struct.unpack_from("<II", data)
    if vertices > 2_000_000 or triangles > 2_000_000 or not triangles:
        raise ValueError("invalid geometry counts")
    if len(data) != 8 + 12 * vertices + 12 * triangles:
        raise ValueError("invalid geometry payload size")
    for item in struct.iter_unpack("<fff", data[8:8 + 12 * vertices]):
        if not all(math.isfinite(v) and abs(v) <= 4096 for v in item):
            raise ValueError("nonfinite/out-of-range geometry position")
    for tri in struct.iter_unpack("<III", data[8 + 12 * vertices:]):
        if any(i >= vertices for i in tri):
            raise ValueError("geometry triangle index out of range")
    return vertices, triangles


def decode_appearance(data: bytes, expected_geometry: int, triangles: int) -> list[int | None]:
    if len(data) < 20:
        raise ValueError("truncated appearance")
    geo, layer, source, flags, count = struct.unpack_from("<IIIII", data)
    if geo != expected_geometry or source > 2 or flags & ~3 or count != triangles:
        raise ValueError("invalid appearance geometry/source/count")
    if len(data) != 20 + count * 84:
        raise ValueError("invalid appearance payload size")
    attrs_end = 20 + count * 72
    for start in range(20, attrs_end, 24):
        *components, _color = struct.unpack_from("<fffffI", data, start)
        if not all(math.isfinite(v) for v in components):
            raise ValueError("nonfinite appearance normal or UV")
    slots: list[int | None] = []
    for slot, variant, frame in struct.iter_unpack("<III", data[attrs_end:]):
        if (variant == 0xFFFFFFFF) != (frame == 0xFFFFFFFF):
            raise ValueError("incomplete appearance variant pair")
        slots.append(None if slot == 0xFFFFFFFF else slot)
    if not (flags & 1) and any(slot is not None for slot in slots):
        raise ValueError("incomplete UV with material-bound triangles")
    return slots


def validate(root: Path) -> dict:
    errors: list[str] = []
    try:
        documents = {p: json.loads((root / p).read_text(encoding="utf-8"))
                     for p in ROOT_FILES}
        pool = Pool((root / "geometry_pool.bin").read_bytes())
    except (OSError, ValueError, struct.error, json.JSONDecodeError) as exc:
        return {"valid": False, "errors": [str(exc)]}
    manifest = documents["manifest.json"]
    if manifest.get("shared_geometry_pool_schema") != 3:
        errors.append("Geometry Pool v3 required")
    if manifest.get("legacy_meshes_retained") is not False:
        errors.append("legacy mesh format still enabled")
    for path in ("geometry_pool.json", "geometry_pool", "meshes"):
        if (root / path).exists():
            errors.append(f"legacy output remains: {path}")
    for kind, field in (("geometry", "shared_geometry_count"),
                        ("appearance", "shared_appearance_count"),
                        ("materials", "shared_material_count")):
        if manifest.get(field) != len(pool.entries[kind]):
            errors.append(f"{kind} manifest count mismatch")
    geometries = {}
    for idx, payload in enumerate(pool.entries["geometry"]):
        try:
            geometries[idx] = decode_geometry(payload)[1]
        except (ValueError, struct.error) as exc:
            errors.append(f"geometry[{idx}]: {exc}")
    materials = {}
    for idx, payload in enumerate(pool.entries["materials"]):
        try:
            obj = json.loads(payload)
            if obj.get("schema") != 2 or not isinstance(obj.get("texture_bindings"), list):
                raise ValueError("invalid material table")
            slots = [t["slot"] for t in obj["texture_bindings"]]
            if len(set(slots)) != len(slots):
                raise ValueError("duplicate material slot")
            materials[idx] = set(slots)
        except (ValueError, KeyError, TypeError, json.JSONDecodeError) as exc:
            errors.append(f"materials[{idx}]: {exc}")
    for idx, payload in enumerate(pool.entries["models"]):
        try:
            model = json.loads(payload)
            if not isinstance(model, dict):
                raise ValueError("invalid model")
            if model.get("geometry_complete") and any(
                model.get(field, 0) for field in
                ("unresolved_compiled_cubes", "incomplete_source_uv_boxes", "incomplete_native_uv")
            ):
                raise ValueError("model claims completeness with missing UV")
        except (ValueError, TypeError, json.JSONDecodeError) as exc:
            errors.append(f"models[{idx}]: {exc}")
    blocks = documents["blocks.json"].get("blocks", [])
    if not isinstance(blocks, list):
        errors.append("invalid blocks list")
        blocks = []
    references = 0
    unresolved = 0
    for block in blocks:
        mid = block.get("shared_material_ref")
        material_slots = set()
        if mid is not None:
            try:
                pool.get("materials", mid)
                material_slots = materials.get(mid, set())
            except ValueError as exc:
                errors.append(str(exc))
        if "texture_bindings" in block:
            errors.append("v2 inline material table remains")
        for variant in block.get("variants", []):
            geos = variant.get("shared_geometry_refs", [])
            apps = variant.get("shared_appearance_refs", [])
            if len(geos) != len(apps):
                errors.append("shared reference length mismatch")
            if "mesh_refs" in variant:
                errors.append("v2 mesh refs remain")
            unresolved += variant.get("unresolved_texture_triangles", 0)
            for gid, aid in zip(geos, apps):
                references += 1
                try:
                    pool.get("geometry", gid)
                    payload = pool.get("appearance", aid)
                    if gid not in geometries:
                        continue
                    for slot in decode_appearance(payload, gid, geometries[gid]):
                        if slot is not None and slot not in material_slots:
                            errors.append("unbound appearance material slot")
                except (ValueError, struct.error) as exc:
                    errors.append(str(exc))
    for filename, key in (("entities.json", "model_parts"),
                          ("geometry_catalog.json", "models")):
        for record in documents[filename].get(key, []):
            try:
                pool.get("models", record.get("model_ref"))
            except ValueError as exc:
                errors.append(str(exc))
    if manifest.get("shared_geometry_references") != references:
        errors.append("shared geometry reference count mismatch")
    if manifest.get("unresolved_texture_triangles") != unresolved:
        errors.append("unresolved texture triangle count mismatch")
    return {"valid": not errors, "errors": errors, "states": len(blocks),
            "shared_geometry": len(pool.entries["geometry"]),
            "shared_appearance": len(pool.entries["appearance"]),
            "shared_materials": len(pool.entries["materials"]),
            "references": references}


def self_test() -> None:
    def u32(*v: int) -> bytes:
        return struct.pack("<" + "I" * len(v), *v)
    geom = u32(3, 1) + struct.pack("<9f", 0,0,0, 1,0,0, 0,1,0) + u32(0,1,2)
    corner = struct.pack("<fffffI", 0,0,1, 0,0, 0xFFFFFFFF)
    appearance = u32(0,0,0,1,1) + corner*3 + u32(2,0xFFFFFFFF,0xFFFFFFFF)
    material = b'{"schema":2,"texture_bindings":[{"slot":2,"resource_alias":"stone"}]}'
    blobs = [[geom], [appearance], [material], []]
    count = sum(map(len, blobs))
    cursor = 28 + 12*count
    header = b"BSCPOOL3" + u32(3,*map(len,blobs))
    directory = bytearray()
    payload = bytearray()
    for group in blobs:
        for entry in group:
            directory.extend(struct.pack("<QI", cursor, len(entry)))
            payload.extend(entry)
            cursor += len(entry)
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        (root / "geometry_pool.bin").write_bytes(header + directory + payload)
        documents = {
            "manifest.json": {"shared_geometry_pool_schema":3,
                "legacy_meshes_retained":False, "shared_geometry_count":1,
                "shared_appearance_count":1, "shared_material_count":1,
                "shared_geometry_references":1, "unresolved_texture_triangles":0},
            "blocks.json": {"blocks":[{"shared_material_ref":0,
                "variants":[{"shared_geometry_refs":[0],
                             "shared_appearance_refs":[0],
                             "unresolved_texture_triangles":0}]}]},
            "entities.json":{"model_parts":[]},
            "geometry_catalog.json":{"models":[]},
            "materials.json":{"materials":[]},
            "coverage.json":{"counts":{}},
        }
        for name, doc in documents.items():
            (root / name).write_text(json.dumps(doc), encoding="utf-8")
        assert validate(root)["valid"], validate(root)
        documents["blocks.json"]["blocks"][0]["variants"][0]["shared_geometry_refs"]=[5]
        (root/"blocks.json").write_text(json.dumps(documents["blocks.json"]))
        assert not validate(root)["valid"]
        documents["blocks.json"]["blocks"][0]["variants"][0]["shared_geometry_refs"]=[0]
        (root/"blocks.json").write_text(json.dumps(documents["blocks.json"]))
        invalid=bytearray((root/"geometry_pool.bin").read_bytes())
        invalid[28]=0xff
        (root/"geometry_pool.bin").write_bytes(invalid)
        assert not validate(root)["valid"]
    print("Geometry Pool v3 binary/index roundtrip and rejection self-test passed")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("directory", type=Path, nargs="?")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        self_test()
    if args.directory:
        result = validate(args.directory)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        if not result["valid"]:
            raise SystemExit(1)
