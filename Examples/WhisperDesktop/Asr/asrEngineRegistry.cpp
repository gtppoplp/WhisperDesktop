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

		// SenseVoice 的模型目录：exe 同目录下的 models\sense-voice，
		// 也可以用环境变量 WHISPERDESKTOP_SENSEVOICE_MODEL 覆盖。
		CString senseVoiceModelDir()
		{
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
		// 第一次调用会真正 LoadLibrary，结果缓存起来，避免 UI 反复填充下拉框时重复加载。
		bool senseVoiceAvailable()
		{
			static const bool cached = []
			{
				if( !SherpaApi::instance().available() )
					return false;

				const CString dir = senseVoiceModelDir();
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
			}();
			return cached;
		}
	}

	CString defaultEngineId()
	{
		return idWhisperGpu;
	}

	bool isGpuEngine( LPCTSTR id )
	{
		return 0 == _tcsicmp( id, idWhisperGpu );
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

		if( 0 == _tcsicmp( id, idSherpaSenseVoice ) )
		{
			auto engine = std::make_unique<SenseVoiceEngine>();
			const CString dir = senseVoiceModelDir();
			if( FAILED( engine->loadFromDirectory( dir, 0 ) ) )
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

		if( senseVoiceAvailable() )
		{
			SenseVoiceEngine probe;
			res.push_back( probe.info() );
		}

		return res;
	}
}
