# BedrockSceneCapture — Bedrock 1.26.51.1

LeviLamina 26.51.9 **客户端**原生模组，目标为 BMCBL 3D 编辑器导出离线几何，而不是复制整个世界或贴图。

## 功能

- **F8 — 注册表与场景快照**：主菜单 F8 直接导出注册表；在游戏世界内首次 F8 开始导出和被动观察原生几何，再按 F8 保存，F9 取消。不会主动遍历已加载区块，但会读取正在渲染方块的 3×3×3 邻居状态。
- **原生静态 BlockGeometry**：保留模型名称、父模型、元素、立方体、六个面、局部 UV、旋转、纹理映射。输出 `geometry_catalog.json`；BlockState 与该目录之间尚不存在经过验证的全量一一映射。
- **CPU 直接构建网格**：对原生 `BlockShape::Block`、`SlimeBlock` 的可视 AABB 生成六面带法线三角形；对 `CrossTexture`、`DoublesidedCrossTexture`、`DoublesidedWideCrossTexture` 生成双面交叉平面。均在主线程分帧构建、全程不使用 GPU、不读 ChunkSource。具有相同可视边界、渲染层与形状的网格在内存中共享，磁盘按 SHA-256 去重。
- **真实渲染场景**：Hook 原生 `BlockTessellator::tessellateInWorld`，只在 F8 录制期间截取楼梯、栅栏、墙、流体等实际输出的几何；状态签名保存中心及 26 邻居的本次游戏注册网络 ID，`native_scene` 仅保证已观察到的邻居排列。无观察样本时不声称精确覆盖。
- **专用 BlockActor**：Hook 箱子、告示牌、悬挂告示牌、床、潜影盒、钟的渲染入口；提取原生 `ModelPart`、`Cube`、骨骼层级与每面 UV，保存到 `entities.json` 和哈希命名模型文件。动画姿态和文本暂不导出。
- **F7 — 世界调试线框**：仍依赖游戏已加载区块。与 F8 无区块模型库是两条独立链路。
- **F9 — 取消 F8**：在清单里标记取消，不谎报完成。

## 与 BMCBL 贴图拼接

BMCBL 的 `crates/bedrock-block-model` 已解析 `blocks.json`、`terrain_texture.json`、资源包几何；`ObjTextureResolver` 负责纹理解析。因此不导出 `textures/`，不复制图集和 GPU 材质。

F8 输出目录：`BedrockSceneCapture/export/1.26.51.1/`。

- `blocks.json`：每个 BlockState 的 `native_block_shape`、`geometry_context`（例如 `stairs_corner_neighbors`、`fluid_neighbors`、`horizontal_connections`）、`visual_bounds`、`visual_bounds_mesh_generated`、`texture_bindings[].resource_alias`，以及 `variants[].mesh_refs`、逐三角形 `mesh_triangle_texture_slots`。
- `meshes/<sha256>.json`：去重网格。`uv_space=block_face_local` 表示 UV 在方块面本地空间，绝对不能当作动态地形图集坐标；`face_index_per_triangle` 与 `face_order` 明确六面拼接顺序（上、下、北、南、西、东）。交叉平面只使用第一个纹理槽，正反面分别提供几何。
- `geometry_catalog.json`：静态 BlockGeometry 定义；离线端还需要完成父模型、变换和条件可见性组合。
- `coverage.json`、`manifest.json`：覆盖与局限；`registry_only=true`、`loaded_chunks_scanned=0`。
- `materials.json`：保留向后兼容的轻量化渲染层提示；不再提取 runtime atlas UV/PBR 数据。
- `entities.json`：仅保存已观察的专用方块实体原生静态模型；未观察到的渲染器仍列为 unsupported。

**精度边界**：注册表 AABB 和交叉平面生成的网格是 `geometry_confidence=visual_shape_proxy`，不是游戏 Tessellator 的逐顶点精确输出。复杂硬编码几何（楼梯、栅栏、门、流体）、邻居连接、动画、专用 BlockEntityRenderer 仍需 BMCBL 的规则或后续独立原生导出；不能把简单代理模型当作完整模型。仅空气可被标记为 complete。

## 构建

Windows x64，LLVM clang-cl 22 + xmake：

```powershell
pwsh -NoProfile -File tools/build_client.ps1
```

