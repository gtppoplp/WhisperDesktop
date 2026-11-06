# 拉取 sherpa-onnx 的二进制库（dll / lib）到本目录。
# 仓库只保留头文件，二进制 22MB 不适合放进 git。
#
# 用法：
#   .\fetch.ps1              拉 CPU 版（约 22MB）
#   .\fetch.ps1 -Cuda        拉 CUDA 版（约 458MB，含 onnxruntime_providers_cuda.dll）
#                            该包基于 cuda 13.x / cudnn 9.x / onnxruntime 1.28.2
#   加 -Force 可强制重新下载（已存在时默认跳过）
# 注意：CPU 版与 CUDA 版都解压到同一个 lib\ 目录，二者只需其一（CUDA 包已含 CPU 所需 DLL）。
param(
    [switch]$Cuda,
    [switch]$Force
)
$ErrorActionPreference = "Stop"

$version = "2.0.1"
if ($Cuda) {
    # CUDA 版：本体 + onnxruntime + cudnn，解压后整包覆盖到 lib\
    $pkg = "sherpa-onnx-v$version-cuda-13.x-cudnn-9.x-onnxruntime1.28.2-win-x64-cuda"
} else {
    $pkg = "sherpa-onnx-v$version-win-x64-shared-MT-Release"
}
$url = "https://github.com/k2-fsa/sherpa-onnx/releases/download/v$version/$pkg.tar.bz2"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$libDir = Join-Path $root "lib"
$tmp = Join-Path $env:TEMP "sherpa-onnx-dl"

if ((Test-Path (Join-Path $libDir "sherpa-onnx-c-api.dll")) -and -not $Force) {
    Write-Host "已存在，跳过下载。要重新下载请加 -Force。" -ForegroundColor Yellow
    exit 0
}

New-Item -ItemType Directory -Force -Path $libDir | Out-Null
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$archive = Join-Path $tmp "sherpa.tar.bz2"

Write-Host "下载 $pkg.zip ($([math]::Round((Invoke-WebRequest -Uri $url -Method Head).Headers.'Content-Length'/1MB))MB)..."
Invoke-WebRequest -Uri $url -OutFile $archive

Write-Host "解压..."
tar -xjf $archive -C $tmp $pkg/lib

$srcLib = Join-Path $tmp (Join-Path $pkg "lib")
if ($Cuda) {
    # CUDA 包里依赖较多（cudart / cudnn / provider），整目录复制
    Copy-Item (Join-Path $srcLib "*") -Destination $libDir -Force -Recurse
    Write-Host "  已复制 CUDA 版全部运行库"
} else {
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
}

Remove-Item -Recurse -Force $tmp
if ($Cuda) {
    Write-Host "完成。CUDA 版运行库已就位，加载模型界面选 GPU (CUDA) 即可。" -ForegroundColor Green
} else {
    Write-Host "完成。现在可以构建 WhisperDesktop.vcxproj。" -ForegroundColor Green
}
