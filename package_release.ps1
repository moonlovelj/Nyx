<#
Package Nyx SceneViewer with prebuilt Bistro assets and emit a ZIP and SHA256.

Examples:
  .\package_release.ps1
  .\package_release.ps1 -Version "v2.0.0" -NoTimestamp
  .\package_release.ps1 -Version "v2.0.0" -NoTimestamp -KeepStaging
  .\package_release.ps1 -SourceDir "MiniEngine\Build\x64\Release\Output\SceneViewer" -IncludeSymbols
#>

param(
  [string]$SourceDir = "MiniEngine\Build\x64\Release\Output\SceneViewer",
  [string]$DistDir = "dist",
  [string]$PackageName = "Nyx-SceneViewer-win64",
  [string]$Version = "",
  [string]$EnvironmentMap = "citrus_orchard_road_puresky_4k.dds",
  [string]$VCRedistRoot = "",
  [switch]$IncludeSymbols,
  [switch]$NoTimestamp,
  [switch]$KeepStaging
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-PathFromBase {
  param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$Base
  )

  if ([IO.Path]::IsPathRooted($Path)) {
    return [IO.Path]::GetFullPath($Path)
  }
  return [IO.Path]::GetFullPath((Join-Path $Base $Path))
}

function Copy-DirectoryContents {
  param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Destination
  )

  if (-not (Test-Path -LiteralPath $Source -PathType Container)) {
    throw "Required directory not found: $Source"
  }

  New-Item -ItemType Directory -Path $Destination -Force | Out-Null
  Get-ChildItem -LiteralPath $Source -Force |
    Copy-Item -Destination $Destination -Recurse -Force
}

function Resolve-ChildPath {
  param([string]$Root, [string]$RelativePath)
  $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/')
  $childFull = [IO.Path]::GetFullPath((Join-Path $rootFull $RelativePath))
  if (-not $childFull.StartsWith($rootFull + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Path must remain inside ${rootFull}: $RelativePath"
  }
  return $childFull
}

function Copy-PayloadFile {
  param([string]$Source, [string]$RelativePath)
  if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
    throw "Required file not found: $Source"
  }
  $destination = Resolve-ChildPath -Root $payloadRoot -RelativePath $RelativePath
  New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
  Copy-Item -LiteralPath $Source -Destination $destination -Force
}

function Assert-MiniCache {
  param([string]$Path, [string]$SourceModel)
  $stream = [IO.File]::OpenRead($Path)
  $reader = [IO.BinaryReader]::new($stream)
  try {
    $magic = [Text.Encoding]::ASCII.GetString($reader.ReadBytes(4))
    $version = $reader.ReadUInt32()
    if ($magic -ne "MINI" -or $version -ne $miniVersion) {
      throw "Rebuild the model cache with this release before packaging: $Path"
    }
  } finally {
    $reader.Dispose()
  }
  if ((Get-Item -LiteralPath $Path).LastWriteTimeUtc -lt (Get-Item -LiteralPath $SourceModel).LastWriteTimeUtc) {
    throw "Model cache is older than its source. Load the model before packaging: $Path"
  }
}

function Assert-DdsTexture {
  param([string]$Path)
  $stream = [IO.File]::OpenRead($Path)
  $reader = [IO.BinaryReader]::new($stream)
  try {
    if ([Text.Encoding]::ASCII.GetString($reader.ReadBytes(4)) -ne "DDS ") {
      throw "Expected a compiled DDS texture, not a source or LFS pointer: $Path"
    }
  } finally {
    $reader.Dispose()
  }
}

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourcePath = Resolve-PathFromBase -Path $SourceDir -Base $repoRoot
$distPath = Resolve-PathFromBase -Path $DistDir -Base $repoRoot

