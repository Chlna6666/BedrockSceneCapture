#!/usr/bin/env python3
"""Validate BedrockSceneCapture Geometry Pool v2 (exclusive export format)."""
import argparse
import hashlib
import json
import math
import re
import tempfile
from collections import Counter
from pathlib import Path

HASH=re.compile(r"[0-9a-f]{64}\Z")
STATUSES=("complete","dynamic","partial","unsupported")
DOCS=("manifest.json","blocks.json","entities.json","materials.json",
      "coverage.json","geometry_catalog.json","geometry_pool.json")

def validate(root: Path):
    errors=[]
    try:
        docs={n:json.loads((root/n).read_bytes()) for n in DOCS}
    except (OSError, ValueError) as exc:
        return {"valid":False,"errors":[f"invalid export index: {exc}"]}
    manifest,blocks,entities,materials,coverage,static,pool=(docs[n] for n in DOCS)
    states=blocks.get("blocks",[])
    if not isinstance(states,list):return {"valid":False,"errors":["invalid block list"]}
    if manifest.get("target_game_version")!="1.26.51.1":errors.append("game version")
    if manifest.get("shared_geometry_pool_schema")!=2 or pool.get("schema")!=2:
        errors.append("v2 schema required")
    if manifest.get("legacy_meshes_retained") is not False or pool.get("legacy_meshes_retained") is not False:
        errors.append("legacy meshes must be disabled")
    if (root/"meshes").exists():errors.append("legacy meshes directory remains")
    if manifest.get("textures_exported") is not False or (root/"textures").exists():
        errors.append("texture payload must stay external")
    if manifest.get("registered_block_states")!=len(states):errors.append("registered block state count")
    if manifest.get("static_geometry_models")!=len(static.get("models",[])):
        errors.append("static model count")
    if manifest.get("captured_block_actor_models")!=len(entities.get("model_parts",[])):
        errors.append("actor model count")
    counts=Counter(s.get("status") for s in states)
    if any(s not in STATUSES for s in counts):errors.append("invalid state coverage")
    if any(counts.get(s,0)!=coverage.get("counts",{}).get(s,0) for s in STATUSES):
        errors.append("state coverage count mismatch")
    caches={kind:{} for kind in ("geometry","appearance","materials","models")}
    seen={kind:set() for kind in caches}
    def load(kind,digest):
        if not isinstance(digest,str) or not HASH.fullmatch(digest):
            errors.append(f"invalid {kind} hash reference")
            return None
        seen[kind].add(digest)
        if digest in caches[kind]:return caches[kind][digest]
        try:
            data=(root/"geometry_pool"/kind/(digest+".json")).read_bytes()
            if hashlib.sha256(data).hexdigest()!=digest:
                errors.append(f"hash mismatch in {kind} {digest}")
            obj=json.loads(data)
        except (OSError,ValueError) as exc:
            errors.append(f"invalid {kind} resource {digest}: {exc}")
            obj=None
        caches[kind][digest]=obj
        return obj

    network={s.get("network_id"):s for s in states if isinstance(s.get("network_id"),int)}
    total_refs=0
    unresolved=0
    for block in states:
        alias=block.get("geometry_alias_network_id")
        if alias is not None:
            original=network.get(alias)
            if (not original or original is block or
                original.get("name")!=block.get("name") or
                original.get("geometry_alias_network_id") is not None or
                not any(x.get("neighborhood")=="registry_visual_bounds"
                        for x in original.get("variants",[])) or
                block.get("variants") or block.get("shared_material_ref") is not None):
                errors.append("invalid visual-only state alias")
            if block.get("name") not in ("minecraft:noteblock","minecraft:note_block",
                                       "noteblock","note_block"):
                errors.append("unverified visual-only alias")
            bindings=[]
        else:
            mat=load("materials",block.get("shared_material_ref"))
            if mat is None or mat.get("schema")!=2:
                errors.append("missing v2 material table")
                bindings=[]
            else:
                bindings=mat.get("texture_bindings",[])
                if not isinstance(bindings,list):bindings=[]
        if "texture_bindings" in block:errors.append("duplicate inline material table")
        slots={t.get("slot"):t for t in bindings}
        if len(slots)!=len(bindings):errors.append("duplicate material slot")
        for tex in bindings:
            for name in ("atlas_variants","carried_atlas_variants"):
                variants=tex.get(name,[])
                if len({(v.get("variant"),v.get("frame")) for v in variants})!=len(variants):
                    errors.append("duplicate atlas variant")
                for v in variants:
                    uv=v.get("atlas_uv")
                    if not isinstance(uv,list) or len(uv)!=4 or any(
                        not isinstance(n,(int,float)) or not math.isfinite(n) for n in uv):
                        errors.append("invalid texture atlas rectangle")
        for variant in block.get("variants",[]):
            if variant.get("capture_source")=="native_block_tessellator":
                neighbors=variant.get("neighbor_network_ids",[])
                if len(neighbors)!=27 or neighbors[0]!=block.get("network_id"):
                    errors.append("invalid neighbor signature")
            geoms=variant.get("shared_geometry_refs",[])
            apps=variant.get("shared_appearance_refs",[])
            if len(geoms)!=len(apps):errors.append("shared reference length mismatch")
            if "mesh_refs" in variant or "mesh_triangle_texture_slots" in variant:
                errors.append("legacy mesh fields present")
            unresolved+=variant.get("unresolved_texture_triangles",0)
            total_refs+=len(geoms)
            for digest,app_digest in zip(geoms,apps):
                geometry=load("geometry",digest)
                appearance=load("appearance",app_digest)
                if geometry is None or appearance is None:continue
                if (geometry.get("schema")!=2 or geometry.get("topology")!="triangles" or
                    geometry.get("precision")!="exact_float32" or
                    geometry.get("winding_preserved") is not True):
                    errors.append("invalid shared geometry metadata")
                if appearance.get("schema")!=2 or appearance.get("geometry_ref")!=digest:
                    errors.append("appearance points to wrong geometry")
                verts=geometry.get("vertices",[])
                tris=geometry.get("triangles",[])
                if not isinstance(verts,list) or not isinstance(tris,list):
                    errors.append("invalid geometry arrays")
                    continue
                if any(not isinstance(v,list) or len(v)!=3 or
                       any(not isinstance(n,(int,float)) or not math.isfinite(n) for n in v)
                       for v in verts):
                    errors.append("invalid shared vertices")
                if any(not isinstance(t,list) or len(t)!=3 or
                       any(not isinstance(n,int) or n<0 or n>=len(verts) for n in t)
                       for t in tris):
                    errors.append("invalid shared triangles")
                if appearance.get("geometry_source")=="native_world_tessellation" and any(
                    any(not (-2.0 <= float(p) <= 3.0) for p in v) for v in verts):
                    errors.append("native vertices outside block-local space")
                corners=appearance.get("corner_attributes",[])
                face_slots=appearance.get("triangle_texture_slots",[])
                face_variants=appearance.get("triangle_texture_variant_indices",[])
                if len(corners)!=3*len(tris) or len(face_slots)!=len(tris) or len(face_variants)!=len(tris):
                    errors.append("appearance triangle/corner count mismatch")
                    continue
                if any(not isinstance(row,list) or len(row)!=6 or
                       any(not isinstance(n,(int,float)) or not math.isfinite(n) for n in row)
                       for row in corners):
                    errors.append("invalid shared corner attributes")
                for slot,uv_variant in zip(face_slots,face_variants):
                    if slot is None:
                        if uv_variant is not None:errors.append("unbound face has atlas variant")
                        continue
                    if slot not in slots:errors.append("unknown material slot")
                    if appearance.get("uv_complete") is False:
                        errors.append("incomplete UV declares a material slot")
                    if appearance.get("uv_space")=="block_face_local":
                        if uv_variant is not None:errors.append("local UV has atlas variant")
                    elif (uv_variant is not None and slot in slots and
                          uv_variant not in [[v.get("variant"),v.get("frame")]
                                             for v in slots[slot].get("atlas_variants",[])]):
                        errors.append("unresolvable UV variant")

    for record in static.get("models",[]):
        load("models",record.get("model_ref"))
    for record in entities.get("model_parts",[]):
        model=load("models",record.get("model_ref"))
        if model is None or model.get("source")!="native_block_actor_model_parts":
            errors.append("invalid BlockEntity native model reference")
            continue
        parts=model.get("parts",[])
        if not any(
            p.get("cubes") or p.get("native_source_boxes") or
            (p.get("poly_mesh",{}).get("positions") and
             (p["poly_mesh"].get("triangles") or p["poly_mesh"].get("quads")))
            for p in parts):
            errors.append("BlockEntity model missing geometry")
    for kind,field in (("geometry","shared_geometry_count"),
                       ("appearance","shared_appearance_count"),
                       ("materials","shared_material_count")):
        if manifest.get(field)!=len(seen[kind]) or set(pool.get(kind,[]))!=seen[kind]:
            errors.append(f"{kind} count/index mismatch")
    if total_refs!=manifest.get("shared_geometry_references"):
        errors.append("shared geometry reference count")
    if unresolved!=manifest.get("unresolved_texture_triangles"):
        errors.append("unresolved texture triangles count")
    return {"valid":not errors,"errors":errors,"states":len(states),
            "shared_geometry":len(seen["geometry"]),
            "shared_appearance":len(seen["appearance"]),
            "shared_materials":len(seen["materials"]),"references":total_refs}

