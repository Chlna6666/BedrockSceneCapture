#!/usr/bin/env python3
"""Validate model-library JSON, hash-addressed geometry and external material bindings."""
import argparse
import hashlib
import json
import math
import re
from pathlib import Path
import tempfile

INDEXES=("manifest.json","blocks.json","entities.json","materials.json","coverage.json","geometry_catalog.json")
STATUSES=("complete","dynamic","partial","unsupported")
HASH_RE=re.compile(r"[0-9a-f]{64}")

def validate(root: Path):
    docs={name:json.loads((root/name).read_text(encoding="utf-8")) for name in INDEXES}
    manifest,blocks,entities,materials,coverage,static=(docs[name] for name in INDEXES)
    errors=[]
    states=blocks.get("blocks",[])
    if manifest.get("target_game_version")!="1.26.51.1":errors.append("game version")
    if manifest.get("registered_block_states")!=len(states):errors.append("registry count")
    if manifest.get("textures_exported") is not False or (root/"textures").exists():errors.append("texture payload")
    counts={name:0 for name in STATUSES}
    expected_hashes=set()
    refs=0
    unknown_triangles=0
    for b in states:
        status=b.get("status")
        if status not in counts:errors.append("invalid coverage status")
        else:counts[status]+=1
        bindings={x["slot"]:x for x in b.get("texture_bindings",[])}
        slots=set(bindings)
        if len(bindings)!=len(b.get("texture_bindings",[])):
            errors.append("duplicate texture slot")
        for slot,texture in bindings.items():
            variants=texture.get("atlas_variants",[])
            keys={(uv["variant"],uv["frame"]) for uv in variants}
            if len(keys)!=len(variants):errors.append("duplicate atlas variant/frame")
            for uv_group in (variants,texture.get("carried_atlas_variants",[])):
                for uv in uv_group:
                    rect=uv.get("atlas_uv")
                    if not isinstance(rect,list) or len(rect)!=4 or any(
                        not isinstance(n,(int,float)) or not math.isfinite(n) for n in rect
                    ):
                        errors.append("non-finite or invalid atlas UV rectangle")
            carried=texture.get("carried_atlas_variants",[])
            if len({(uv["variant"],uv["frame"]) for uv in carried})!=len(carried):
                errors.append("duplicate carried atlas variant/frame")
            if variants and not texture.get("resource_alias") and not texture.get("atlas_item"):
                errors.append("unresolvable texture identifier")
        for v in b.get("variants",[]):
            meshes=v.get("mesh_refs",[])
            face_slots=v.get("mesh_triangle_texture_slots",[])
            variant_slots=v.get("mesh_triangle_texture_variant_indices",[])
            if len(face_slots)!=len(meshes) or len(variant_slots)!=len(meshes):
                errors.append("missing face binding arrays")
            unknown_triangles+=v.get("unresolved_texture_triangles",0)
            for pos,digest in enumerate(meshes):
                refs+=1
                if not isinstance(digest,str) or HASH_RE.fullmatch(digest) is None:
                    errors.append("invalid mesh hash reference")
                    continue
                expected_hashes.add(digest)
                path=root/"meshes"/(digest+".json")
                try:
                    mesh=json.loads(path.read_text(encoding="utf-8"))
                except (OSError,ValueError) as exc:
                    errors.append(f"unreadable referenced mesh {digest}: {exc}")
                    continue
                if mesh.get("topology")!="triangles":errors.append("mesh topology")
                if mesh.get("uv_complete") not in (True,False,None):
                    errors.append("invalid UV validity marker")
                vertices=mesh.get("vertices",[])
                indices=mesh.get("indices",[])
                if len(indices)%3:errors.append("index triangle alignment")
                if any(i<0 or i>=len(vertices) for i in indices):errors.append("index bounds")
                if any(len(vv)!=9 or any(not math.isfinite(v) for v in vv[:8]) for vv in vertices):
                    errors.append("invalid vertices")
                if pos<len(face_slots):
                    tri_slots=face_slots[pos]
                    if len(tri_slots)!=len(indices)//3:errors.append("triangle binding count")
                    if any(x is not None and x not in slots for x in tri_slots):errors.append("invalid texture slot")
                    if mesh.get("uv_complete") is False and any(x is not None for x in tri_slots):
                        errors.append("UV-incomplete mesh cannot declare resolved texture slots")
                    if pos<len(variant_slots):
                        indices_for_mesh=variant_slots[pos]
                        if len(indices_for_mesh)!=len(tri_slots):
                            errors.append("triangle variant count mismatch")
                        for slot,variant in zip(tri_slots,indices_for_mesh):
                            if slot is None and variant is not None:
                                errors.append("unresolved face must have null variant")
                            if slot is not None and mesh.get("uv_space")=="block_face_local":
                                if variant is not None:
                                    errors.append("face-local texture must not use runtime atlas variant")
                                if mesh.get("geometry_source")!="registry_visual_bounds":
                                    errors.append("unrecognized face-local geometry provenance")
                                continue
                            if slot is not None:
                                if not isinstance(variant,list) or len(variant)!=2:
                                    errors.append("missing variant for resolved face")
                                elif tuple(variant) not in {
                                    (uv["variant"],uv["frame"])
                                    for uv in bindings[slot].get("atlas_variants",[])
                                }:
                                    errors.append("referenced UV variant not exported")
    for model in static.get("models",[]):
        digest=model.get("mesh_ref")
        if not isinstance(digest,str) or HASH_RE.fullmatch(digest) is None:
            errors.append("invalid static geometry hash reference")
            continue
        expected_hashes.add(digest)
    for digest in expected_hashes:
        path=root/"meshes"/(digest+".json")
        try:
            content=path.read_bytes()
        except OSError as exc:
            errors.append(f"missing referenced geometry {digest}: {exc}")
            continue
        if hashlib.sha256(content).hexdigest()!=digest:
            errors.append("mesh content hash mismatch")
    if counts!=coverage.get("counts"):errors.append("coverage counts")
    if refs!=manifest.get("native_mesh_refs"):errors.append("native reference count")
    if len(static.get("models",[]))!=manifest.get("static_geometry_models"):
        errors.append("static model count")
    if unknown_triangles!=manifest.get("unresolved_texture_triangles"):
        errors.append("unresolved triangle count")
    if entities.get("status")!="unsupported" and not entities.get("geometry"):
        errors.append("entities marked supported without geometry")
    return {"valid":not errors,"errors":errors,
            "states":len(states),"geometry_hashes":len(expected_hashes),
            "unresolved_triangles":unknown_triangles}

