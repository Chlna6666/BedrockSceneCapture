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
    by_network_id={
        b.get("network_id"):b for b in states
        if isinstance(b.get("network_id"),int) and not isinstance(b.get("network_id"),bool)
    }
    for b in states:
        status=b.get("status")
        if status not in counts:errors.append("invalid coverage status")
        else:counts[status]+=1
        alias=b.get("geometry_alias_network_id")
        if alias is not None:
            source=by_network_id.get(alias)
            if (not isinstance(alias,int) or isinstance(alias,bool) or
                source is None or source is b or
                source.get("name")!=b.get("name") or
                source.get("geometry_alias_network_id") is not None or
                not any(v.get("neighborhood")=="registry_visual_bounds"
                        for v in source.get("variants",[]))):
                errors.append("invalid canonical geometry state reference")
            if b.get("variants"):
                errors.append("audio-only alias duplicates geometry samples")
            if b.get("name") not in (
                "minecraft:noteblock","minecraft:note_block","noteblock","note_block"
            ):
                errors.append("visual alias is not a verified audio-only block")
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
            if v.get("capture_source")=="native_block_tessellator":
                neighbors=v.get("neighbor_network_ids",[])
                if (not isinstance(neighbors,list) or len(neighbors)!=27 or
                    any(not isinstance(x,int) or x<0 for x in neighbors)):
                    errors.append("invalid native neighbor state signature")
                elif neighbors[0]!=b.get("network_id"):
                    errors.append("native scene center does not match block state")
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
                if mesh.get("geometry_source")=="native_world_tessellation" and any(
                    any(not (-2.0 <= float(coord) <= 3.0) for coord in vv[:3])
                    for vv in vertices if len(vv)>=3
                ):
                    errors.append("native mesh vertices outside local block coordinate range")
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
                                if mesh.get("geometry_source") not in ("registry_visual_bounds","registry_cross_planes"):
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
    actor_models=entities.get("model_parts",[])
    if not isinstance(actor_models,list):
        actor_models=[]
        errors.append("invalid block actor model list")
    seen_actor_models=set()
    for actor in actor_models:
        identity=(actor.get("name"),actor.get("mesh_ref"))
        if identity in seen_actor_models:
            errors.append("duplicate native BlockActor model")
        seen_actor_models.add(identity)
        digest=actor.get("mesh_ref")
        if not isinstance(digest,str) or HASH_RE.fullmatch(digest) is None:
            errors.append("invalid block actor model reference")
            continue
        expected_hashes.add(digest)
        try:
            actor_json=json.loads((root/"meshes"/(digest+".json")).read_text(encoding="utf-8"))
            if actor_json.get("source")!="native_block_actor_model_parts" or not actor_json.get("parts"):
                errors.append("invalid native BlockActor model structure")
            else:
                cubes=[c for p in actor_json["parts"] for c in p.get("cubes",[])]
                if not any(
                    len(c.get("size",[]))==3 and
                    all(isinstance(n,(int,float)) and math.isfinite(n) and n!=0 for n in c["size"])
                    for c in cubes
                ):
                    errors.append("native BlockActor model has no nonzero geometry")
        except (OSError,ValueError) as exc:
            errors.append(f"unreadable native block actor model: {exc}")
    if manifest.get("captured_block_actor_models") not in (None,len(actor_models)):
        errors.append("native block actor model count")
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
    if entities.get("status")!="unsupported" and not (
        entities.get("geometry") or actor_models
    ):
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
        local_raw=(json.dumps(local_mesh,separators=(",",":"))+"\n").encode()
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
        # No duplicated geometry is required for an audio-only pitch change:
        # preserve its BlockState but reference the original model by ID.
        state["name"]="minecraft:noteblock"
        variant["neighborhood"]="registry_visual_bounds"
        state["network_id"]=42
        second={"name":"minecraft:noteblock","network_id":43,
                "geometry_alias_network_id":42,"status":"partial",
                "texture_bindings":[],"variants":[]}
        examples["blocks.json"]["blocks"].append(second)
        examples["manifest.json"]["registered_block_states"]=2
        examples["coverage.json"]["counts"]["partial"]=2
        for name in ("blocks.json","manifest.json","coverage.json"):
            (p/name).write_text(json.dumps(examples[name]),encoding="utf-8")
        assert validate(p)["valid"],"audio-only state alias must resolve to canonical geometry"
        second["geometry_alias_network_id"]=9999
        (p/"blocks.json").write_text(json.dumps(examples["blocks.json"]),encoding="utf-8")
        assert "invalid canonical geometry state reference" in validate(p)["errors"]
        examples["blocks.json"]["blocks"].pop()
        variant.pop("neighborhood")
        examples["manifest.json"]["registered_block_states"]=1
        examples["coverage.json"]["counts"]["partial"]=1
        for name in ("blocks.json","manifest.json","coverage.json"):
            (p/name).write_text(json.dumps(examples[name]),encoding="utf-8")

        # Passive native scene uses exact center and 26-neighbor state IDs.
        variant["capture_source"]="native_block_tessellator"
        state["network_id"]=42
        variant["neighbor_network_ids"]=[42]+list(range(1,27))
        (p/"blocks.json").write_text(json.dumps(examples["blocks.json"]),encoding="utf-8")
        assert validate(p)["valid"],"native neighbor signature should be valid"
        variant["neighbor_network_ids"][0]=99
        (p/"blocks.json").write_text(json.dumps(examples["blocks.json"]),encoding="utf-8")
        assert "native scene center does not match block state" in validate(p)["errors"]
        # Keep a native triangle in bounded block-local coordinates.
        # The historical F8 exporter double-subtracted world BlockPos,
        # producing invalid coordinates like (-352, -64, -304).
        badmesh={"topology":"triangles","uv_complete":False,
            "geometry_source":"native_world_tessellation","uv_space":"runtime_terrain_atlas",
            "vertices":[[-352,0,0,0,0,1,0,0,4294967295]],
            "indices":[]}
        badraw=(json.dumps(badmesh,separators=(",",":"))+"\n").encode()
        badhash=hashlib.sha256(badraw).hexdigest()
        (p/"meshes"/(badhash+".json")).write_bytes(badraw)
        variant["mesh_refs"]=[badhash]
        variant["mesh_triangle_texture_slots"]=[[]]
        variant["mesh_triangle_texture_variant_indices"]=[[]]
        (p/"blocks.json").write_text(json.dumps(examples["blocks.json"]),encoding="utf-8")
        assert "native mesh vertices outside local block coordinate range" in validate(p)["errors"]
        variant["mesh_refs"]=[local_hash]
        variant["mesh_triangle_texture_slots"]=[[2]]
        variant["mesh_triangle_texture_variant_indices"]=[[None]]
        variant.pop("capture_source")
        variant.pop("neighbor_network_ids")
        state.pop("network_id")
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
