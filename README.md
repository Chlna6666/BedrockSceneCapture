# BedrockSceneCapture

Experimental Bedrock 1.26.51.1 client-native model catalog exporter for LeviLamina SDK 26.51.9.

## Hotkeys

- F7: Incrementally cache native triangle/vertex wireframes for loaded chunks, prioritize nearer chunks, and render GPU batches with frustum checks. The current GPU cache is limited to 512 closest loaded chunks; large chunks may truncate wireframe output.
- F8: Enumerate all registered block-state permutations and sample native world geometry from loaded chunks.
- F9: Cancel in-progress F8 (writes an explicitly partial index).

## Output

Inside LeviLamina mod data directory, under BedrockSceneCapture/export/1.26.51.1/:

- manifest.json: target version, counts, resource-pack fingerprint null until implemented, no claim of complete export.
- blocks.json: every registered state, NBT (hex), native IDs, atlas UV hints and sampled neighbor-context model references.
- meshes/<sha256>.json: deduplicated native position/normal/UV/ABGR and triangle indices.
- materials.json: observed native layer numbers and external atlas references.
- entities.json: explicit unsupported geometry/skeleton/animation state.
- coverage.json: complete/dynamic/partial/unsupported status per state with reasons.

textures/ is deliberately not exported. Resolve texture assets against locally installed resource packs and matching game version.

## Correctness

F8 does not serialize chunk cells or write any .bsc files. Registry coverage is distinct from native mesh sample coverage. Native meshes for unobserved states are absent, not reconstructed as cubes. Dynamic neighbor-dependent shapes have at most 8 observed 3x3x3 context signatures; these samples are not executable offline tessellation rules. Dedicated block-entity rendering, fluids, skeletal entity models and material blend recipes require subsequent implementation. They are not advertised as complete. Only air is marked complete. Material IDs and atlas UVs require additional asset resolution. Resource pack fingerprint is currently unavailable. Every capture overwrites the version-scoped index while old content-addressed mesh files may remain unreferenced.

Capture work is rate-limited on the game thread to approximately 3 ms per tick and 2 tessellations per tick; SHA-256 and JSON I/O run on a worker thread. A scan of live loaded chunks is not atomic.

## Build

Build with LLVM 22, xmake and LeviLamina 26.51.9 client SDK.

    xmake repo -u
    xmake f -a x64 -p windows -m release -y
    xmake -y -v

GitHub Actions verifies the packaged client manifest and LeviLamina allocator export. In-game hook correctness and completeness remain to be tested.
