param (
    [Parameter(Mandatory = $true)]
    [string]$InputFile
)

# 解析输入文件路径和名字
$InputFullPath = Resolve-Path $InputFile
$InputDir = Split-Path $InputFullPath
$InputName = [System.IO.Path]::GetFileNameWithoutExtension($InputFullPath)

# 输出文件名
$OutputName = "${InputName}_bc6h.dds"
$OutputFullPath = Join-Path $InputDir $OutputName

# 创建临时输出目录
$tempDir = Join-Path $InputDir "_tmp_texconv_bc6h"
if (-Not (Test-Path $tempDir)) {
    New-Item -ItemType Directory -Path $tempDir | Out-Null
}

# 运行 texconv.exe 压缩为 BC6H，输出到临时目录
& ".\texconv.exe" `
    -f BC6H_UF16 `
    -y `
    -o $tempDir `
    "$InputFullPath"

# texconv 默认输出和输入文件同名 dds，拼接路径
$tempOutputFile = Join-Path $tempDir ("$InputName.dds")

# 检查输出文件是否生成
if (-Not (Test-Path $tempOutputFile)) {
    Write-Error "texconv 没有生成输出文件，转换失败！"
    exit 1
}

# 移动并重命名到最终目录，覆盖同名文件（但原始输入文件不动）
Move-Item -Path $tempOutputFile -Destination $OutputFullPath -Force

# 清理临时目录
Remove-Item -Path $tempDir -Recurse -Force

Write-Host "转换完成，生成文件： $OutputFullPath"
