# 拉取 sherpa-onnx 的二进制库（dll / lib）到本目录。
# 仓库只保留头文件，二进制 22MB 不适合放进 git。
$ErrorActionPreference = "Stop"

$version = "2.0.1"
$pkg = "sherpa-onnx-v$version-win-x64-shared-MT-Release"
$url = "https://github.com/k2-fsa/sherpa-onnx/releases/download/v$version/$pkg.tar.bz2"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$libDir = Join-Path $root "lib"
$tmp = Join-Path $env:TEMP "sherpa-onnx-dl"

if (Test-Path (Join-Path $libDir "sherpa-onnx-c-api.dll")) {
    Write-Host "已存在，跳过下载。如需强制重新下载请先删除 lib\ 目录。" -ForegroundColor Yellow
    exit 0
}

New-Item -ItemType Directory -Force -Path $libDir | Out-Null
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$archive = Join-Path $tmp "sherpa.tar.bz2"

Write-Host "下载 $url"
Invoke-WebRequest -Uri $url -OutFile $archive

Write-Host "解压..."
tar -xjf $archive -C $tmp $pkg/lib

$srcLib = Join-Path $tmp (Join-Path $pkg "lib")
$need = @(
    "sherpa-onnx-c-api.dll",
    "sherpa-onnx-c-api.lib",
    "onnxruntime.dll",
    "onnxruntime_providers_shared.dll"
)
foreach ($f in $need) {
    Copy-Item (Join-Path $srcLib $f) -Destination $libDir -Force
    Write-Host "  已放置 $f"
}

Remove-Item -Recurse -Force $tmp
Write-Host "完成。现在可以构建 WhisperDesktop.vcxproj。" -ForegroundColor Green
