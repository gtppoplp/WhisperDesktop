#pragma once
#include "iAsrEngine.h"

// sherpa-onnx 官方 C API 头文件。
// 复制自 https://github.com/k2-fsa/sherpa-onnx v2.0.1 的 include/sherpa-onnx/c-api/c-api.h
// 放到了 third_party/sherpa-onnx/include 下。
//
// 这里需要 SherpaOnnxOfflineRecognizerConfig 的完整定义（buildConfig 的签名用到），
// 而该结构体按值传递，不能用前置声明代替。
#include <sherpa-onnx/c-api/c-api.h>
namespace Asr
{
	// sherpa-onnx 的最小动态绑定。
	//
	// 用 LoadLibrary/GetProcAddress 而不是链接导入库，原因：
	// 1) 没装 sherpa-onnx 的用户也能正常使用 Whisper，程序不会因为缺 DLL 起不来；
	// 2) 可以在运行期给出「缺哪个 DLL」的明确提示，而不是启动即崩溃；
	// 3) 允许用户自己替换 sherpa-onnx 版本而不需要重新编译。
	class SherpaApi
	{
	public:
		// 单例：整个进程只加载一次 DLL
		static SherpaApi& instance();

		// DLL 是否加载成功
		bool available() const
		{
			return nullptr != createOfflineRecognizer;
		}

		// 加载失败的原因，给 UI / 日志显示用
		const CString& lastError() const
		{
			return loadError;
		}

		// 函数指针类型与官方 c-api.h 的签名一一对应。
		// 返回类型用 const void* 代替具体的结构体指针，调用处再 static_cast。
		using pfnCreateRecognizer = const void* ( * )( const void* config );
		using pfnDestroyRecognizer = void ( * )( const void* recognizer );
		using pfnCreateStream = const void* ( * )( const void* recognizer );
		using pfnDestroyStream = void ( * )( const void* stream );
		using pfnAcceptWaveform = void ( * )( const void* stream, int32_t sampleRate, const float* samples, int32_t n );
		using pfnDecodeStream = void ( * )( const void* recognizer, const void* stream );
		using pfnGetResult = const void* ( * )( const void* stream );
		using pfnDestroyResult = void ( * )( const void* result );

		pfnCreateRecognizer createOfflineRecognizer = nullptr;
		pfnDestroyRecognizer destroyOfflineRecognizer = nullptr;
		pfnCreateStream createOfflineStream = nullptr;
		pfnDestroyStream destroyOfflineStream = nullptr;
		pfnAcceptWaveform acceptWaveformOffline = nullptr;
		pfnDecodeStream decodeOfflineStream = nullptr;
		pfnGetResult getOfflineStreamResult = nullptr;
		pfnDestroyResult destroyOfflineRecognizerResult = nullptr;

	private:
		SherpaApi();
		CString loadError;
	};

	// SenseVoice 引擎：跑在 CPU 上，支持中/英/日/韩/粤。
	//
	// 模型目录约定（默认相对 exe 所在目录）：
	//   <dir>/model.int8.onnx   或  <dir>/model.onnx
	//   <dir>/tokens.txt
	// 也可以用环境变量 WHISPERDESKTOP_SENSEVOICE_MODEL 指定绝对目录。
	class SenseVoiceEngine : public iAsrEngine
	{
		sAsrEngineInfo engineInfo;
		const void* recognizer = nullptr;

		// 模型文件路径
		CString modelPath;
		CString tokensPath;

		// 把 sherpa 输出的连续文本切成可读片段。
		// SenseVoice 是非自回归模型，官方 C API 通常不给逐句时间戳，
		// 这里按标点和长度切分，时间轴按字符数等比估算。
		static void splitIntoCaptions( const std::string& text, uint64_t totalTicks, std::vector<sAsrSegment>& out );

		// 按当前加载参数再造一个独立识别器（分块并行时每个线程一个）。
		// 返回 nullptr 表示失败。调用方用 SherpaApi::destroyOfflineRecognizer 释放。
		const void* createExtraRecognizer() const;

