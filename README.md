# BedrockSceneCapture

C++ client-native **Minecraft Bedrock 26.51.x** diagnostic mod for [LeviLamina](https://github.com/LiteLDev/LeviLamina). Uses the original game BlockTessellator, BlockGraphics and Tessellator MeshData; it does not reconstruct cube models or derive UVs from texture names.

## Keys

| Key | Action |
|---|---|
| **F7** | Toggle a local **triangle-edge wireframe + vertex cross-marker** overlay, updating as the player changes block position. Uses a small 5x5x5 block region to bound frame cost. |
| **F8** | Snapshot and export every already loaded chunk of the current dimension. |
| **F9** | Cancel the active export. |

F7 is an **overlay**, not a GPU rasterizer-wide polygon-mode override. Vanilla terrain materials remain underneath; only the added triangle edges and vertex markers are rendered without texture mapping. This avoids unsafe modification of the game's render pipeline. The preview intentionally excludes blocks rendered only by dedicated block entity / fluid renderers.

### Export

Outputs to the LeviLamina mod data directory: `BedrockSceneCapture/captures/capture_<unix_millis>/`.

Each `chunk_X_Z.bsc` is a little-endian `BSC1` file:
- 4-byte magic, u32 version (1), i32 chunk X/Z, i32 minY/maxY (exclusive)
- u32 palette count, cell count, mesh count, missing mesh count
- palette entries: u32 length + UTF-8 ID; u32 length + **native little-endian binary serialization BlockState NBT**; network ID (u32), legacy data (u16), first atlas UV rectangle (4x f32)
- cells: u16 palette indices in Y/Z/X order
- meshes: u32 cell index, render layer, primitive mode, vertex count, index count; each vertex has 8 f32 (XYZ, normal XYZ, UV) + u32 native ABGR; followed by u32 index values
- `capture.json` includes chunk-count summary, not a guarantee that disk writes succeeded (look for filesystem failures in debug logs).

Mesh positions are relative to their block; UVs are from the native terrain atlas (the atlas pixels themselves are **not yet exported**). Missing geometry is tracked rather than replaced by fabricated cubes. The base format also does not include dedicated block entities, dynamic entity meshes, PBR textures or reliable per-face atlas asset names.

**Safety/performance:** Uses available loaded chunks only, never generates new chunks, caps game-thread scan at ~3 ms and 512 cells per tick, performs I/O on a separate thread (max 2 pending chunks), and discards native pointers when the world/player changes. An F8 snapshot processes loaded chunks progressively; blocks may change while a large scan is in progress, so it is **not atomic**.

## Build

GitHub Actions builds automatically after every push to `main`, PR and manual dispatch, targeting Windows x64 with LLVM 22, xmake, and LeviLamina 26.51.9 client SDK. The dependency remains within game version `26.51.x`. Download the `BedrockSceneCapture-26.51.x-windows-x64` artifact from Actions.

Local Windows build:

```powershell
xmake repo -u
xmake f -a x64 -p windows -m release -y
xmake -y -v
python tools/inspect.py --self-test
```

This project is experimental. CI compilation and in-game validation of mesh submission, render thread safety, and world transition need to pass before considering it stable. For reference, [LHolo](https://github.com/MarmieQi/LHolo) uses the same game-native tessellator and a similar render hook (its GPL license applies to copied code; this repository's independent code is released under its own LICENSE).
