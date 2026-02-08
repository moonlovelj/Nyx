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
