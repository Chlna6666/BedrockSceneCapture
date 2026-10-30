#!/usr/bin/env python3
"""Validate model-library JSON, hash-addressed geometry and external material bindings."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import tempfile

INDEXES=("manifest.json","blocks.json","entities.json","materials.json","coverage.json","geometry_catalog.json")
STATUSES=("complete","dynamic","partial","unsupported")

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
        for slot,texture in bindings.items():
            variants=texture.get("atlas_variants",[])
            keys={(uv["variant"],uv["frame"]) for uv in variants}
            if len(keys)!=len(variants):errors.append("duplicate atlas variant/frame")
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
                expected_hashes.add(digest)
                mesh=json.loads((root/"meshes"/(digest+".json")).read_text(encoding="utf-8"))
                if mesh.get("topology")!="triangles":errors.append("mesh topology")
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
                    if pos<len(variant_slots):
                        indices_for_mesh=variant_slots[pos]
                        if len(indices_for_mesh)!=len(tri_slots):
                            errors.append("triangle variant count mismatch")
                        for slot,variant in zip(tri_slots,indices_for_mesh):
                            if slot is None and variant is not None:
                                errors.append("unresolved face must have null variant")
                            if slot is not None:
                                if not isinstance(variant,list) or len(variant)!=2:
                                    errors.append("missing variant for resolved face")
                                elif tuple(variant) not in {
                                    (uv["variant"],uv["frame"])
                                    for uv in bindings[slot].get("atlas_variants",[])
                                }:
                                    errors.append("referenced UV variant not exported")
    for model in static.get("models",[]):
        expected_hashes.add(model["mesh_ref"])
    for digest in expected_hashes:
        if len(digest)!=64:errors.append("invalid hash")
        content=(root/"meshes"/(digest+".json")).read_bytes()
        if hashlib.sha256(content).hexdigest()!=digest:errors.append("mesh content hash mismatch")
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
        (p/"meshes"/(digest+".json")).write_bytes(mesh+b" ")
        assert not validate(p)["valid"],"hash corruption not detected"
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
