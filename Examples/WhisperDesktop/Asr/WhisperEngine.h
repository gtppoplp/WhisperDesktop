#pragma once
#include "iAsrEngine.h"

namespace Whisper
{
	struct iModel;
	struct iMediaFoundation;
}

namespace Asr
{
	// 把现有的 Whisper GPU 引擎包装成 iAsrEngine。
	//
	// 这里刻意不碰 Whisper 内核：模型由 AppState 统一加载并共享（iModel 本身是只读、
	// 多线程可共享的），每个引擎实例只持有一份轻量的 iContext。
	// 因此多个槽位可以在同一个进程里并行跑，只要它们的 iContext 各自独立。
	//
	// 转写走 runStreamed：(音频文件 -> Media Foundation 增量解码 -> mel -> encoder -> decoder)，
	// 这条路径在改动前就是 TranscribeDlg 使用的路径，行为完全不变。
	class WhisperEngine : public iAsrEngine
	{
		// 共享的只读模型，不持有引用计数（生命周期由 AppState 保证）
		Whisper::iModel* model = nullptr;
		// Media Foundation 封装，用于解码音频/视频文件
		Whisper::iMediaFoundation* mediaFoundation = nullptr;

		// 该实例自己的推理上下文，懒创建
		CComPtr<Whisper::iContext> context;

		sAsrEngineInfo engineInfo;

		// 取消标志，由回调读取
		volatile bool cancelRequested = false;

		const sAsrParams* activeParams = nullptr;
		sAsrResult* activeResult = nullptr;

		HRESULT ensureContext();
		void collectSegments( Whisper::iContext* ctx );

		static HRESULT __cdecl newSegmentCallbackStatic( Whisper::iContext* ctx, uint32_t n_new, void* user_data ) noexcept;
		static HRESULT __cdecl encoderBeginCallbackStatic( Whisper::iContext* ctx, void* user_data ) noexcept;
		static HRESULT __cdecl progressCallbackStatic( double p, Whisper::iContext* ctx, void* user_data ) noexcept;

	public:
		// modelInstance / mfInstance 必须由调用方保证存活（通常是 AppState::model / mediaFoundation）
		WhisperEngine( Whisper::iModel* modelInstance, Whisper::iMediaFoundation* mfInstance );

		const sAsrEngineInfo& info() const override
		{
			return engineInfo;
		}

		HRESULT load( LPCTSTR modelPath, bool useGpu ) override;

		HRESULT run( const float* samples, size_t countSamples, const sAsrParams& params, sAsrResult& result ) override;

		// 直接从媒体文件转写（内部用 Media Foundation 增量解码，无需把整段 PCM 读进内存）
		// 这是批量转写的主力入口：音频和视频容器都由 Media Foundation 处理。
		HRESULT transcribeFile( LPCTSTR mediaPath, const sAsrParams& params, sAsrResult& result );

		bool isLoaded() const override
		{
			return nullptr != model && nullptr != context;
		}
	};
}
