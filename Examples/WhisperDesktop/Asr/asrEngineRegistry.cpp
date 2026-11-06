#include "stdafx.h"
#include "asrEngineRegistry.h"
#include "WhisperEngine.h"
#include "SenseVoiceEngine.h"

namespace Asr
{
	namespace
	{
		constexpr LPCTSTR idWhisperGpu = L"whisper-gpu";
		constexpr LPCTSTR idSherpaSenseVoice = L"sherpa-sensevoice";
		constexpr LPCTSTR idSherpaSenseVoiceGpu = L"sherpa-sensevoice-gpu";

		// SenseVoice 的模型目录解析优先级：
		// 1) 界面上选过的目录（注册表 senseVoiceDir，从 AppState 传进来）
		// 2) 环境变量 WHISPERDESKTOP_SENSEVOICE_MODEL
		// 3) exe 同目录下的 models\sense-voice
		CString senseVoiceModelDir( const CString& configured )
		{
			if( !configured.IsEmpty() )
				return configured;

			wchar_t buf[ MAX_PATH ] = {};
			const DWORD n = ::GetEnvironmentVariableW( L"WHISPERDESKTOP_SENSEVOICE_MODEL", buf, MAX_PATH );
			if( n > 0 && n < MAX_PATH )
				return CString( buf );

			wchar_t exePath[ MAX_PATH ] = {};
			if( 0 == ::GetModuleFileNameW( nullptr, exePath, MAX_PATH ) )
				return CString();
			wchar_t* p = ::PathFindFileNameW( exePath );
			if( nullptr != p )
				*p = 0;

			wchar_t combined[ MAX_PATH ] = {};
			if( !::PathCombineW( combined, exePath, L"models\\sense-voice" ) )
				return CString();
			return CString( combined );
		}

		// 判断 sherpa-onnx 是否真的可用（DLL + 模型都在）。
		// DLL 加载结果由 SherpaApi 单例缓存；模型文件检测每次都做，
		// 这样用户把模型放进去之后不需要重启程序就能看到 SenseVoice 选项。
		bool senseVoiceAvailable( const CString& configuredDir )
		{
			if( !SherpaApi::instance().available() )
				return false;

			const CString dir = senseVoiceModelDir( configuredDir );
			if( dir.IsEmpty() )
				return false;

			wchar_t buf[ MAX_PATH ] = {};
			if( !::PathCombineW( buf, dir, L"tokens.txt" ) || !::PathFileExistsW( buf ) )
				return false;

			static const LPCTSTR models[] = { L"model.int8.onnx", L"model.onnx", L"model.fp32.onnx" };
			for( LPCTSTR name : models )
			{
				if( ::PathCombineW( buf, dir, name ) && ::PathFileExistsW( buf ) )
					return true;
			}
			return false;
		}
	}

	CString defaultEngineId()
	{
		return idWhisperGpu;
	}

	bool isGpuEngine( LPCTSTR id )
	{
		return 0 == _tcsicmp( id, idWhisperGpu ) || 0 == _tcsicmp( id, idSherpaSenseVoiceGpu );
	}

	bool isGpuEngineInfo( const std::vector<sAsrEngineInfo>& engines, LPCTSTR id )
	{
		if( nullptr == id )
			return false;
		for( const sAsrEngineInfo& e : engines )
		{
			if( 0 == e.id.CompareNoCase( id ) )
				return e.execDevice == eEngineDevice::Gpu;
		}
		return isGpuEngine( id );
	}

	CString cpuVariantId( const std::vector<sAsrEngineInfo>& engines, LPCTSTR id )
	{
		if( nullptr == id )
			return CString();
		const sAsrEngineInfo* self = nullptr;
		for( const sAsrEngineInfo& e : engines )
		{
			if( 0 == e.id.CompareNoCase( id ) )
			{
				self = &e;
				break;
			}
		}
		if( nullptr == self || self->execDevice != eEngineDevice::Gpu )
			return CString();

		// 能力集合相同、且是 CPU 变体的，就是同一个实现
		for( const sAsrEngineInfo& e : engines )
		{
			if( e.execDevice != eEngineDevice::Cpu )
				continue;
			if( (uint32_t)e.flags != (uint32_t)self->flags )
				continue;
			return e.id;
		}
		return CString();
	}

	std::unique_ptr<iAsrEngine> createEngine( LPCTSTR id, const AppContext& app )
	{
		if( nullptr == id )
			return nullptr;

		if( 0 == _tcsicmp( id, idWhisperGpu ) )
		{
			if( nullptr == app.model )
				return nullptr;
			return std::make_unique<WhisperEngine>( app.model, app.mediaFoundation );
		}

		// SenseVoice 的 CPU / GPU 是两个不同的 id，但共用同一个实现。
		// 设备由 id 决定（异构并行时 GPU 槽和 CPU 槽会同时存在），
		// 这样同一批任务里可以一边用 CUDA 一边用 CPU 核心。
		if( 0 == _tcsicmp( id, idSherpaSenseVoice ) || 0 == _tcsicmp( id, idSherpaSenseVoiceGpu ) )
		{
			auto engine = std::make_unique<SenseVoiceEngine>();
			const CString dir = senseVoiceModelDir( app.senseVoiceDir );
			const bool wantGpu = ( 0 == _tcsicmp( id, idSherpaSenseVoiceGpu ) );
			// 请求 GPU 但 CUDA 不可用时 loadFromDirectory 内部会自动回退到 CPU，
			// 引擎会把实际边界写回自己的 info()（这时它会自报为 CPU）。
			if( FAILED( engine->loadFromDirectory( dir, 0, wantGpu ? L"cuda" : L"cpu" ) ) )
				return nullptr;
			return engine;
		}

		// 后续在这里挂 Qwen3-ASR / 其它模型

		return nullptr;
	}

	std::vector<sAsrEngineInfo> listAvailableEngines( const AppContext& app )
	{
		std::vector<sAsrEngineInfo> res;

		if( nullptr != app.model )
		{
			WhisperEngine probe( app.model, app.mediaFoundation );
			res.push_back( probe.info() );
		}

		if( senseVoiceAvailable( app.senseVoiceDir ) )
		{
			// 用户设备偏好只决定默认把 SenseVoice 放哪：GPU 时把 GPU 变体排在第一位。
			// 两个变体同时列出，转写界面的「GPU+CPU」异构调度才能同时拿到两种槽位。
			const bool preferGpu = !app.senseVoiceDevice.IsEmpty() && 0 == app.senseVoiceDevice.CompareNoCase( L"gpu" );

			SenseVoiceEngine gpuProbe;
			gpuProbe.infoForDevice( true );
			// CUDA 版运行库不在时不要谎报 GPU，否则调度会白建一个跑不起来的槽
			CString reason;
			const bool gpuUsable = SenseVoiceEngine::cudaAvailable( reason );

			SenseVoiceEngine cpuProbe;
			cpuProbe.infoForDevice( false );

			if( preferGpu && gpuUsable )
			{
				res.push_back( gpuProbe.info() );
				res.push_back( cpuProbe.info() );
			}
			else
			{
				res.push_back( cpuProbe.info() );
				if( gpuUsable )
					res.push_back( gpuProbe.info() );
			}
		}

		return res;
	}
}