或直接运行 `xmake f -p windows -a x64 -m release -y` 与 `xmake -y -v`。CI 执行 Windows x64 模组编译、打包校验，及 Linux 模型索引格式自测。构建通过不代表已完成实际游戏中的 F7/F8 功能验证。

## Remote desktop / F8 input fallback

The normal F7/F8/F9 hotkeys are polled only when the Minecraft window is the foreground process.
RDP, remote-control software, virtual keyboards and Fn-key interception can make them unreliable.

For a reliable input-independent option, find the full **BSC remote control directory**
in the LeviLamina log (`<mod data dir>/control/`). While the game is running,
create an **empty marker file** in that directory:

| File | Action |
|---|---|
| `status.flag` | Log game/menu hook activity, focus, key detection and capture state |
| `start.flag` | Start F8 registry export / in-world native observation |
| `save.flag` | Save an active world observation (waits for registry enumeration to finish) |
| `cancel.flag` | Cancel an active capture |
| `preview.flag` | Toggle F7 wireframe visualization in world |

Example PowerShell, executed on the **remote Windows machine** (replace the path with
the exact directory printed in the mod log):

```powershell
$control = 'C:\<BSC-data-directory>\control'
New-Item -ItemType File -Path (Join-Path $control 'status.flag') -Force
New-Item -ItemType File -Path (Join-Path $control 'start.flag') -Force
# Explore loaded stairs, fences, walls, fluids and BlockActor models.
New-Item -ItemType File -Path (Join-Path $control 'save.flag') -Force
```

Command files are removed when consumed and polled at most once per 150 ms on the
game update thread; they are not scripts, contain no commands and require no
keyboard foreground focus. The `status.flag` log distinguishes failed key
forwarding from a missing game tick/menu hook.

## 统一共享模型资源库（Geometry Pool v2）

F8 新增实际落盘的共享资源池，不再仅对独立 Mesh 计算几何哈希。

- `geometry_pool.json`：所有被状态引用的共享几何、外观与纹理表索引，`schema=2`。
- `geometry_pool/geometry/<sha256>.json`：精确 float32 坐标（仅归一化 -0）、三角形拓扑和绕序，无纹理、UV、颜色、渲染层。
- `geometry_pool/appearance/<sha256>.json`：共享几何引用与对应三角形顺序的法线、UV、顶点颜色、渲染层、纹理槽和变体索引。
- `geometry_pool/materials/<sha256>.json`：`BlockGraphics` 的纹理名称、图集绑定和变体表。不同 BlockState 可引用同一表。
- `blocks.json`：每个 `variants[]` 通过 `shared_geometry_refs` 和 `shared_appearance_refs` 引用资源池；状态级 `shared_material_ref` 指向纹理表。每个列表顺序与旧 `mesh_refs` 一致。

所有方块均按实际几何全局去重：石头、矿石、不同材质石墙、栅栏、楼梯、不同颜色方块等，只有在三角形位置和绕序严格一致时才共用几何。状态 NBT、邻居签名、BlockEntity 模型族、UV 和颜色仍独立保留。含水状态及水面渲染规则不可因固体几何一致而丢弃。

**严格边界**：当前只对三角形次序和不改变绕序的循环顶点置换做精确归一化；不自动合并旋转、镜像、缩放等不同姿态。`mesh_geometry_hashes` 是旧版量化几何提示；新增 `shared_geometry_refs` 才是精确、可寻址的 v2 几何资源 ID。原生网格和视觉 AABB 代理均有来源标签，不能把代理的共享当作已验证模型。

**兼容与体积**：迁移期保留完整 `meshes/` v1 文件和原有状态字段，因此本版本导出目录可能比之前更大。BMCBL 需要增加 v2 解析器并使用共享 GPU 几何缓冲，验证视觉一致后才能关闭 v1 输出并真正减少磁盘容量。`manifest.json` 含 `shared_geometry_pool_schema`、`shared_geometry_count`、`shared_appearance_count`、`shared_material_count` 和 `legacy_meshes_retained`。由于邻居规则、流体及专用 BlockEntity 尚未全量验证，导出继续标记 `complete=false`。

验证新导出：`python tools/verify_model_library.py <F8-export-directory>`。校验每个资源的 SHA-256、引用与三角形/外观流对齐，但仍不能代替客户端的原生模型精确性比对。
