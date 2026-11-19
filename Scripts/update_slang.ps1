#requires -Version 5.1
<#
.SYNOPSIS
Checks or updates the vendored Slang Windows x64 SDK using the latest official stable release.
.EXAMPLE
.\Scripts\update_slang.ps1 -CheckOnly
Check for an update from the repository root.
.EXAMPLE
.\update_slang.ps1
Install the latest stable release from the Scripts directory.
#>
[CmdletBinding()]
param([switch] $CheckOnly)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$repo = Split-Path -Parent $PSScriptRoot
$relativeSdk = 'MiniEngine/ThirdParty/Slang'
$sdk = [IO.Path]::GetFullPath((Join-Path $repo $relativeSdk))
$parent = [IO.Path]::GetDirectoryName($sdk)
$manifestName = 'NyxSlangSdk.props'
$backup = Join-Path $parent '.nyx-slang-previous'
$releaseBase = 'https://github.com/shader-slang/slang/releases'
$tagPattern = '^v([0-9]{4}\.[0-9]+(?:\.[0-9]+){0,2})$'
$assetFiles = @(
    'LICENSE', 'README.md',
    'bin/gfx.dll', 'bin/gfx.slang', 'bin/slang-compiler.dll', 'bin/slang-glsl-module.dll',
    'bin/slang-glslang.dll', 'bin/slang-rt.dll', 'bin/slang.dll', 'bin/slang.exe',
    'bin/slang.slang', 'bin/slangc.exe', 'bin/slangd.exe', 'bin/slangi.exe',
    'lib/gfx.lib', 'lib/slang-compiler.lib', 'lib/slang-rt.lib', 'lib/slang.lib'
)

function Get-ReleaseInfo {
    $release = $null
    $headers = @{ Accept = 'application/vnd.github+json'; 'User-Agent' = 'Nyx-Slang-Updater' }
    if ($env:GITHUB_TOKEN) { $headers.Authorization = "Bearer $env:GITHUB_TOKEN" }
    try {
        $release = Invoke-RestMethod -Uri 'https://api.github.com/repos/shader-slang/slang/releases/latest' -Headers $headers -TimeoutSec 30
    } catch {
        $response = $_.Exception.Response
        if (!$response -or [int]$response.StatusCode -notin @(403, 429)) { throw }
        # GitHub also publishes the digest next to the exact asset on its release page.
        $latest = Invoke-WebRequest -UseBasicParsing -Uri "$releaseBase/latest" -TimeoutSec 30
        $latestUri = $latest.BaseResponse.ResponseUri
        if (!$latestUri) { $latestUri = $latest.BaseResponse.RequestMessage.RequestUri }
        if ($latestUri.AbsoluteUri -notmatch '^https://github\.com/shader-slang/slang/releases/tag/([^/?#]+)$') {
            throw "Unrecognized latest Slang release URL: $latestUri"
        }
        $tag = $Matches[1]
    }

    if ($release) {
        if ($release.draft -ne $false -or $release.prerelease -ne $false) { throw 'Expected a published stable release.' }
        $tag = $release.tag_name
    }
    if ($tag -cnotmatch $tagPattern) { throw "Unexpected stable release tag: $tag" }
    $version = $Matches[1]
    $name = "slang-$version-windows-x86_64.zip"
    $url = "$releaseBase/download/$tag/$name"
    if ($release) {
        $assets = @($release.assets | Where-Object { $_.name -ceq $name -and $_.state -eq 'uploaded' })
        if ($assets.Count -ne 1 -or $assets[0].browser_download_url -cne $url) {
            throw "Missing or ambiguous official release asset: $name"
        }
        $digest = $assets[0].digest
    } else {
        $html = (Invoke-WebRequest -UseBasicParsing -Uri "$releaseBase/expanded_assets/$tag" -TimeoutSec 30).Content
        $assetHref = [regex]::Escape("/shader-slang/slang/releases/download/$tag/$name")
        if ([regex]::Matches($html, ('href="' + $assetHref + '"')).Count -ne 1) { throw "Missing or ambiguous release asset: $name" }
        $label = [regex]::Escape("Copy to clipboard digest for $name")
        $copies = [regex]::Matches($html, ('<clipboard-copy\b(?=[^>]*\baria-label="' + $label + '")(?=[^>]*\bvalue="(sha256:[0-9a-fA-F]{64})")[^>]*>'))
        if ($copies.Count -ne 1) { throw "Cannot verify the official digest for $name" }
        $digest = $copies[0].Groups[1].Value
    }
    if ($digest -notmatch '^sha256:([0-9a-fA-F]{64})$') { throw "Invalid SHA-256 digest for $name" }
    [pscustomobject]@{ Version = $version; Tag = $tag; Asset = $name; Url = $url; Sha256 = $Matches[1].ToLowerInvariant(); Modules = "slang-standard-module-$version" }
}

