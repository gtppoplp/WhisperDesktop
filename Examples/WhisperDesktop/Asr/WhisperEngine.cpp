#include "stdafx.h"
#include "WhisperEngine.h"
#include "Utils/logger.h"

using namespace Whisper;

// ComLightLib 的 CHECK 宏：失败即返回 HRESULT
#define CHECK( hr ) { const HRESULT __hr = ( hr ); if( FAILED( __hr ) ) return __hr; }

namespace Asr
{
	WhisperEngine::WhisperEngine( iModel* modelInstance, iMediaFoundation* mfInstance ) :
		model( modelInstance ),
		mediaFoundation( mfInstance )
	{
		engineInfo.id = L"whisper-gpu";
		engineInfo.displayName = L"Whisper (GPU)";
		engineInfo.flags = UsesGpu | SupportsTimestamps | SupportsTranslate | SupportsLanguage;
		engineInfo.execDevice = eEngineDevice::Gpu;
		// GPU 引擎不使用「每实例 CPU 线程」这个概念
		engineInfo.cpuThreadsPerInstance = 0;
		// 权重在 AppState 里共享、常驻显存；每次推理的临时缓冲约几十 MB，
		// 但调度器只看系统内存，这里填的是 CPU 侧临时占用。
		engineInfo.memoryPerInstanceMb = 128;
		// Whisper 的 GPU 槽固定 1 个：小模型在单卡上已经能吃满算力，
		// 再开一个只会两份 K/V cache 互相挤显存带宽。留 0 让调度器退回 1。
		engineInfo.gpuMemoryPerInstanceMb = 0;
	}

	HRESULT WhisperEngine::load( LPCTSTR modelPath, bool useGpu )
	{
		// 模型由「加载模型」界面统一加载并共享，这里只建立本实例自己的推理上下文。
		// 保留 modelPath / useGpu 参数是为了满足 iAsrEngine 的通用接口。
		UNREFERENCED_PARAMETER( modelPath );
		UNREFERENCED_PARAMETER( useGpu );

		if( nullptr == model )
		{
			logError( u8"Whisper 引擎未绑定模型，请先在主界面加载模型" );
			return E_UNEXPECTED;
		}
		return ensureContext();
	}

	HRESULT WhisperEngine::ensureContext()
	{
		if( nullptr != context )
			return S_OK;
		if( nullptr == model )
			return E_UNEXPECTED;

		CComPtr<iContext> ctx;
		CHECK( model->createContext( &ctx ) );
		context = ctx;
		return S_OK;
	}

	HRESULT __cdecl WhisperEngine::newSegmentCallbackStatic( iContext* ctx, uint32_t, void* user_data ) noexcept
	{
		WhisperEngine* self = static_cast<WhisperEngine*>( user_data );
		if( nullptr != self )
			self->collectSegments( ctx );
		return S_OK;
	}

	HRESULT __cdecl WhisperEngine::encoderBeginCallbackStatic( iContext*, void* user_data ) noexcept
	{
		// 返回 S_FALSE 会让引擎优雅地提前结束，并让 runStreamed 返回 S_OK。
		// 这和改动前 TranscribeDlg::encoderBeginCallback 的取消行为一致。
		WhisperEngine* self = static_cast<WhisperEngine*>( user_data );
		if( nullptr != self && self->cancelRequested )
			return S_FALSE;
		return S_OK;
	}

	HRESULT __cdecl WhisperEngine::progressCallbackStatic( double p, iContext*, void* user_data ) noexcept
	{
		WhisperEngine* self = static_cast<WhisperEngine*>( user_data );
		if( nullptr == self )
			return S_OK;

		if( self->cancelRequested )
			return S_FALSE;

		if( nullptr != self->activeParams && self->activeParams->onProgress )
		{
			if( S_OK != self->activeParams->onProgress( p ) )
			{
				self->cancelRequested = true;
				return S_FALSE;
			}
		}
		return S_OK;
	}

	void WhisperEngine::collectSegments( iContext* ctx )
	{
		if( nullptr == activeParams || nullptr == activeResult )
			return;

		CComPtr<iTranscribeResult> res;
		if( FAILED( ctx->getResults( eResultFlags::Timestamps | eResultFlags::Tokens, &res ) ) )
			return;
		if( nullptr == res )
			return;

		sTranscribeLength len{};
		if( FAILED( res->getSize( len ) ) )
			return;

		const sSegment* const segs = res->getSegments();
		const size_t total = len.countSegments;

		// 只追加新出现的片段，已经报告过的不重复报告
		const size_t already = activeResult->segments.size();
		for( size_t i = already; i < total; i++ )
		{
			const sSegment& seg = segs[ i ];
			sAsrSegment out;
			out.text = ( nullptr != seg.text ) ? seg.text : "";
			out.beginTicks = seg.time.begin.ticks;
			out.endTicks = seg.time.end.ticks;

			activeResult->segments.push_back( out );

			// 实时日志用
			if( activeParams->onSegment )
				activeParams->onSegment( out );
		}
	}

	HRESULT WhisperEngine::run( const float* samples, size_t countSamples, const sAsrParams& params, sAsrResult& result )
	{
		// 直接把内存里的 PCM 喂给引擎需要构造 iAudioBuffer，而当前媒体层的
		// loadAudioFile 只能从文件加载。批量场景一律走 transcribeFile，这里只做明确报错。
		UNREFERENCED_PARAMETER( samples );
		UNREFERENCED_PARAMETER( countSamples );
		UNREFERENCED_PARAMETER( params );
		UNREFERENCED_PARAMETER( result );
		logError( u8"WhisperEngine::run 未实现，请使用 transcribeFile" );
		return E_NOTIMPL;
	}

	HRESULT WhisperEngine::transcribeFile( LPCTSTR mediaPath, const sAsrParams& params, sAsrResult& result )
	{
		result.clear();
		result.engineName = "Whisper";

		if( nullptr == mediaPath || 0 == mediaPath[ 0 ] )
			return E_INVALIDARG;
		if( nullptr == mediaFoundation )
			return E_UNEXPECTED;

		CHECK( ensureContext() );

		cancelRequested = false;
		activeParams = &params;
		activeResult = &result;

		// 保证即使中途 return 也不会留下悬垂指针
		struct Scope
		{
			WhisperEngine& self;
			~Scope()
			{
				self.activeParams = nullptr;
				self.activeResult = nullptr;
			}
		} scope{ *this };

		CComPtr<iAudioReader> reader;
		CHECK( mediaFoundation->openAudioFile( mediaPath, false, &reader ) );

		// 以下参数设置与改动前的 TranscribeDlg::transcribe() 逐项对应
		sFullParams fullParams;
		CHECK( context->fullDefaultParams( eSamplingStrategy::Greedy, &fullParams ) );
		fullParams.language = params.language;
		fullParams.setFlag( eFullParamsFlags::Translate, params.translate );
		fullParams.resetFlag( eFullParamsFlags::PrintRealtime );

		fullParams.new_segment_callback = &newSegmentCallbackStatic;
		fullParams.new_segment_callback_user_data = this;
		fullParams.encoder_begin_callback = &encoderBeginCallbackStatic;
		fullParams.encoder_begin_callback_user_data = this;

		sProgressSink progressSink{ &progressCallbackStatic, this };

		HRESULT hr = context->runStreamed( fullParams, progressSink, reader );

		// runStreamed 在取消时也返回 S_OK，这里补一次收尾，把最后一批片段收进来
		collectSegments( context );

		return hr;
	}
}