if ($PackageName -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') {
  throw "PackageName must be a simple file name without directory components."
}
$miniVersionMatch = [regex]::Match((Get-Content -LiteralPath (Join-Path $repoRoot "MiniEngine\Model\ModelLoader.h") -Raw), '#define\s+CURRENT_MINI_FILE_VERSION\s+(\d+)')
if (-not $miniVersionMatch.Success) { throw "Could not determine the model cache version." }
$miniVersion = [uint32]$miniVersionMatch.Groups[1].Value
$sourceCommit = (& git -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw "Could not determine the source commit." }
$sourceChanges = @(& git -C $repoRoot diff HEAD --name-only --ignore-submodules=untracked -- MiniEngine)
if ($LASTEXITCODE -ne 0 -or $sourceChanges.Count -ne 0) {
  throw "Commit renderer source changes before packaging a release."
}

if (-not (Test-Path -LiteralPath $sourcePath)) {
  throw "SourceDir not found: $sourcePath"
}

$exePath = Join-Path $sourcePath "SceneViewer.exe"
if (-not (Test-Path -LiteralPath $exePath)) {
  throw "SceneViewer.exe not found under SourceDir: $sourcePath"
}

$timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
$nameParts = @($PackageName)
if (-not [string]::IsNullOrWhiteSpace($Version)) {
  if ($Version -notmatch '^v\d+\.\d+\.\d+([\-+][0-9A-Za-z\.-]+)?$') {
    throw "Invalid Version format: '$Version'. Expected like v1.0.0 or v1.0.0-beta.1"
  }
  $nameParts += $Version
}
if (-not $NoTimestamp) {
  $nameParts += $timestamp
}
$packageStem = ($nameParts -join "-")
$zipPath = Resolve-ChildPath -Root $distPath -RelativePath "$packageStem.zip"
$shaPath = "$zipPath.sha256"
$stagingRoot = Resolve-ChildPath -Root $distPath -RelativePath "_staging_$packageStem"
$payloadRoot = Resolve-ChildPath -Root $stagingRoot -RelativePath $packageStem

New-Item -ItemType Directory -Path $distPath -Force | Out-Null

if (Test-Path -LiteralPath $stagingRoot) {
  Remove-Item -LiteralPath $stagingRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $payloadRoot -Force | Out-Null

Write-Host "Collecting release runtime from: $sourcePath"
$runtimeFiles = @("SceneViewer.exe", "slang.dll", "slang-compiler.dll", "slang-rt.dll", "dxcompiler.dll", "dxil.dll", "WinPixEventRuntime.dll")
foreach ($runtimeFile in $runtimeFiles) {
  Copy-PayloadFile -Source (Join-Path $sourcePath $runtimeFile) -RelativePath $runtimeFile
}
Copy-PayloadFile -Source (Join-Path $sourcePath "D3D12\D3D12Core.dll") -RelativePath "D3D12\D3D12Core.dll"
if (Test-Path -LiteralPath (Join-Path $sourcePath "SceneViewer.exe.manifest")) {
  Copy-PayloadFile -Source (Join-Path $sourcePath "SceneViewer.exe.manifest") -RelativePath "SceneViewer.exe.manifest"
}
Copy-DirectoryContents -Source (Join-Path $sourcePath "slang-standard-module-2026.10") -Destination (Join-Path $payloadRoot "slang-standard-module-2026.10")

# Use the compiler's redistributable directories, never arbitrary system DLLs.
if ([string]::IsNullOrWhiteSpace($VCRedistRoot)) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path -LiteralPath $vswhere)) { throw "Specify -VCRedistRoot pointing to the MSVC redistributable x64 directory." }
  $vsInstall = (& $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
  if ([string]::IsNullOrWhiteSpace($vsInstall)) { throw "Visual C++ redistributable directory was not found." }
  $redistVersion = Get-ChildItem -LiteralPath (Join-Path $vsInstall "VC\Redist\MSVC") -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
  if ($null -eq $redistVersion) { throw "Visual C++ redistributable version was not found." }
  $VCRedistRoot = Join-Path $redistVersion.FullName "x64"
}
foreach ($runtimeName in @("concrt140.dll", "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll", "msvcp140_codecvt_ids.dll", "vcruntime140.dll", "vcruntime140_1.dll")) {
  Copy-PayloadFile -Source (Join-Path $VCRedistRoot "Microsoft.VC143.CRT\$runtimeName") -RelativePath $runtimeName
}
Copy-PayloadFile -Source (Join-Path $VCRedistRoot "Microsoft.VC143.OpenMP\vcomp140.dll") -RelativePath "vcomp140.dll"
if ($IncludeSymbols) {
  Copy-PayloadFile -Source (Join-Path $sourcePath "SceneViewer.pdb") -RelativePath "SceneViewer.pdb"
}