function Assert-SdkFiles($directory, $version, $modules) {
    foreach ($file in ($assetFiles + @('include/slang.h', 'include/slang-com-ptr.h', 'include/slang-tag-version.h'))) {
        $path = Join-Path $directory $file
        if (![IO.File]::Exists($path) -or (Get-Item -LiteralPath $path).Length -eq 0) { throw "Missing SDK file: $path" }
    }
    $header = [IO.File]::ReadAllText((Join-Path $directory 'include/slang-tag-version.h'))
    $expectedMacro = '(?m)^\s*#define\s+SLANG_TAG_VERSION\s+"' + [regex]::Escape($version) + '"'
    if ($header -notmatch $expectedMacro) { throw "SDK header does not identify version $version." }
    $moduleDir = Join-Path $directory "bin/$modules"
    if (!(Get-ChildItem -LiteralPath $moduleDir -File -Recurse | Select-Object -First 1)) { throw "Empty standard module directory: $moduleDir" }
}

function Read-InstalledSdk {
    [xml]$xml = [IO.File]::ReadAllText((Join-Path $sdk $manifestName))
    $props = $xml.Project.PropertyGroup
    $version = [string]$props.NyxSlangVersion
    if ("v$version" -cnotmatch $tagPattern -or $props.NyxSlangTag -cne "v$version" -or
        $props.NyxSlangModuleDirectory -cne "slang-standard-module-$version" -or
        $props.NyxSlangAsset -cne "slang-$version-windows-x86_64.zip" -or
        [string]$props.NyxSlangSha256 -notmatch '^[0-9a-fA-F]{64}$' -or
        [string]$props.NyxSlangContentSha256 -notmatch '^[0-9a-fA-F]{64}$') {
        throw "Invalid installed SDK manifest: $sdk\$manifestName"
    }
    Assert-SdkFiles $sdk $version $props.NyxSlangModuleDirectory
    return $props
}

function Get-SdkContentSha256([string]$directory) {
    $root = [IO.DirectoryInfo]::new($directory)
    if ($root.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Unexpected SDK link: $directory" }
    $pending = [Collections.Generic.Stack[IO.DirectoryInfo]]::new()
    $pending.Push($root)
    $paths = [Collections.Generic.List[string]]::new()
    while ($pending.Count) {
        foreach ($item in $pending.Pop().EnumerateFileSystemInfos()) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Unexpected SDK link: $($item.FullName)" }
            if ($item -is [IO.DirectoryInfo]) {
                $pending.Push($item)
            } else {
                $path = $item.FullName.Substring($root.FullName.Length + 1).Replace('\', '/')
                if ($path -cne $manifestName) { $paths.Add($path) }
            }
        }
    }
    # Hash the UTF-8 (no BOM) text of a standard SHA-256 file list in ordinal path order.
    $paths.Sort([StringComparer]::Ordinal)
    $fileList = [Text.StringBuilder]::new()
    foreach ($path in $paths) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $root.FullName $path) -Algorithm SHA256).Hash.ToLowerInvariant()
        [void]$fileList.Append($hash).Append('  ').Append($path).Append("`n")
    }
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($fileList.ToString()))
        return [BitConverter]::ToString($digest).Replace('-', '').ToLowerInvariant()
    } finally { $sha.Dispose() }
}

function Assert-CleanSdk($installedHash, $manifestHash) {
    if ((Get-FileHash -LiteralPath (Join-Path $sdk $manifestName)).Hash -ne $manifestHash -or
        (Get-SdkContentSha256 $sdk) -ne $installedHash) {
        throw "The installed SDK contains local changes. Save them before updating: $sdk"
    }
}

function Expand-SelectedFiles($archive, $directory, $modules) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName
            $legalNotice = $name -match '^(?:licen[cs]es?|copying|notices?|third[-_]party[-_]notices?)(?:[._-][^/]*)?(?:/|$)'
            if (!$entry.Name -or ($name -notin $assetFiles -and !$name.StartsWith('include/') -and
                !$name.StartsWith("bin/$modules/") -and !$legalNotice)) { continue }
            if ($name -match '(?:^|/)(?:\.|\.\.)(?:/|$)|[\\:]') { throw "Unexpected archive path: $name" }
            $path = [IO.Path]::GetFullPath((Join-Path $directory $name))
            if (!$path.StartsWith($directory + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Archive path leaves staging directory: $name"
            }
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $path, $false)
            # MSBuild tracks headers and import libraries by timestamp at the fixed SDK path.
            [IO.File]::SetLastWriteTimeUtc($path, [DateTime]::UtcNow)
        }
    } finally { $zip.Dispose() }
}

