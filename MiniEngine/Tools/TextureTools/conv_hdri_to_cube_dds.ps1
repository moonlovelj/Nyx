param (
    [Parameter(Mandatory = $true)]
    [string]$InputFile
)

# Parse input file path and name
$InputFullPath = Resolve-Path $InputFile
$InputDir = Split-Path $InputFullPath
$InputName = [System.IO.Path]::GetFileNameWithoutExtension($InputFullPath)

# Output file name
$OutputName = "${InputName}_bc6h.dds"
$OutputFullPath = Join-Path $InputDir $OutputName

# Create temporary output directory
$tempDir = Join-Path $InputDir "_tmp_texconv_bc6h"
if (-Not (Test-Path $tempDir)) {
    New-Item -ItemType Directory -Path $tempDir | Out-Null
}

# Run texconv.exe to compress to BC6H, output to temp directory
& ".\texconv.exe" `
    -f BC6H_UF16 `
    -y `
    -o $tempDir `
    "$InputFullPath"

# texconv outputs a DDS with the same name as input by default; build the path
$tempOutputFile = Join-Path $tempDir ("$InputName.dds")

# Check whether output file was generated
if (-Not (Test-Path $tempOutputFile)) {
    Write-Error "texconv 没有生成输出文件，转换失败！"
    exit 1
}

# Move and rename to final directory, overwrite same-name file (leave original input untouched)
Move-Item -Path $tempOutputFile -Destination $OutputFullPath -Force

# Clean up temporary directory
Remove-Item -Path $tempDir -Recurse -Force

Write-Host "转换完成，生成文件： $OutputFullPath"
