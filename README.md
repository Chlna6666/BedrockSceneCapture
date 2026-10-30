# BedrockSceneCapture — Minecraft Bedrock 1.26.51.1

LeviLamina 26.51.9 client mod. F8 exports native block-state registry information and native block geometry definitions even when those particular block states are not placed in any world.

## Controls

- F7: Loaded-region incremental chunk wireframe inspection, nearest-first, capped at 512 cached chunks and bounded per-chunk line count. Native tessellation and GPU upload are incremental.
- F8: Enumerate registered BlockType permutations and static BlockGeometry::Model definitions; optionally sample live tessellation from loaded chunks for context-dependent geometry.
- F9: Stop sampling; output is marked cancelled/incomplete.

## Output

All output is written to the LeviLamina data directory under `BedrockSceneCapture/export/1.26.51.1/`.

- `manifest.json`: schema, game/SDK versions, state/model/mesh counts, failure and unresolved UV counts, missing resource-pack fingerprint.
- `blocks.json`: BlockState NBT hex, network ID, variant IDs; per-state texture slot alias, all known native UV variants and animation-frame indices, atlas item/source path, atlas rectangles, source dimensions, native render layer and PBR handles; sampled meshes carry per-triangle slot plus variant/frame reference.
- `geometry_catalog.json`: all available native static BlockGeometry::Model names to hash-addressed JSON model definition references; relationships to specific block states are **not verified**.
- `meshes/<sha256>.json`: deduplicated native tessellated triangle meshes (positions, normals, UV, colors, indices) **or** registered static block model element/face definitions with native textures, parent, transforms and UV. Distinguish by `topology` versus `kind`.
- `materials.json`: native layer references and offline atlas UV remapping contract; texture pixels are always loaded from a matching local game/resource pack installation.
- `entities.json`: explicit unsupported status until entity and block-entity renderer extraction is implemented.
- `coverage.json`: per-state complete / partial / dynamic / unsupported with reasons.

No `textures/` or `chunk_X_Z.bsc` is generated.

## Texture correctness and offline usage

Native terrain UVs belong to the game's dynamically assembled atlas and **must not** be applied directly to a different atlas. For sampled triangles, first find the block state's referenced texture slot in `mesh_triangle_texture_slots`, look up `texture_bindings`, resolve `resource_alias` through the local Bedrock `terrain_texture.json` and active resource-pack priority, then rebase that triangle's atlas UV to the corresponding local texture or atlas rectangle. The original rectangle is preserved to derive local UV coordinates. If a triangle has null slot or the local asset is missing, report unresolved rather than guessing. The schema includes original atlas item and source hints but does not prove exact material fidelity without validation against an in-game render.

Static model definitions already include face texture identifiers and texture maps; they do not require placement in the world to export. For static model definitions the offline renderer must reproduce the source's box/face transforms, UV rotation and texture-map resolution. The mapping of each BlockState to its static model definition has not yet been verified. Dynamic blocks (stairs, fences, water, lava and other neighbor-sensitive states) require offline neighbor rules; a finite set of mesh samples cannot guarantee every context. Block-entity models, actor animation, alpha blending and PBR material recipes still need dedicated capture.

Registered state enumeration is complete *for the initialized registry*; mesh and material completeness is not claimed. Only air is classified complete. Pack fingerprints are not yet available. Live model sampling is rate-limited to approximately 3 ms and 2 tessellator calls per game tick, with asynchronous hash and disk I/O. F8 currently requires being in a world to use its LocalPlayer hotkey handler, although it does not require all states or static model definitions to be loaded into world chunks.

## Build and validation

On Windows x64: `xmake repo -u`, `xmake f -a x64 -p windows -m release -y`, `xmake -y -v`.

`python tools/verify_model_library.py --self-test` validates format invariants. `python tools/verify_model_library.py <export-dir>` checks counts, reference hashes, triangle texture slot arrays and JSON consistency. CI also checks the client mod package and unified LeviLamina allocator exports. In-game validation remains required.

Texture mappings are extracted directly from `TextureAtlasItem.mTextureUVs` rather than indexing `getTexture()` with the packed legacy block state. All published atlas variants include their original atlas rectangles; ambiguous UV matches are marked unresolved, and transparent render classes remain hints rather than verified shader pipelines.
