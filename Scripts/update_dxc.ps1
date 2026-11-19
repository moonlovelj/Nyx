#requires -Version 5.1
<#
.SYNOPSIS
Checks or updates the bundled Windows x64 DirectX Shader Compiler from Microsoft's GitHub releases.
.EXAMPLE
.\Scripts\update_dxc.ps1 -Channel preview -CheckOnly
Check the preview channel from the repository root.
.EXAMPLE
.\update_dxc.ps1 -Channel release
Install the latest stable release from the Scripts directory.
#>
[CmdletBinding()]
param(
    [ValidateSet('release', 'preview')][string] $Channel = 'release',
    [switch] $CheckOnly
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
$Channel = $Channel.ToLowerInvariant()
$repoRoot = Split-Path -Parent $PSScriptRoot
$parent = Join-Path $repoRoot 'MiniEngine/ThirdParty'
$sdk = Join-Path $parent 'DXC'
$backup = Join-Path $parent '.nyx-dxc-previous'
$manifestName = 'NyxDxcSdk.props'
$api = 'https://api.github.com/repos/microsoft/DirectXShaderCompiler/releases'
$downloadBase = 'https://github.com/microsoft/DirectXShaderCompiler/releases/download'
$assetPattern = '^dxc_(?:preview_)?\d{4}_\d{2}_\d{2}\.zip$'
$tagPattern = '^v[0-9][0-9A-Za-z._-]*$'
$runtimeFiles = @('bin/x64/dxc.exe', 'bin/x64/dxcompiler.dll', 'bin/x64/dxil.dll', 'bin/x64/dxv.exe')
$requiredFiles = $runtimeFiles + @('LICENSE-MIT.txt', 'LICENSE-LLVM.txt', 'LICENSE-MS.txt')

# The public release page remains available when the anonymous REST quota is exhausted.
function Get-PublishedReleasePage {
    $releasesPage = 'https://github.com/microsoft/DirectXShaderCompiler/releases'
    $latest = Invoke-WebRequest -UseBasicParsing -Uri "$releasesPage/latest" -TimeoutSec 30
    $uri = $latest.BaseResponse.ResponseUri
    if (!$uri) { $uri = $latest.BaseResponse.RequestMessage.RequestUri }
    if ($uri.AbsoluteUri -cnotmatch '^https://github\.com/microsoft/DirectXShaderCompiler/releases/tag/([^/?#]+)$') {
        throw "Unexpected DXC latest release URL: $uri"
    }
    $tag = $Matches[1]
    if ($tag -cnotmatch $tagPattern) { throw "Unrecognized DXC stable tag: $tag" }

    # Commit and individual asset timestamps are also displayed; only accept the release byline.
    $times = [regex]::Matches($latest.Content, 'released this\s*<relative-time\b[^>]*\bdatetime="(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)"')
    if ($times.Count -ne 1) { throw "Cannot identify the publication time of $tag on GitHub." }
    $html = (Invoke-WebRequest -UseBasicParsing -Uri "$releasesPage/expanded_assets/$tag" -MaximumRedirection 0 -TimeoutSec 30).Content
    $prefix = [regex]::Escape("/microsoft/DirectXShaderCompiler/releases/download/$tag/")
    $links = @([regex]::Matches($html, ('href="' + $prefix + '([^"/?#]+)"')) | Where-Object {
        $_.Groups[1].Value -cmatch $assetPattern
    })
    if ($links.Count -ne 1) { throw "Missing or ambiguous Windows DXC archive on the release page for $tag." }
    $name = $links[0].Groups[1].Value
    $label = [regex]::Escape("Copy to clipboard digest for $name")
    $digests = [regex]::Matches($html, ('<clipboard-copy\b(?=[^>]*\baria-label="' + $label + '")(?=[^>]*\bvalue="(sha256:[0-9a-fA-F]{64})")[^>]*>'))
    if ($digests.Count -ne 1) { throw "Cannot verify the official digest for $name." }

    [pscustomobject]@{
        tag_name = $tag; published_at = $times[0].Groups[1].Value; draft = $false; prerelease = $false
        assets = @([pscustomobject]@{ name = $name; state = 'uploaded'; browser_download_url = "$downloadBase/$tag/$name"; digest = $digests[0].Groups[1].Value })
    }
}

function Request-ReleaseData($uri) {
    $headers = @{ Accept = 'application/vnd.github+json'; 'User-Agent' = 'Nyx-DXC-Updater' }
    if ($env:GITHUB_TOKEN) { $headers.Authorization = "Bearer $env:GITHUB_TOKEN" }
    try {
        Invoke-RestMethod -Uri $uri -Headers $headers -TimeoutSec 30
    } catch {
        $response = $_.Exception.Response
        if ($response -and [int]$response.StatusCode -in @(403, 429)) {
            if (!$env:GITHUB_TOKEN -and $uri -ceq "$api/latest") {
                $remaining = $null
                if ($response.Headers -is [System.Net.WebHeaderCollection]) {
                    $remaining = $response.Headers['X-RateLimit-Remaining']
                } elseif ($response.Headers.Contains('X-RateLimit-Remaining')) {
                    $remaining = $response.Headers.GetValues('X-RateLimit-Remaining') -join ','
                }
                if ($remaining -eq '0') { return Get-PublishedReleasePage }
            }
            throw 'GitHub denied the release query or its API rate limit was reached. Set GITHUB_TOKEN for authenticated access, or retry later.'
        }
        throw
    }
}

function Get-ReleaseInfo {
    if ($Channel -eq 'release') {
        $release = Request-ReleaseData "$api/latest"
        if ($release.draft -ne $false -or $release.prerelease -ne $false) { throw 'Expected a published stable DXC release.' }
    } else {
        # Releases are not strictly ordered by publication date. Compare published_at across pages.
        $page = 1
        $release = $null
        do {
            $batch = Request-ReleaseData ("{0}?per_page=100&page={1}" -f $api, $page)
            foreach ($candidate in $batch) {
                if ($candidate.draft -eq $false -and $candidate.prerelease -eq $true -and $candidate.published_at -and
                    (!$release -or [DateTimeOffset]$candidate.published_at -gt [DateTimeOffset]$release.published_at -or
                    ([DateTimeOffset]$candidate.published_at -eq [DateTimeOffset]$release.published_at -and $candidate.id -gt $release.id))) {
                    $release = $candidate
                }
            }
            $page++
        } while ($batch.Count -eq 100)
        if (!$release) { throw 'Microsoft has not published a DXC preview.' }
    }

    $tag = [string]$release.tag_name
    if ($tag -cnotmatch $tagPattern -or !$release.published_at) { throw "Unrecognized DXC release metadata: $tag" }
    # Some preview releases use the same Windows ZIP naming convention as stable releases.
    $assets = @($release.assets | Where-Object { $_.name -cmatch $assetPattern -and $_.state -eq 'uploaded' })
    if ($assets.Count -ne 1) { throw "Expected exactly one DXC Windows binary archive in $tag. Found $($assets.Count)." }
    $asset = $assets[0]
    $url = "$downloadBase/$tag/$($asset.name)"
    if ($asset.browser_download_url -cne $url -or [string]$asset.digest -notmatch '^sha256:([0-9a-fA-F]{64})$') {
        throw "Cannot verify the official download URL and SHA-256 for $($asset.name)."
    }
    [pscustomobject]@{
        Tag = $tag; Channel = $Channel; Asset = $asset.name; Url = $url; Sha256 = $Matches[1].ToLowerInvariant()
        PublishedAt = ([DateTimeOffset]$release.published_at).UtcDateTime.ToString('yyyy-MM-ddTHH:mm:ssZ', [cultureinfo]::InvariantCulture)
    }
}

function Assert-SdkFiles([string]$directory) {
    foreach ($relative in $requiredFiles) {
        $path = Join-Path $directory $relative
        if (![IO.File]::Exists($path) -or (Get-Item -LiteralPath $path).Length -eq 0) { throw "Missing DXC SDK file: $path" }
    }
}

function Get-SdkContentSha256([string]$directory) {
    # Same file-list format as the Slang lock: ordinal relative paths, lowercase SHA-256, UTF-8, LF.
    $root = [IO.DirectoryInfo]::new($directory)
    $paths = [Collections.Generic.List[string]]::new()
    $pending = [Collections.Generic.Stack[IO.DirectoryInfo]]::new()
    $pending.Push($root)
    while ($pending.Count) {
        $dir = $pending.Pop()
        if ($dir.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Unexpected DXC SDK link: $($dir.FullName)" }
        foreach ($item in $dir.EnumerateFileSystemInfos()) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Unexpected DXC SDK link: $($item.FullName)" }
            if ($item -is [IO.DirectoryInfo]) {
                $pending.Push($item)
            } else {
                $name = $item.FullName.Substring($root.FullName.Length + 1).Replace('\', '/')
                if ($name -cne $manifestName) { $paths.Add($name) }
            }
        }
    }
    $paths.Sort([StringComparer]::Ordinal)
    $lines = [Text.StringBuilder]::new()
    foreach ($name in $paths) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $directory $name) -Algorithm SHA256).Hash.ToLowerInvariant()
        [void]$lines.Append($hash).Append('  ').Append($name).Append([char]10)
    }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($lines.ToString()))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Assert-InstalledContent($installed, $manifestHash) {
    if ((Get-FileHash -LiteralPath (Join-Path $sdk $manifestName)).Hash -ne $manifestHash -or
        (Get-SdkContentSha256 $sdk) -ne $installed.NyxDxcContentSha256) {
        throw "The bundled DXC SDK contains local changes. Save them before updating: $sdk"
    }
}

