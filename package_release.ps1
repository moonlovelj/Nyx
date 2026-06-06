<#
Package Nyx SceneViewer Release build into a distributable zip and emit SHA256.

Examples:
  .\package_release.ps1
  .\package_release.ps1 -Version "v1.0.0"
  .\package_release.ps1 -PackageName "Nyx-zorah-prebuilt"
  .\package_release.ps1 -SourceDir "MiniEngine\Build\x64\Release\Output\SceneViewer" -IncludeSymbols
#>

param(
  [string]$SourceDir = "MiniEngine\Build\x64\Release\Output\SceneViewer",
  [string]$DistDir = "dist",
  [string]$PackageName = "Nyx-SceneViewer-win64",
  [string]$Version = "",
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

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourcePath = Resolve-PathFromBase -Path $SourceDir -Base $repoRoot
$distPath = Resolve-PathFromBase -Path $DistDir -Base $repoRoot

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
$zipPath = Join-Path $distPath "$packageStem.zip"
$shaPath = "$zipPath.sha256"
$stagingRoot = Join-Path $distPath "_staging_$packageStem"
$payloadRoot = Join-Path $stagingRoot $packageStem

New-Item -ItemType Directory -Path $distPath -Force | Out-Null

if (Test-Path -LiteralPath $stagingRoot) {
  Remove-Item -LiteralPath $stagingRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $payloadRoot -Force | Out-Null

Write-Host "Collecting files from: $sourcePath"
Get-ChildItem -LiteralPath $sourcePath -Force | Copy-Item -Destination $payloadRoot -Recurse -Force

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

$modelShaderHeaders = @("StructsIO.h", "MeshletStructs.h")
$modelPackageRoot = Join-Path $payloadRoot "MiniEngine\Model"
New-Item -ItemType Directory -Path $modelPackageRoot -Force | Out-Null
foreach ($headerName in $modelShaderHeaders) {
  $headerSource = Join-Path (Join-Path $repoRoot "MiniEngine\Model") $headerName
  if (-not (Test-Path -LiteralPath $headerSource -PathType Leaf)) {
    throw "Required shader header not found: $headerSource"
  }
  Copy-Item -LiteralPath $headerSource -Destination (Join-Path $modelPackageRoot $headerName) -Force
}

# Package the source HDR files. SceneViewer generates the DDS cache on first use.
$hdriSourceRoot = Join-Path $repoRoot "MiniEngine\SceneViewer\Textures\HDRIs"
$hdriDestRoot = Join-Path $payloadRoot "Textures\HDRIs"
if (-not (Test-Path -LiteralPath $hdriSourceRoot -PathType Container)) {
  throw "Required HDRI directory not found: $hdriSourceRoot"
}

$sourceHdriFiles = @(Get-ChildItem -LiteralPath $hdriSourceRoot -File -Filter "*.hdr")
if ($sourceHdriFiles.Count -eq 0) {
  throw "No source HDRI files found under: $hdriSourceRoot"
}

New-Item -ItemType Directory -Path $hdriDestRoot -Force | Out-Null
foreach ($hdriFile in $sourceHdriFiles) {
  Copy-Item -LiteralPath $hdriFile.FullName -Destination $hdriDestRoot -Force
}

# Always include selected sample assets from the repo asset source.
$assetSourceRoot = Join-Path $repoRoot "MiniEngine\SceneViewer\Assets"
$assetDestRoot = Join-Path $payloadRoot "Assets"
New-Item -ItemType Directory -Path $assetDestRoot -Force | Out-Null

$requiredAssets = @("bunny", "Jinx")
foreach ($assetName in $requiredAssets) {
  $assetSrc = Join-Path $assetSourceRoot $assetName
  $assetDst = Join-Path $assetDestRoot $assetName
  if (Test-Path -LiteralPath $assetSrc) {
    Write-Host "Including asset: $assetName"
    if (Test-Path -LiteralPath $assetDst) {
      Remove-Item -LiteralPath $assetDst -Recurse -Force
    }
    Copy-Item -LiteralPath $assetSrc -Destination $assetDestRoot -Recurse -Force
  } else {
    Write-Warning "Asset folder not found, skipped: $assetSrc"
  }
}

# Bundle top-level docs for redistribution context.
$extraFiles = @("README.md", "LICENSE", "THIRD_PARTY_NOTICES.md")
foreach ($name in $extraFiles) {
  $src = Join-Path $repoRoot $name
  if (Test-Path -LiteralPath $src) {
    Copy-Item -LiteralPath $src -Destination (Join-Path $payloadRoot $name) -Force
  }
}

$requiredPackagePaths = @(
  "SceneViewer.exe",
  "slang.dll",
  "slang-compiler.dll",
  "slang-rt.dll",
  "slang-standard-module-2026.10",
  "dxcompiler.dll",
  "dxil.dll",
  "MiniEngine\Model\Shaders\VBufferMesh.slang",
  "MiniEngine\Model\StructsIO.h",
  "MiniEngine\Model\MeshletStructs.h",
  "MiniEngine\Core\Shaders\Math.hlsli",
  "Textures\HDRIs",
  "Assets\bunny"
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
Compress-Archive -Path (Join-Path $stagingRoot "*") -DestinationPath $zipPath -CompressionLevel Optimal

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
