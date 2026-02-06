<div align="center">

# Nyx

**Nanite-style Virtualized Geometry Renderer on DirectX 12 (MiniEngine-based)**

Nyx is a learning- and research-driven personal rendering project dedicated to studying extreme-scale geometry rendering techniques, such as continuous LOD, GPU-driven culling, mesh-shader dispatch, and demand-driven geometry streaming.

It serves as a hands-on experimental platform built on Microsoft MiniEngine and is currently validated with a mega scene containing:

- **1,639,668,228 unique triangles**
- **18,949,504,889 instanced triangles**

## Jump To

- [Showcase](#showcase)
- [Core Features](#core-features)
- [Performance Snapshot](#performance-snapshot)
- [Architecture](#architecture)
- [Build and Run](#build-and-run)

## Showcase

- Demo video: `TODO`
- Technical breakdown post: `TODO`
- PIX capture screenshots: `TODO`

| View | Preview |
|---|---|
| Hero shot | `TODO` |
| Meshlet LOD view | `TODO` |
| Culling debug view | `TODO` |
| Streaming stress view | `TODO` |

## Why Nyx

Real-time scenes are increasingly dominated by tiny triangles and massive geometry datasets. Nyx explores a practical DX12 implementation strategy to:

1. Keep visual detail where it matters via screen-space error traversal.
2. Move visibility and traversal work to GPU.
3. Stream only required geometry pages under memory pressure.
4. Keep the pipeline inspectable and tunable in runtime.

## Core Features

- Nanite-style meshlet hierarchy with DAG/BVH traversal
- GPU-driven rendering with indirect `DispatchMesh`
- Two-pass frustum + HZB occlusion culling
- Visibility buffer path + resolve to deferred GBuffer
- Dynamic geometry streaming with residency table updates
- Async IO worker + LZ4-compressed geometry pages
- Runtime ImGui controls:
  - GLTF hot switching
  - pixel error threshold
  - freeze culling
  - debug visualization modes

## Performance Snapshot

### Triangle Scale

| Scene | Unique Triangles | Instanced Triangles |
|---|---:|---:|
| `zorah_main_public.gltf` | 1,639,668,228 | 18,949,504,889 |

Data source: `MiniEngine/SceneViewer/Assets/zorah_main_public.gltf/CountGltfTriangles.ps1`

### Local Asset Size (Stress Setup)

| File | Size |
|---|---:|
| `zorah_main_public.gltf.bin` | ~38.8 GB |
| `zorah_main_public.gltf.nvsngeo` | ~49.1 GB |
| `zorah_main_public.mini` | ~55.5 GB |

### Reproducible Benchmark Table Template

| Scene | Resolution | GPU | CPU | Avg FPS | 1% Low | Notes |
|---|---|---|---|---:|---:|---|
| Zorah | `TODO` | `TODO` | `TODO` | `TODO` | `TODO` | `TODO` |
| San Miguel | `TODO` | `TODO` | `TODO` | `TODO` | `TODO` | `TODO` |
| Jinx | `TODO` | `TODO` | `TODO` | `TODO` | `TODO` | `TODO` |

## Architecture

### Offline / First-load Build

When loading `.gltf` / `.glb`, Nyx can build a cached `.mini`:

1. Parse scene/material/camera/animation data.
2. Build meshlets and group them.
3. Simplify groups iteratively to generate multi-level LOD.
4. Build hierarchy nodes and metadata.
5. Pack geometry into pages.
6. Compress pages with LZ4.
7. Save metadata + blob into `.mini`.

### Runtime Frame Flow

1. Instance cull pass
2. DAG/hierarchy cull pass (frustum + HZB)
3. Build indirect mesh dispatch args
4. Mesh shader pass 0
5. Export depth + generate HZB
6. Repeat cull + mesh shader pass 1
7. Resolve visibility buffer to GBuffer
8. Deferred lighting + post

### Streaming Runtime

- Page size: **256 KB**
- Chunk size: **256 MB**
- GPU request mask readback + async page load
- LZ4 decompress + upload + address table sync
- Dynamic chunk growth for VRAM pool

### Diagrams

- Cluster culling flow: `Docs/ClusterCull.md`
- DAG sketch: `Docs/DAG.md`

## Technical Snapshot

- Language: **C++20**
- Graphics API: **DirectX 12**
- Shader Model: **6.6**
- Agility SDK package: `Microsoft.Direct3D.D3D12 1.616.1`
- Meshlet defaults:
  - Max vertices: **128**
  - Max triangles: **128**
- Max streaming requests/update: **16384**

## Build and Run

### Requirements

- Windows 10/11
- Visual Studio 2022
- DX12-capable GPU with Mesh Shader support
- NuGet package restore enabled

Recommended for mega scenes:

- High VRAM GPU
- 32 GB+ RAM
- NVMe SSD
- Large free disk space

### Steps

1. Open `MiniEngine/SceneViewer/SceneViewer.sln`
2. Select `Release | x64`
3. Build and run `SceneViewer`

## Command Line Examples

Load a model:

```bash
SceneViewer.exe -model Assets/jinx/scene.gltf
```

Force rebuild `.mini` cache:

```bash
SceneViewer.exe -model Assets/jinx/scene.gltf -rebuild 1
```

## Controls

- `W A S D` / `Q E`: move
- Mouse: look
- Mouse wheel: camera speed scaling
- `Shift`: fine movement toggle
- `Backspace`: engine tuning/debug UI

## Repository Layout

- `MiniEngine/Core`: engine core, graphics systems, input, tuning
- `MiniEngine/Model`: model pipeline, meshlets, culling, streaming, shaders
- `MiniEngine/SceneViewer`: runtime viewer application
- `MiniEngine/SceneViewer/Assets`: sample and stress scenes

## Acknowledgements

- [Microsoft MiniEngine](https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/MiniEngine)
- [Unreal Engine Nanite talks and publications](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf)
- [`meshoptimizer`](https://github.com/zeux/meshoptimizer)
- [`lz4`](https://github.com/lz4/lz4)
- [`cgltf`](https://github.com/jkuhlmann/cgltf)
- [`imgui`](https://github.com/ocornut/imgui)
- Inspiration references: [`nanite-webgpu`](https://github.com/Scthe/nanite-webgpu), [`vk_lod_clusters`](https://github.com/nvpro-samples/vk_lod_clusters)

## License and Asset Notice

- Nyx source code is licensed under the MIT License. See `LICENSE`.
- Third-party components keep their original licenses. See `THIRD_PARTY_NOTICES.md`.
- Scene assets may have separate licenses and usage restrictions.
- Verify asset terms before redistribution.

## Contact

- GitHub: https://github.com/moonlovelj
- Email: `love.jingjing.forever.1314@gmail.com`



