#pragma once
#include "iAsrEngine.h"

// sherpa-onnx 官方 C API 头文件。
// 复制自 https://github.com/k2-fsa/sherpa-onnx v2.0.1 的 include/sherpa-onnx/c-api/c-api.h
// 放到了 third_party/sherpa-onnx/include 下。
//
// 注意：这里不直接 #include，因为本文件可能被不含该目录的工程包含。
// 真正需要 C API 类型的代码在 SenseVoiceEngine.cpp 里包含。
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

	public:
		SenseVoiceEngine();
		~SenseVoiceEngine() override;

		const sAsrEngineInfo& info() const override
		{
			return engineInfo;
		}

		HRESULT load( LPCTSTR modelPath, bool useGpu ) override;

		HRESULT run( const float* samples, size_t countSamples, const sAsrParams& params, sAsrResult& result ) override;

		bool isLoaded() const override
		{
			return nullptr != recognizer;
		}

		// 手动指定模型目录（含 model*.onnx 与 tokens.txt）
		HRESULT loadFromDirectory( LPCTSTR directory, int threads );

		// 把 Whisper 的语言 key 转成 sherpa 认识的字符串
		static const char* languageCodeFromKey( uint32_t key );
	};
}