# Runtime Slang compilation needs source files and their original relative include layout.
$shaderTrees = @(
  @{
    Source = Join-Path $repoRoot "MiniEngine\Model\Shaders"
    Destination = Join-Path $payloadRoot "MiniEngine\Model\Shaders"
  },
  @{
    Source = Join-Path $repoRoot "MiniEngine\Core\Shaders"
    Destination = Join-Path $payloadRoot "MiniEngine\Core\Shaders"
  }
)

foreach ($shaderTree in $shaderTrees) {
  Write-Host "Including shader tree: $($shaderTree.Source)"
  Copy-DirectoryContents -Source $shaderTree.Source -Destination $shaderTree.Destination
}

$modelShaderHeaders = @(
  "StructsIO.h",
  "MeshletStructs.h",
  "VirtualShadowMapShared.h",
  "VirtualShadowMapCullShared.h"
)
$modelPackageRoot = Join-Path $payloadRoot "MiniEngine\Model"
New-Item -ItemType Directory -Path $modelPackageRoot -Force | Out-Null
foreach ($headerName in $modelShaderHeaders) {
  $headerSource = Join-Path (Join-Path $repoRoot "MiniEngine\Model") $headerName
  if (-not (Test-Path -LiteralPath $headerSource -PathType Leaf)) {
    throw "Required shader header not found: $headerSource"
  }
  Copy-Item -LiteralPath $headerSource -Destination (Join-Path $modelPackageRoot $headerName) -Force
}

