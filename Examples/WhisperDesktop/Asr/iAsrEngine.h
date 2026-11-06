#pragma once

// 语音识别引擎的统一抽象层。
//
// 目标：把「批量转写」与「具体用哪个模型」彻底解耦。
// TranscribeDlg 只面向 iAsrEngine 编程，引擎实现自己决定跑 GPU 还是 CPU、
// 用什么模型格式。接入新模型（SenseVoice / Qwen3-ASR / ...）时，
// 只需要新增一个 iAsrEngine 实现并注册到 asrEngineRegistry，批量逻辑一行都不用改。
//
// 约定：
// - 引擎实例不是线程安全的。并发跑多个任务时，每个槽位持有各自的实例。
// - 一段音频是被 run() 一次性消费的。支持增量流的引擎在抽象层这里
//   被归一化为「一次性拿到完整结果」，这样所有引擎在批量里表现一致。

#include "stdafx.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Asr
{
	// 引擎能力标志。用位掩码而不是枚举：调度器要按「GPU 槽 / CPU 槽」分派任务。
	enum eEngineFlags : uint32_t
	{
		FlagsNone = 0,
		// 支持在 GPU 上运行（能否同时开多个 GPU 槽由 gpuMemoryPerInstanceMb 决定）
		UsesGpu = 1,
		// 支持在 CPU 上运行（可以多槽位并行）
		UsesCpu = 2,
		// 支持时间戳输出（SRT / VTT）
		SupportsTimestamps = 4,
		// 支持翻译成英文（目前只有 Whisper）
		SupportsTranslate = 8,
		// 支持按语言选择（不支持时引擎自己决定语言）
		SupportsLanguage = 16,
	};

	inline eEngineFlags operator |( eEngineFlags a, eEngineFlags b )
	{
		return (eEngineFlags)( (uint32_t)a | (uint32_t)b );
	}
	template<typename T>
	inline bool hasFlag( T v, T f )
	{
		return 0 != ( (uint32_t)v & (uint32_t)f );
	}

	// 一条转写结果片段。字段与 Whisper::sSegment 保持同构，
	// 这样 UI 层的输出代码（txt / srt / vtt）在两种引擎之间可以完全复用。
	struct sAsrSegment
	{
		// UTF-8，以 0 结尾。注意 Whisper 对中文有时会把一个字符拆到两个 token 里，
		// 拼接时必须以 segment 为单位，不要以 token 为单位重新拼接。
		std::string text;
		// 起止时间，单位是 100 纳秒 tick（与 Whisper::sTimeSpan 一致）
		uint64_t beginTicks = 0;
		uint64_t endTicks = 0;
	};

	struct sAsrResult
	{
		std::vector<sAsrSegment> segments;

		// 识别出的语言，BCP-47 风格短码（"zh" / "en" / "yue" ...），未知时为空
		std::string detectedLanguage;

		// 实际使用的引擎名，用于 UI 显示和日志
		CStringA engineName;

		void clear()
		{
			segments.clear();
			detectedLanguage.clear();
		}
		bool empty() const
		{
			return segments.empty();
		}
	};

	// 单次转写参数
	struct sAsrParams
	{
		// 语言 key，取值与 Whisper::makeLanguageKey 一致；0 表示自动/引擎决定
		uint32_t language = 0;
		bool translate = false;
		// 需要时间戳（影响引擎是否走带时间戳的解码路径）
		bool wantTimestamps = false;

		// 进度回调，0.0 ~ 1.0，在引擎的工作线程里被调用，实现方负责 PostMessage 之类的线程切换。
		// 返回 S_FALSE 表示请求取消。
		std::function<HRESULT( double )> onProgress;
		// 新增片段回调（用于实时把内容打到日志），可以为空
		std::function<HRESULT( const sAsrSegment& )> onSegment;
		// 返回 S_OK 继续，S_FALSE 取消。可以为空。
		std::function<HRESULT()> shouldContinue;
	};

	// 引擎实例实际运行的硬件
	enum struct eEngineDevice : uint8_t
	{
		Cpu = 0,
		Gpu = 1,
	};

	// 引擎能力描述，供 UI 显示和调度器分派
	struct sAsrEngineInfo
	{
		// 稳定标识，用于注册表查找和配置持久化，例如 L"whisper-gpu" / L"sherpa-sensevoice"
		CString id;
		// 显示名，例如 L"Whisper (GPU)"
		CString displayName;
		// 该实现支持哪些后端（能力），与下面 execDevice 是两回事
		eEngineFlags flags = FlagsNone;
		// 这条条目实际跑在哪块硬件上。listAvailableEngines 会为同一实现
		// 分别输出 CPU / GPU 两条（例如 SenseVoice CPU 与 SenseVoice GPU），
		// 调度器据此取「同一实现」的两个变体组成异构并行。
		eEngineDevice execDevice = eEngineDevice::Cpu;
		// 该引擎单实例建议占用的 CPU 线程数；0 表示与 CPU 无关（UsesGpu 的引擎）
		int cpuThreadsPerInstance = 0;
		// 单实例大致内存占用（MB），用于调度器估算能开几个
		uint32_t memoryPerInstanceMb = 0;
		// 单实例大致占用的显存（MB）。只有 UsesGpu 的引擎需要，0 表示未知/不适用。
		// 调度器用它按「可用显存」推算 GPU 槽位上限，而不是一律只开 1 个。
		uint32_t gpuMemoryPerInstanceMb = 0;
	};

	class iAsrEngine
	{
	public:
		virtual ~iAsrEngine() = default;

		virtual const sAsrEngineInfo& info() const = 0;

		// 加载模型。返回成功后可反复调用 run()。
		virtual HRESULT load( LPCTSTR modelPath, bool useGpu ) = 0;

		// 执行一次转写。
		// samples: 16kHz 单声道 float PCM，取值范围 [-1, 1]
		// countSamples: 采样点数量
		virtual HRESULT run( const float* samples, size_t countSamples, const sAsrParams& params, sAsrResult& result ) = 0;

		// 模型是否已就绪
		virtual bool isLoaded() const = 0;
	};
}