def self_test():
    with tempfile.TemporaryDirectory() as temp:
        root=Path(temp)
        for kind in ("geometry","appearance","materials","models"):
            (root/"geometry_pool"/kind).mkdir(parents=True,exist_ok=True)
        def add(kind,obj):
            data=(json.dumps(obj,separators=(",",":"))+"\n").encode()
            digest=hashlib.sha256(data).hexdigest()
            (root/"geometry_pool"/kind/(digest+".json")).write_bytes(data)
            return digest
        geometry={"schema":2,"topology":"triangles","precision":"exact_float32",
                  "winding_preserved":True,"vertices":[[0,0,0],[0,1,0],[1,1,0]],
                  "triangles":[[0,1,2]]}
        g=add("geometry",geometry)
        appearance={"schema":2,"geometry_ref":g,"uv_space":"block_face_local",
                    "geometry_source":"registry_visual_bounds","uv_complete":True,
                    "corner_attributes":[[0,0,-1,0,0,4294967295],
                                         [0,0,-1,0,1,4294967295],
                                         [0,0,-1,1,1,4294967295]],
                    "triangle_texture_slots":[2],"triangle_texture_variant_indices":[None]}
        a=add("appearance",appearance)
        m=add("materials",{"schema":2,"texture_bindings":[{"slot":2,"resource_alias":"stone"}]})
        files={
            "manifest.json":{"target_game_version":"1.26.51.1","registered_block_states":1,
                             "static_geometry_models":0,"captured_block_actor_models":0,
                             "textures_exported":False,"shared_geometry_pool_schema":2,
                             "legacy_meshes_retained":False,"shared_geometry_count":1,
                             "shared_appearance_count":1,"shared_material_count":1,
                             "shared_geometry_references":1,"unresolved_texture_triangles":0},
            "blocks.json":{"blocks":[{"name":"minecraft:stone","network_id":1,"status":"partial",
                                      "shared_material_ref":m,
                                      "variants":[{"neighborhood":"registry_visual_bounds",
                                                   "shared_geometry_refs":[g],
                                                   "shared_appearance_refs":[a],
                                                   "unresolved_texture_triangles":0}]}]},
            "entities.json":{"model_parts":[]},
            "materials.json":{"materials":[]},
            "coverage.json":{"counts":{"complete":0,"dynamic":0,"partial":1,"unsupported":0}},
            "geometry_catalog.json":{"models":[]},
            "geometry_pool.json":{"schema":2,"legacy_meshes_retained":False,
                                  "geometry":[g],"appearance":[a],"materials":[m]}
        }
        def save():
            for name,obj in files.items():
                (root/name).write_text(json.dumps(obj),encoding="utf-8")
        save()
        assert validate(root)["valid"],validate(root)["errors"]
        state=files["blocks.json"]["blocks"][0]
        state["variants"][0]["shared_appearance_refs"]=[]
        save()
        assert "shared reference length mismatch" in validate(root)["errors"]
        state["variants"][0]["shared_appearance_refs"]=[a]
        state["shared_material_ref"]="../../outside"
        save()
        assert "invalid materials hash reference" in validate(root)["errors"]
        state["shared_material_ref"]=m
        (root/"geometry_pool"/"geometry"/(g+".json")).write_bytes(b"{}")
        save()
        assert any("hash mismatch" in e for e in validate(root)["errors"])
        (root/"geometry_pool"/"geometry"/(g+".json")).write_bytes(
            (json.dumps(geometry,separators=(",",":"))+"\n").encode())
        (root/"meshes").mkdir()
        assert "legacy meshes directory remains" in validate(root)["errors"]
        (root/"meshes").rmdir()
        save()
        assert validate(root)["valid"],validate(root)["errors"]
    print("Geometry Pool v2 exclusive-export schema/hash self-test passed")

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