# One precompiled environment preserves the default lighting without source HDRs.
$hdriSourceRoot = Join-Path $repoRoot "MiniEngine\SceneViewer\Textures\HDRIs"
$hdriDestRoot = Join-Path $payloadRoot "Textures\HDRIs"
$environmentSource = Resolve-ChildPath -Root $hdriSourceRoot -RelativePath $EnvironmentMap
Assert-DdsTexture -Path $environmentSource
Copy-PayloadFile -Source $environmentSource -RelativePath ("Textures\HDRIs\" + [IO.Path]::GetFileName($EnvironmentMap))

# Keep the tiny default scene and a complete, precompiled Bistro runtime.
$assetSourceRoot = Join-Path $repoRoot "MiniEngine\SceneViewer\Assets"
foreach ($sampleName in @("bunny", "bistro-metalrough")) {
  $sampleRoot = Join-Path $assetSourceRoot $sampleName
  $modelName = if ($sampleName -eq "bunny") { "bunny" } else { "bistro_metalrough" }
  $modelSource = Join-Path $sampleRoot "$modelName.gltf"
  $cacheSource = Join-Path $sampleRoot "$modelName.mini"
  Assert-MiniCache -Path $cacheSource -SourceModel $modelSource
  foreach ($fileName in @("$modelName.gltf", "$modelName.mini")) {
    Copy-PayloadFile -Source (Join-Path $sampleRoot $fileName) -RelativePath "Assets\$sampleName\$fileName"
  }
  $gltf = Get-Content -LiteralPath $modelSource -Raw | ConvertFrom-Json
  foreach ($buffer in $gltf.buffers) {
    $bufferPath = [Uri]::UnescapeDataString([string]$buffer.uri)
    Copy-PayloadFile -Source (Resolve-ChildPath -Root $sampleRoot -RelativePath $bufferPath) -RelativePath "Assets\$sampleName\$bufferPath"
  }
  $imagesProperty = $gltf.PSObject.Properties['images']
  if ($null -ne $imagesProperty) {
    foreach ($image in $gltf.images) {
      $texturePath = [IO.Path]::ChangeExtension([Uri]::UnescapeDataString([string]$image.uri), ".dds")
      $textureSource = Resolve-ChildPath -Root $sampleRoot -RelativePath $texturePath
      Assert-DdsTexture -Path $textureSource
      Copy-PayloadFile -Source $textureSource -RelativePath "Assets\$sampleName\$texturePath"
    }
  }
  Write-Host "Included prebuilt scene: $sampleName"
}

# Preserve notice paths, including the files referenced by THIRD_PARTY_NOTICES.md.
$extraFiles = @(
  "LICENSE", "THIRD_PARTY_NOTICES.md", "MiniEngine/License.txt",
  "MiniEngine/ThirdParty/imgui/LICENSE.txt", "MiniEngine/ThirdParty/imgui/imstb_rectpack.h",
  "MiniEngine/ThirdParty/imgui/imstb_textedit.h", "MiniEngine/ThirdParty/imgui/imstb_truetype.h",
  "MiniEngine/ThirdParty/meshoptimizer/LICENSE.md", "MiniEngine/ThirdParty/lz4/LICENSE",
  "MiniEngine/ThirdParty/cgltf/LICENSE", "MiniEngine/Model/json.hpp",
  "MiniEngine/ThirdParty/slang-2026.10/LICENSE",
  "MiniEngine/ThirdParty/dxc_2026_05_27/LICENSE-MIT.txt",
  "MiniEngine/ThirdParty/dxc_2026_05_27/LICENSE-LLVM.txt",
  "MiniEngine/ThirdParty/dxc_2026_05_27/LICENSE-MS.txt",
  "MiniEngine/Core/XeGTAO.cpp", "MiniEngine/SceneViewer/Assets/bistro-metalrough/LICENSE"
)
foreach ($name in $extraFiles) {
  Copy-PayloadFile -Source (Join-Path $repoRoot $name) -RelativePath $name
}
Copy-PayloadFile -Source (Join-Path $assetSourceRoot "bistro-metalrough\LICENSE") -RelativePath "Assets\bistro-metalrough\LICENSE"
foreach ($dependencyName in @("directxmesh_desktop_win10.2024.10.29.1", "directxtex_desktop_win10.2024.10.29.1", "Microsoft.Direct3D.D3D12.1.616.1", "WinPixEventRuntime.1.0.210209001", "zlib-msvc-x64.1.2.11.8900")) {
  $dependencyRoot = Join-Path $repoRoot "Packages\$dependencyName"
  $notices = @(Get-ChildItem -LiteralPath $dependencyRoot -Recurse -File | Where-Object { $_.Name -match '(?i)^(license|notice|copying|copyright)' -or $_.Name -eq 'zlib.h' })
  foreach ($notice in $notices) {
    Copy-PayloadFile -Source $notice.FullName -RelativePath $notice.FullName.Substring($repoRoot.Length + 1)
  }
}

$launcher = @'
@echo off
setlocal
pushd "%~dp0"
SceneViewer.exe -model "Assets\bistro-metalrough\bistro_metalrough.gltf" %*
set "result=%errorlevel%"
popd
exit /b %result%
'@
[IO.File]::WriteAllText((Join-Path $payloadRoot "LaunchBistro.cmd"), ($launcher -replace "\r?\n", "`r`n") + "`r`n", [Text.Encoding]::ASCII)
$readme = @"
Nyx $Version - Virtual Shadow Maps and Bistro Demo

Quick start
  Extract the entire ZIP to a writable directory and run LaunchBistro.cmd.
  Running SceneViewer.exe directly opens the default bunny scene.

Requirements
  Windows 10/11 x64; a current graphics driver; DirectX 12 mesh shaders,
  Shader Model 6.6, and 64-bit integer atomics on typed textures.

Included
  Bistro with prebuilt geometry, original-resolution BC7 textures, and animations.
  The default bunny scene and one precompiled environment map.
  Slang/DXC, D3D12 Agility, Visual C++ and OpenMP runtime dependencies.
  Shaders compile at startup; scene geometry and textures are already compiled.

Controls
  W/A/S/D and Q/E: move; mouse: look; mouse wheel: movement speed.
  Shift: toggle fine movement. Backspace: show/hide tuning UI.
  Hold Alt to interact with the UI.
  Camera/Lock Camera: lock/unlock the current view.
  Application/Pause Animations: pause/resume all model animations.
  Application/GLTF: switch included scenes; Application/IBL: environment.

The package preserves the renderer's defaults, including monitor-native resolution.
Use Graphics/Display in the tuning UI to adjust rendering and window resolution.

Source and documentation: https://github.com/moonlovelj/Nyx
Zorah demo: https://www.youtube.com/watch?v=4YmfQTLy4jc
Bistro source: https://github.com/moonlovelj/bistro-metalrough
Source commit: $sourceCommit
See LICENSE, THIRD_PARTY_NOTICES.md, and the included component licenses.
"@
[IO.File]::WriteAllText((Join-Path $payloadRoot "README.txt"), $readme, [Text.Encoding]::ASCII)
$bistroCommit = (& git -C (Join-Path $assetSourceRoot "bistro-metalrough") rev-parse HEAD).Trim()
$exeHash = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash.ToLowerInvariant()
$buildInfo = "Version: $Version`r`nSource commit: $sourceCommit`r`nBistro commit: $bistroCommit`r`nConfiguration: Release x64`r`nMINI cache version: $miniVersion`r`nSceneViewer.exe SHA256: $exeHash`r`n"
[IO.File]::WriteAllText((Join-Path $payloadRoot "BUILDINFO.txt"), $buildInfo, [Text.Encoding]::ASCII)

$requiredPackagePaths = @(
  "SceneViewer.exe",
  "slang.dll",
  "slang-compiler.dll",
  "slang-rt.dll",
  "slang-standard-module-2026.10",
  "dxcompiler.dll",
  "dxil.dll",
  "D3D12\D3D12Core.dll",
  "vcruntime140.dll",
  "msvcp140.dll",
  "vcomp140.dll",
  "LaunchBistro.cmd",
  "README.txt",
  "MiniEngine\Model\Shaders\VBufferMesh.slang",
  "MiniEngine\Model\StructsIO.h",
  "MiniEngine\Model\MeshletStructs.h",
  "MiniEngine\Model\VirtualShadowMapShared.h",
  "MiniEngine\Model\VirtualShadowMapCullShared.h",
  "MiniEngine\Core\Shaders\Math.hlsli",
  "Textures\HDRIs",
  "Assets\bunny\bunny.mini",
  "Assets\bistro-metalrough\bistro_metalrough.gltf",
  "Assets\bistro-metalrough\bistro_metalrough.mini",
  "Assets\bistro-metalrough\bistro-anim.bin",
  "Assets\bistro-metalrough\LICENSE"
)

foreach ($relativePath in $requiredPackagePaths) {
  $requiredPath = Join-Path $payloadRoot $relativePath
  if (-not (Test-Path -LiteralPath $requiredPath)) {
    throw "Required package content missing: $relativePath"
  }
}

if (-not $IncludeSymbols) {
  $symbolPatterns = @("*.pdb", "*.ipdb", "*.iobj", "*.ilk")
  foreach ($pattern in $symbolPatterns) {
    Get-ChildItem -LiteralPath $payloadRoot -Recurse -File -Filter $pattern -ErrorAction SilentlyContinue |
      Remove-Item -Force
  }
}

if (Test-Path -LiteralPath $zipPath) {
  Remove-Item -LiteralPath $zipPath -Force
}
if (Test-Path -LiteralPath $shaPath) {
  Remove-Item -LiteralPath $shaPath -Force
}

Write-Host "Creating package: $zipPath"
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($payloadRoot, $zipPath, [IO.Compression.CompressionLevel]::Optimal, $true)
if ((Get-Item -LiteralPath $zipPath).Length -ge 2GB) {
  throw "Package exceeds the GitHub Release per-file limit. Split the scene assets before publishing."
}

$sha256 = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
$shaLine = "$sha256 *$([IO.Path]::GetFileName($zipPath))"
Set-Content -LiteralPath $shaPath -Value $shaLine -Encoding ascii

if (-not $KeepStaging) {
  Remove-Item -LiteralPath $stagingRoot -Recurse -Force
}

Write-Host ""
Write-Host "Done."
Write-Host "ZIP:      $zipPath"
Write-Host "SHA256:   $sha256"
Write-Host "SHA file: $shaPath"