def self_test():
    with tempfile.TemporaryDirectory() as td:
        p=Path(td);(p/"meshes").mkdir()
        mesh=b'{"topology":"triangles","vertices":[],"indices":[]}\n'
        digest=hashlib.sha256(mesh).hexdigest()
        (p/"meshes"/(digest+".json")).write_bytes(mesh)
        examples={
            "manifest.json":{"target_game_version":"1.26.51.1","registered_block_states":1,
                             "native_mesh_refs":1,"static_geometry_models":0,
                             "unresolved_texture_triangles":0,"textures_exported":False},
            "blocks.json":{"blocks":[{"status":"partial","texture_bindings":[],
                                      "variants":[{"mesh_refs":[digest],
                                                   "mesh_triangle_texture_slots":[[]],
                                                   "mesh_triangle_texture_variant_indices":[[]],
                                                   "unresolved_texture_triangles":0}]}]},
            "entities.json":{"status":"unsupported"},
            "materials.json":{"materials":[]},
            "coverage.json":{"counts":dict.fromkeys(STATUSES,0)},
            "geometry_catalog.json":{"models":[]},
        }
        examples["coverage.json"]["counts"]["partial"]=1
        for name,obj in examples.items():
            (p/name).write_text(json.dumps(obj),encoding="utf-8")
        assert validate(p)["valid"]
        # Face-local registry mesh uses the native slot index and leaves
        # runtime atlas variants unset. BMCBL resolves texture aliases.
        local_mesh={
            "topology":"triangles","uv_complete":True,
            "geometry_source":"registry_visual_bounds","uv_space":"block_face_local",
            "vertices":[
                [0,0,0,0,0,-1,0,0,4294967295],
                [0,1,0,0,0,-1,0,1,4294967295],
                [1,1,0,0,0,-1,1,1,4294967295],
            ],
            "indices":[0,1,2]
        }
        local_raw=(json.dumps(local_mesh,separators=(",",":"))+"\\n").encode()
        local_hash=hashlib.sha256(local_raw).hexdigest()
        (p/"meshes"/(local_hash+".json")).write_bytes(local_raw)
        state=examples["blocks.json"]["blocks"][0]
        state["texture_bindings"]=[{"slot":2,"resource_alias":"stone"}]
        variant=state["variants"][0]
        variant["mesh_refs"]=[local_hash]
        variant["mesh_triangle_texture_slots"]=[[2]]
        variant["mesh_triangle_texture_variant_indices"]=[[None]]
        examples["manifest.json"]["native_mesh_refs"]=1
        for name in ("blocks.json","manifest.json"):
            (p/name).write_text(json.dumps(examples[name]),encoding="utf-8")
        assert validate(p)["valid"],"face-local slot must be resolvable without atlas"
        variant["mesh_triangle_texture_variant_indices"]=[[[0,0]]]
        (p/"blocks.json").write_text(json.dumps(examples["blocks.json"]),encoding="utf-8")
        assert "face-local texture must not use runtime atlas variant" in validate(p)["errors"]
        # Restore the original small fixture for corruption tests.
        state["texture_bindings"]=[]
        variant["mesh_refs"]=[digest]
        variant["mesh_triangle_texture_slots"]=[[]]
        variant["mesh_triangle_texture_variant_indices"]=[[]]
        (p/"blocks.json").write_text(json.dumps(examples["blocks.json"]),encoding="utf-8")
        (p/"meshes"/(digest+".json")).write_bytes(mesh+b" ")
        assert not validate(p)["valid"],"hash corruption not detected"
        # A reference must never be interpreted as a filesystem path, even
        # when the JSON indexes originate from an untrusted export folder.
        (p/"meshes"/(digest+".json")).write_bytes(mesh)
        examples["blocks.json"]["blocks"][0]["variants"][0]["mesh_refs"]=["../../traversal"]
        (p/"blocks.json").write_text(json.dumps(examples["blocks.json"]),encoding="utf-8")
        assert "invalid mesh hash reference" in validate(p)["errors"]
    print("Model library schema/hash self-test passed")

if __name__=="__main__":
    parser=argparse.ArgumentParser()
    parser.add_argument("directory",nargs="?",type=Path)
    parser.add_argument("--self-test",action="store_true")
    args=parser.parse_args()
    if args.self_test:self_test()
    if args.directory:
        result=validate(args.directory)
        print(json.dumps(result,ensure_ascii=False,indent=2))
        if not result["valid"]:raise SystemExit(1)