function Expand-DxcFiles($archive, $directory) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName.Replace('\', '/')
            if ($name.EndsWith('/')) { continue }
            $notice = $name -match '^(?:licen[cs]es?|copying|notices?|third[-_]party[-_]notices?)(?:[._-][^/]*)?(?:/|$)'
            if ($name -notin $runtimeFiles -and $name -ne 'ReleaseNotes.md' -and !$notice) { continue }
            if ($name -match '(?:^|/)(?:\.|\.\.)(?:/|$)|:') { throw "Unexpected DXC archive path: $name" }
            # Microsoft has used both spellings for the MIT license in its SDK archives.
            if ($name -ieq 'LICENCE-MIT.txt') { $name = 'LICENSE-MIT.txt' }
            $path = [IO.Path]::GetFullPath((Join-Path $directory $name))
            if (!$path.StartsWith($directory + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Archive path leaves DXC staging directory: $name"
            }
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $path, $false)
            [IO.File]::SetLastWriteTimeUtc($path, [DateTime]::UtcNow)
        }
    } finally { $zip.Dispose() }
    Assert-SdkFiles $directory
}

function Write-Manifest($directory, $release) {
    $contentHash = Get-SdkContentSha256 $directory
    $xml = @"
<?xml version="1.0" encoding="utf-8"?>
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <PropertyGroup>
    <NyxDxcTag>$($release.Tag)</NyxDxcTag>
    <NyxDxcChannel>$($release.Channel)</NyxDxcChannel>
    <NyxDxcAsset>$($release.Asset)</NyxDxcAsset>
    <NyxDxcPublishedAt>$($release.PublishedAt)</NyxDxcPublishedAt>
    <NyxDxcSha256>$($release.Sha256)</NyxDxcSha256>
    <NyxDxcContentSha256>$contentHash</NyxDxcContentSha256>
  </PropertyGroup>
</Project>
"@
    [IO.File]::WriteAllText((Join-Path $directory $manifestName), $xml.Replace([Environment]::NewLine, [string][char]10) + [char]10)
}

