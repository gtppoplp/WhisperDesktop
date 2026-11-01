# third_party/sherpa-onnx

SenseVoice 引擎（CPU 侧）依赖 [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) 的 C API。

## 仓库里保留了什么

```
third_party/sherpa-onnx/
└── include/sherpa-onnx/c-api/*.h    # 编译期需要的头文件，约 248KB
```

## 需要你手动补充什么

`.dll` 和 `.lib` 有 22MB，不放进 git。首次构建前请执行：

```bash
./third_party/sherpa-onnx/fetch.ps1
```

或者手动下载后放到 `third_party/sherpa-onnx/lib/`：

```
sherpa-onnx-c-api.dll
sherpa-onnx-c-api.lib
onnxruntime.dll
onnxruntime_providers_shared.dll
```

## 为什么选这个版本

`sherpa-onnx-v2.0.1-win-x64-shared-MT-Release`

- **v2.0.1**：与仓库里的 `c-api.h` 一致。**不要只换 DLL 不换头文件** ——
  `SherpaOnnxOfflineModelConfig` / `SherpaOnnxOfflineRecognizerConfig` 的字段
  随版本增删，结构体错位会造成越界读写。
- **MT**：本工程用 `RuntimeLibrary=MultiThreaded`（静态 CRT），
  MT 版可以直接链接；若换成 MD 版，需要同时改工程的运行时库设置。
- **shared**：用 DLL 而不是静态库，这样缺库时程序还能启动（引擎在运行期
  `LoadLibrary` 探测，找不到就只在 UI 里隐藏 SenseVoice 选项）。

## 模型下载

```
https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17.tar.bz2
```

解压到 `<exe 目录>/models/sense-voice/`，需要其中两个文件：

```
model.int8.onnx    # 229MB，代码优先使用
tokens.txt
```

`model.onnx`（fp32，938MB）可选，代码会回退使用它。

也可以用环境变量 `WHISPERDESKTOP_SENSEVOICE_MODEL` 指定模型目录的绝对路径。
