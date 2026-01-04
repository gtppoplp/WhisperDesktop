# WhisperDesktop（Windows）

WhisperDesktop 是一个运行在 **Windows 64 位**上的本地语音转文字工具，  
基于 OpenAI Whisper 模型的 C/C++ 实现，可将音频、视频或麦克风输入转录为文本。

本项目适合：
- 会议/访谈录音整理
- 课程、播客、视频的文字转写
- 本地、离线的批量语音转录需求

---

## 快速开始

1. 在 **Releases** 页面下载 WhisperDesktop
2. 解压后运行 `WhisperDesktop.exe`
3. 首次使用请先下载并加载模型（推荐 `ggml-medium.bin`）

---

## 界面与功能概览

### 加载模型
下载并选择 Whisper 模型文件（模型越大，效果通常越好）

![加载模型界面](images/加载Whisper模型.png)

---

### 转录文件
批量转录本地音频或视频文件，适合整理录音资料

![转录文件界面](images/转录音频文件.png)

---

### 捕获音频
从麦克风实时捕获并转录语音，适合会议或即时记录

![捕获音频界面](images/捕获音频.png)

---

### 调试控制台
提供中文 UI 与控制台输出，便于查看运行状态与日志

![调试控制台](images/debug控制台.png)

---

## 主要特性

- 基于 Whisper 模型的本地语音识别
- 支持批量文件转录与实时麦克风转录
- Windows 原生实现，无需 Python 或额外运行时
- 支持 GPU 加速（Direct3D 11）
- 中文界面，适合日常使用

---

## 系统要求

- **操作系统**：Windows 64 位（推荐 Windows 10 / 11）
- **显卡**：支持 Direct3D 11 的 GPU
- **CPU**：支持 AVX / F16C 指令集

---

## 二次开发与改动说明

本项目基于上游 Whisper Windows 实现进行整理与二次开发，主要包括：

- 界面与交互优化，简化使用流程
- 中文界面与提示信息（部分由 AI 辅助翻译并调整）
- 使用体验与稳定性改进
- 功能取舍与精简，面向日常使用场景

> 本仓库更侧重“拿来即用”，而非底层实现细节。

---

## 免责声明

本软件按“现状（AS IS）”提供，不对识别准确率或使用结果作任何保证。  
语音识别效果会受到音质、口音、噪声、模型大小等因素影响。

---

## 上游项目与致谢

- Whisper Windows 实现（Const-me）  
  https://github.com/Const-me/Whisper
- whisper.cpp（C/C++ 实现）  
  https://github.com/ggerganov/whisper.cpp
- OpenAI Whisper 模型  
  https://github.com/openai/whisper

---

## License

本项目遵循上游项目的开源许可证要求，  
具体请查看仓库中的 `LICENSE` 文件。