		// 构造识别器配置。加载时和分块并行时共用，避免两处参数不一致。
		// 注意 SherpaOnnxOfflineRecognizerConfig 定义在全局命名空间（C API 头文件）。
		bool buildConfig( ::SherpaOnnxOfflineRecognizerConfig& config,
			const CStringA& modelA, const CStringA& tokensA, int threads, const char* providerName ) const;

		// 分块并行时每个线程使用的识别器个数上限（内部再按 CPU 核数收敛）
		int chunkParallelism = 1;

		// 加载时记录的实际参数，供 createExtraRecognizer 复现
		CStringA loadedModelA;
		CStringA loadedTokensA;
		int loadedThreads = 1;

	public:
		SenseVoiceEngine();
		~SenseVoiceEngine() override;

		const sAsrEngineInfo& info() const override
		{
			return engineInfo;
		}

		// 按目标设备设置显示名与能力标志（加载模型界面 / 引擎列表用）。
		// 只是 UI 层的描述，真正能否跑 GPU 由 cudaAvailable() 和识别器创建结果决定。
		//
		// 注意：UsesGpu / UsesCpu 描述的是「这份实例实际在哪跑」，两者都必须设置——
		// 同一条目的 execDevice 才说明它跑在哪块硬件上。调度器用 execDevice 判断
		// 同一个引擎能否既提供 GPU 槽又提供 CPU 槽（异构并行）。之前这里把两个标志
		// 当成互斥，导致 SenseVoice 选 GPU 时整台机器的 CPU 槽就全没了。
		void infoForDevice( bool gpu )
		{
			if( gpu )
			{
				engineInfo.id = L"sherpa-sensevoice-gpu";
				engineInfo.displayName = L"SenseVoice (GPU)";
				engineInfo.flags = UsesGpu | UsesCpu | SupportsTimestamps | SupportsLanguage;
				engineInfo.execDevice = eEngineDevice::Gpu;
				engineInfo.cpuThreadsPerInstance = 0;
				engineInfo.memoryPerInstanceMb = 256;
				// int8 模型权重 + ORT CUDA arena 经验值；偏保守，宁可少开一个也别 OOM
				engineInfo.gpuMemoryPerInstanceMb = 900;
			}
			else
			{
				engineInfo.id = L"sherpa-sensevoice";
				engineInfo.displayName = L"SenseVoice (CPU)";
				engineInfo.flags = UsesGpu | UsesCpu | SupportsTimestamps | SupportsLanguage;
				engineInfo.execDevice = eEngineDevice::Cpu;
				engineInfo.cpuThreadsPerInstance = 1;
				engineInfo.memoryPerInstanceMb = 320;
				engineInfo.gpuMemoryPerInstanceMb = 0;
			}
		}

		HRESULT load( LPCTSTR modelPath, bool useGpu ) override;

		HRESULT run( const float* samples, size_t countSamples, const sAsrParams& params, sAsrResult& result ) override;

		bool isLoaded() const override
		{
			return nullptr != recognizer;
		}

		// 手动指定模型目录（含 model*.onnx 与 tokens.txt）
		// provider: "cpu" / "cuda"；传 nullptr 用 "cpu"
		HRESULT loadFromDirectory( LPCTSTR directory, int threads, LPCTSTR provider = nullptr );

		// 实际生效的执行后端，加载后用这个判断运行在 CPU 还是 GPU 上
		CString activeProvider;
		// 请求 GPU 但回退到 CPU 时的原因，用于向用户解释
		CString fallbackReason;

		// 检测 CUDA 是否真的可用（DLL 文件齐全 + 显卡枚举得到）
		static bool cudaAvailable( CString& reason );

		// 用一小段静音真实跑一次推理，确认 CUDA 路径真的能出结果。
		// sherpa 在 CUDA provider 注册失败时也会返回非空识别器，随后静默用 CPU，
		// 所以这个探测是「真的跑在 GPU 上」的唯一可靠证据。
		static bool probeCudaInference( const void* rec, SherpaApi& api, CString& error );

		// 把 Whisper 的语言 key 转成 sherpa 认识的字符串
		static const char* languageCodeFromKey( uint32_t key );
	};
}