function Move-SdkDirectory([string]$source, [string]$destination) {
    # A Windows scanner can briefly hold a just-extracted compiler or its parent directory.
    for ($attempt = 0; ; $attempt++) {
        try {
            [IO.Directory]::Move($source, $destination)
            return
        } catch {
            $cause = $_.Exception.GetBaseException()
            $win32 = $cause.HResult -band 0xffff
            if ($win32 -notin @(5, 32, 33) -or $attempt -ge 6) { throw }
            Start-Sleep -Milliseconds 350
        }
    }
}

function Update-Dxc {
    if ([IO.Directory]::Exists($backup) -and ![IO.Directory]::Exists($sdk)) {
        if ($CheckOnly) { throw "An interrupted DXC update left its backup at $backup. Run without -CheckOnly to restore it." }
        Move-SdkDirectory $backup $sdk
        Write-Host 'Restored the previous DXC SDK after an interrupted update.'
    }

    $manifest = Join-Path $sdk $manifestName
    $manifestHash = (Get-FileHash -LiteralPath $manifest).Hash
    $installed = ([xml][IO.File]::ReadAllText($manifest)).Project.PropertyGroup
    if ($installed.NyxDxcTag -cnotmatch $tagPattern -or $installed.NyxDxcChannel -cnotin @('release','preview') -or
        $installed.NyxDxcAsset -cnotmatch $assetPattern -or $installed.NyxDxcSha256 -notmatch '^[0-9a-fA-F]{64}$' -or
        $installed.NyxDxcContentSha256 -notmatch '^[0-9a-fA-F]{64}$' -or
        $installed.NyxDxcPublishedAt -notmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$') {
        throw "Invalid DXC SDK manifest: $manifest"
    }
    Assert-SdkFiles $sdk
    $latest = Get-ReleaseInfo
    $sameChannel = $installed.NyxDxcChannel -ceq $Channel
    if ($sameChannel -and $installed.NyxDxcTag -ceq $latest.Tag) {
        if ($installed.NyxDxcSha256 -ne $latest.Sha256 -or $installed.NyxDxcAsset -cne $latest.Asset) {
            throw "The official archive for $($latest.Tag) differs from the installed lock."
        }
        Write-Host "DXC $Channel $($latest.Tag) is up to date."
        return
    }
    if ($sameChannel -and [DateTimeOffset]$installed.NyxDxcPublishedAt -gt [DateTimeOffset]$latest.PublishedAt) {
        Write-Host "Installed DXC $Channel $($installed.NyxDxcTag) is newer than $($latest.Tag)."
        return
    }
    Write-Host "DXC $($installed.NyxDxcChannel) $($installed.NyxDxcTag) -> $Channel $($latest.Tag) ($($latest.Asset))"
    if ($CheckOnly) { return }

    if ([IO.Directory]::Exists($backup)) { throw "Inspect the previous DXC backup before updating: $backup" }
    Assert-InstalledContent $installed $manifestHash
    $work = Join-Path $parent ('.nyx-dxc-update-' + [Guid]::NewGuid().ToString('N'))
    $stage = Join-Path $work 'sdk'
    [void][IO.Directory]::CreateDirectory($stage)
    try {
        $archive = Join-Path $work $latest.Asset
        Invoke-WebRequest -UseBasicParsing -Uri $latest.Url -OutFile $archive -TimeoutSec 300
        if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $latest.Sha256) { throw "SHA-256 verification failed for $($latest.Asset)." }
        Expand-DxcFiles $archive $stage
        Write-Manifest $stage $latest
        Assert-InstalledContent $installed $manifestHash
        Move-SdkDirectory $sdk $backup
        try { Move-SdkDirectory $stage $sdk } catch {
            Move-SdkDirectory $backup $sdk
            throw
        }
        try { [IO.Directory]::Delete($backup, $true) } catch {
            Write-Warning "DXC was installed; the old SDK remains at $backup. Close any process using it before removing the backup."
        }
        Write-Host "Installed DXC $Channel $($latest.Tag)."
    } finally {
        if ([IO.Directory]::Exists($work)) { [IO.Directory]::Delete($work, $true) }
    }
}

# Scope the mutex to this checkout, including recovery between the two directory renames.
$sha = [Security.Cryptography.SHA256]::Create()
try { $key = [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($sdk.ToUpperInvariant()))).Replace('-', '') }
finally { $sha.Dispose() }
$mutex = [Threading.Mutex]::new($false, "Local\Nyx-DXC-$key")
$locked = $false
try {
    try { $locked = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (!$locked) { throw 'Another DXC update is running in this checkout.' }
    Update-Dxc
} finally {
    if ($locked) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