function Write-Manifest($directory, $info, $contentHash) {
    $xml = @"
<?xml version="1.0" encoding="utf-8"?>
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <NyxSlangVersion>$($info.Version)</NyxSlangVersion>
    <NyxSlangTag>$($info.Tag)</NyxSlangTag>
    <NyxSlangAsset>$($info.Asset)</NyxSlangAsset>
    <NyxSlangSha256>$($info.Sha256)</NyxSlangSha256>
    <NyxSlangContentSha256>$contentHash</NyxSlangContentSha256>
    <NyxSlangModuleDirectory>$($info.Modules)</NyxSlangModuleDirectory>
  </PropertyGroup>
</Project>
"@
    [IO.File]::WriteAllText((Join-Path $directory $manifestName), $xml.Replace("`r`n", "`n") + "`n")
}

function Update-Sdk {
    if ([IO.Directory]::Exists($backup) -and ![IO.Directory]::Exists($sdk)) {
        if ($CheckOnly) { throw "An interrupted update left the installed SDK at $backup. Run without -CheckOnly to restore it." }
        [IO.Directory]::Move($backup, $sdk)
        Write-Host 'Restored the previous SDK following an interrupted update.'
    }
    if ([IO.Directory]::Exists($backup)) { Write-Warning "Previous SDK backup still exists at $backup." }
    $manifestHash = (Get-FileHash -LiteralPath (Join-Path $sdk $manifestName)).Hash
    $installed = Read-InstalledSdk
    $latest = Get-ReleaseInfo
    $comparison = ([version]$latest.Version).CompareTo([version]$installed.NyxSlangVersion)
    if ($comparison -eq 0) {
        if ($latest.Sha256 -ne $installed.NyxSlangSha256) { throw "The published digest for $($latest.Tag) differs from the installed lock." }
        Write-Host "Slang $($latest.Version) is up to date."
        return
    }
    if ($comparison -lt 0) { Write-Host "Installed Slang $($installed.NyxSlangVersion) is newer than the latest stable release $($latest.Version)."; return }
    Write-Host "Slang $($installed.NyxSlangVersion) -> $($latest.Version) ($($latest.Asset))"
    if ($CheckOnly) { return }
    if ([IO.Directory]::Exists($backup)) { throw "Inspect and move the existing SDK backup before another update: $backup" }
    Assert-CleanSdk $installed.NyxSlangContentSha256 $manifestHash
    $work = Join-Path $parent ('.nyx-slang-update-' + [guid]::NewGuid().ToString('N'))
    $stage = Join-Path $work 'sdk'
    [void][IO.Directory]::CreateDirectory($stage)
    try {
        $archive = Join-Path $work $latest.Asset
        Invoke-WebRequest -UseBasicParsing -Uri $latest.Url -OutFile $archive -TimeoutSec 300
        if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $latest.Sha256) { throw "SHA-256 verification failed for $($latest.Asset)." }
        Expand-SelectedFiles $archive $stage $latest.Modules
        Assert-SdkFiles $stage $latest.Version $latest.Modules
        Write-Manifest $stage $latest (Get-SdkContentSha256 $stage)
        Assert-CleanSdk $installed.NyxSlangContentSha256 $manifestHash
        [IO.Directory]::Move($sdk, $backup)
        try { [IO.Directory]::Move($stage, $sdk) } catch {
            [IO.Directory]::Move($backup, $sdk)
            throw
        }
        # The backup is the previously checked directory at the fixed sibling path.
        try { [IO.Directory]::Delete($backup, $true) } catch {
            Write-Warning "The new SDK is active. Close processes using the old SDK and clear its remaining backup at $backup. $($_.Exception.Message)"
        }
        Write-Host "Installed Slang $($latest.Version). Review the SDK changes and run Tests."
    } finally {
        # Only the newly allocated staging directory can be removed here.
        if ([IO.Directory]::Exists($work)) { [IO.Directory]::Delete($work, $true) }
    }
}

# Serialize updates for this checkout, including recovery after an interrupted rename.
$sha = [Security.Cryptography.SHA256]::Create()
try { $key = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($sdk.ToUpperInvariant()))).Replace('-', '') }
finally { $sha.Dispose() }
$mutex = [Threading.Mutex]::new($false, "Local\Nyx-Slang-$key")
$locked = $false
try {
    try { $locked = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (!$locked) { throw 'Another Slang update is running in this checkout.' }
    Update-Sdk
} finally {
    if ($locked) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
