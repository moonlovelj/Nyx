# Third-Party Notices

This repository includes third-party code and content. Those components remain
under their own licenses.

## Included Components

| Component | License | Local notice |
| --- | --- | --- |
| [Microsoft MiniEngine](https://github.com/microsoft/DirectX-Graphics-Samples/tree/master/MiniEngine) | MIT | [License.txt](MiniEngine/License.txt) |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT | [LICENSE.txt](MiniEngine/ThirdParty/imgui/LICENSE.txt) |
| [stb](https://github.com/nothings/stb), modified copies in ImGui | MIT or public-domain dedication | [rectpack](MiniEngine/ThirdParty/imgui/imstb_rectpack.h), [textedit](MiniEngine/ThirdParty/imgui/imstb_textedit.h), [truetype](MiniEngine/ThirdParty/imgui/imstb_truetype.h) |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | MIT | [LICENSE.md](MiniEngine/ThirdParty/meshoptimizer/LICENSE.md) |
| [LZ4](https://github.com/lz4/lz4) | BSD 2-Clause | [LICENSE](MiniEngine/ThirdParty/lz4/LICENSE) |
| [cgltf](https://github.com/jkuhlmann/cgltf) | MIT | [LICENSE](MiniEngine/ThirdParty/cgltf/LICENSE) |
| [nlohmann/json](https://github.com/nlohmann/json) | MIT; embedded notices retained | [json.hpp](MiniEngine/Model/json.hpp) |
| [Slang](https://github.com/shader-slang/slang) | Apache-2.0 WITH LLVM-exception | [LICENSE](MiniEngine/ThirdParty/slang-2026.10/LICENSE) |
| [DXC](https://github.com/microsoft/DirectXShaderCompiler) | MIT, LLVM/NCSA, Microsoft terms | [MIT](MiniEngine/ThirdParty/dxc_2026_05_27/LICENSE-MIT.txt), [LLVM](MiniEngine/ThirdParty/dxc_2026_05_27/LICENSE-LLVM.txt), [Microsoft](MiniEngine/ThirdParty/dxc_2026_05_27/LICENSE-MS.txt) |

## SDK and NuGet Dependencies

Package versions are recorded in each project's `packages.config`. Local package
notices are available under `Packages/` after NuGet restore.

| Component | License / notice |
| --- | --- |
| [DirectXTex](https://github.com/microsoft/DirectXTex) | [MIT](https://github.com/microsoft/DirectXTex/blob/main/LICENSE) |
| [DirectXMesh](https://github.com/microsoft/DirectXMesh) | [MIT](https://github.com/microsoft/DirectXMesh/blob/main/LICENSE) |
| [DirectXMath](https://github.com/microsoft/DirectXMath), supplied by the Windows SDK | [MIT](https://github.com/microsoft/DirectXMath/blob/main/LICENSE) |
| [zlib](https://zlib.net/) | zlib license, retained in the package's `zlib.h` |
| [PIX Event Runtime](https://devblogs.microsoft.com/pix/winpixeventruntime/) | 1.0.210209001: Microsoft Software License Terms; 1.0.240308001: MIT. See each package's `license.txt`. |
| [D3D12 Agility SDK](https://www.nuget.org/packages/Microsoft.Direct3D.D3D12/1.616.1) | Microsoft terms for the runtime (`LICENSE.txt`); MIT for code covered by `LICENSE-CODE.txt` |

## Adapted Rendering Code

| Source | Upstream license / copyright | Local implementation |
| --- | --- | --- |
| [Intel XeGTAO](https://github.com/GameTechDev/XeGTAO) | [MIT](https://github.com/GameTechDev/XeGTAO/blob/master/LICENSE), Intel Corporation | [Integration](MiniEngine/Core/XeGTAO.cpp), [shaders](MiniEngine/Core/Shaders/XeGTAOCommon.hlsli) |
| [Intel TAA](https://github.com/GameTechDev/TAA) | MIT, 2019-2020 Intel Corporation; notice retained in the shader | [TemporalBlendCS.hlsl](MiniEngine/Core/Shaders/TemporalBlendCS.hlsl) |

## Tool Dependencies

These are used by separate utilities, rather than SceneViewer.

| Component | License | Tool |
| --- | --- | --- |
| [Assimp](https://github.com/assimp/assimp) | [BSD-3-Clause](https://github.com/assimp/assimp/blob/v5.4.3/LICENSE) | [ModelConverter](MiniEngine/ModelConverter/) |
| [FreeType](https://freetype.org/) | [FTL or GPLv2](https://freetype.org/license) | [SDFFontCreator](MiniEngine/Tools/SDFFontCreator/) |

## Assets

Scene assets and external datasets may have separate license and usage
restrictions. Verify asset terms before redistribution.

