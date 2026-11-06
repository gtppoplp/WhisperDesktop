#include "stdafx.h"
#include "SenseVoiceEngine.h"
#include "Utils/logger.h"

// 官方 C API 头文件，路径在 third_party/sherpa-onnx/include 下。
// 这个目录已经加进 WhisperDesktop.vcxproj 的 AdditionalIncludeDirectories。
#include <sherpa-onnx/c-api/c-api.h>

#include <atomic>
#include <thread>

namespace
{
	// 100 纳秒 tick / 秒
	constexpr uint64_t ticksPerSecond = 10'000'000;

	// 判断 UTF-8 字符的字节长度（只看首字节）
	inline int utf8CharLength( unsigned char c )
	{
		if( c < 0x80 )
			return 1;
		if( ( c & 0xE0 ) == 0xC0 )
			return 2;
		if( ( c & 0xF0 ) == 0xE0 )
			return 3;
		if( ( c & 0xF8 ) == 0xF0 )
			return 4;
		return 1;
	}

	// 是否是句子结束标点（支持中英文）
	inline bool isSentenceEnd( const char* p, int len )
	{
		if( len == 3 )
		{
			const unsigned char b0 = (unsigned char)p[ 0 ];
			const unsigned char b1 = (unsigned char)p[ 1 ];
			const unsigned char b2 = (unsigned char)p[ 2 ];
			// 。 = E3 80 82
			if( b0 == 0xE3 && b1 == 0x80 && b2 == 0x82 )
				return true;
			// ！ = EF BC 81, ？ = EF BC 9F, ； = EF BC 9B
			if( b0 == 0xEF && b1 == 0xBC && ( b2 == 0x81 || b2 == 0x9F || b2 == 0x9B ) )
				return true;
			return false;
		}
		if( len == 1 )
		{
			const char c = p[ 0 ];
			return c == '.' || c == '!' || c == '?' || c == ';';
		}
		return false;
	}

	// 从 exe 所在目录出发拼接子路径
	CString pathFromExeDirectory( LPCTSTR relative )
	{
		wchar_t exePath[ MAX_PATH ] = {};
		if( 0 == ::GetModuleFileNameW( nullptr, exePath, MAX_PATH ) )
			return CString();
		wchar_t* p = ::PathFindFileNameW( exePath );
		if( nullptr != p )
			*p = 0;

		wchar_t combined[ MAX_PATH ] = {};
		if( !::PathCombineW( combined, exePath, relative ) )
			return CString();
		return CString( combined );
	}

	// 在目录里找第一个存在的文件
	CString findFirstExisting( LPCTSTR directory, const LPCTSTR* names, size_t count )
	{
		wchar_t buf[ MAX_PATH ] = {};
		for( size_t i = 0; i < count; i++ )
		{
			if( ::PathCombineW( buf, directory, names[ i ] ) && ::PathFileExistsW( buf ) )
				return CString( buf );
		}
		return CString();
	}

	// 显式加载 exe 同目录的 onnxruntime.dll。
	//
	// 原因：onnxruntime.dll 也存在于 C:\Windows\System32（系统自带 1.17），
	// 而 Windows 默认搜索顺序里 System32 优先于「应用程序目录」后面的部分，
	// 于是 CUDA 版 sherpa（要求 API 28 / ORT 1.28）会去调用 1.17 的旧 API 并失败。
	// 这里用绝对路径 + LOAD_WITH_ALTERED_SEARCH_PATH 先把它钉死，
	// 返回的 HMODULE 故意不释放：进程生命周期内保持已加载状态。
	HMODULE loadOrtFromExeDir()
	{
		static HMODULE cached = nullptr;
		if( nullptr != cached )
			return cached;

		const CString path = pathFromExeDirectory( L"onnxruntime.dll" );
		if( path.IsEmpty() )
			return nullptr;

		cached = ::LoadLibraryExW( path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH );
		if( nullptr != cached )
			logInfo( u8"已加载 onnxruntime.dll：exe 同目录版本" );
		return cached;
	}

	// 把 exe 目录和 CUDA Toolkit 的 bin 目录补进本进程的 PATH。
	//
	// CUDA provider 在 DllMain 里要加载 cublasLt64_13.dll / cublas64_13.dll，
	// 这些在 CUDA Toolkit 的 bin 下。用户从快捷方式启动时系统 PATH 里通常有，
	// 但从某些宿主进程（IDE、终端、服务）启动时 PATH 可能被裁剪，
	// 结果就是「明明装了 CUDA 却报不可用」。这里自己补齐，不依赖外部环境。
	void ensureCudaRuntimeOnPath()
	{
		static bool done = false;
		if( done )
			return;
		done = true;

		std::vector<CString> dirs;
		const CString exeDir = pathFromExeDirectory( L"" );
		if( !exeDir.IsEmpty() )
			dirs.push_back( exeDir );

		// CUDA Toolkit 版本目录名不固定，优先用环境变量，再扫几个常见版本
		static const LPCTSTR envNames[] = { L"CUDA_PATH", L"CUDA_PATH_V13_3", L"CUDA_PATH_V13_2", L"CUDA_PATH_V13_1", L"CUDA_PATH_V13_0" };
		for( LPCTSTR name : envNames )
		{
			wchar_t buf[ MAX_PATH ] = {};
			if( ::GetEnvironmentVariableW( name, buf, MAX_PATH ) > 0 )
			{
				wchar_t sub[ MAX_PATH ] = {};
				if( ::PathCombineW( sub, buf, L"bin\\x64" ) )
					dirs.push_back( CString( sub ) );
				if( ::PathCombineW( sub, buf, L"bin" ) )
					dirs.push_back( CString( sub ) );
			}
		}

		wchar_t pathBuf[ 32767 ] = {};
		const DWORD n = ::GetEnvironmentVariableW( L"PATH", pathBuf, 32767 );
		CString path = ( n > 0 ) ? CString( pathBuf ) : CString();

		bool changed = false;
		for( const CString& d : dirs )
		{
			if( d.IsEmpty() || !::PathIsDirectoryW( d ) )
				continue;
			CString probe;
			probe.Format( L";%s;", (LPCWSTR)d );
			CString hay = L";";
			hay += path;
			hay += L";";
			if( hay.Find( probe ) >= 0 )
				continue;
			if( path.IsEmpty() || path.Right( 1 ) == L";" )
				path = d + L";" + path;
			else
				path = d + L";" + path;
			changed = true;
		}

		if( changed )
			::SetEnvironmentVariableW( L"PATH", path );
	}
}

namespace Asr
{
	// ===============================================================
	// SherpaApi
	// ===============================================================

	SherpaApi& SherpaApi::instance()
	{
		static SherpaApi api;
		return api;
	}

	SherpaApi::SherpaApi()
	{
		// sherpa-onnx-c-api.dll 依赖 onnxruntime.dll，两个都必须在 exe 同目录或 PATH 上。
		//
		// 关键：必须先把 exe 同目录的 onnxruntime.dll 显式加载进来。
		// 否则 Windows 的 DLL 搜索顺序会先命中 C:\Windows\System32\onnxruntime.dll
		// （系统自带 1.17），而 CUDA 版 sherpa 需要 ONNX Runtime 1.28 的 API 28，
		// 加载后启动推理会直接报 "requested API version [28] is not available"。
		loadOrtFromExeDir();
		ensureCudaRuntimeOnPath();

		HMODULE dll = ::LoadLibraryW( L"sherpa-onnx-c-api.dll" );
		if( nullptr == dll )
		{
			const DWORD err = ::GetLastError();
			if( err == ERROR_MOD_NOT_FOUND )
				loadError = L"未找到 sherpa-onnx-c-api.dll（或它依赖的 onnxruntime.dll），SenseVoice 引擎不可用。";
			else
			{
				CString tmp;
				tmp.Format( L"加载 sherpa-onnx-c-api.dll 失败，错误码 %lu。", (unsigned long)err );
				loadError = tmp;
			}
			return;
		}

		auto resolve = [ dll ]( const char* name ) -> FARPROC
		{
			return ::GetProcAddress( dll, name );
		};

		createOfflineRecognizer = (pfnCreateRecognizer)resolve( "SherpaOnnxCreateOfflineRecognizer" );
		destroyOfflineRecognizer = (pfnDestroyRecognizer)resolve( "SherpaOnnxDestroyOfflineRecognizer" );
		createOfflineStream = (pfnCreateStream)resolve( "SherpaOnnxCreateOfflineStream" );
		destroyOfflineStream = (pfnDestroyStream)resolve( "SherpaOnnxDestroyOfflineStream" );
		acceptWaveformOffline = (pfnAcceptWaveform)resolve( "SherpaOnnxAcceptWaveformOffline" );
		decodeOfflineStream = (pfnDecodeStream)resolve( "SherpaOnnxDecodeOfflineStream" );
		getOfflineStreamResult = (pfnGetResult)resolve( "SherpaOnnxGetOfflineStreamResult" );
		destroyOfflineRecognizerResult = (pfnDestroyResult)resolve( "SherpaOnnxDestroyOfflineRecognizerResult" );

		if( nullptr == createOfflineRecognizer || nullptr == destroyOfflineRecognizer ||
			nullptr == createOfflineStream || nullptr == destroyOfflineStream ||
			nullptr == acceptWaveformOffline || nullptr == decodeOfflineStream ||
			nullptr == getOfflineStreamResult || nullptr == destroyOfflineRecognizerResult )
		{
			loadError = L"sherpa-onnx-c-api.dll 版本不匹配，缺少必要的导出函数。";
			createOfflineRecognizer = nullptr;
			return;
		}

		// 成功加载后故意不释放：与进程同生命周期，避免其它模块再用时句柄失效
	}

	// 分块计划里的一段：记录采样范围与（并行结束后回填的）识别文本。
	// 只占用索引，不复制 PCM 数据——采样指针由调用方传入，各线程只读。
	struct Chunk
	{
		size_t beginSample = 0;
		size_t endSample = 0;
		std::string text;
	};

	// 把 countSamples 切成固定长度的块。块之间不留重叠：
	// 重叠会让相邻两段各识别一遍同样的音频，拼出来的文本重复，
	// 而去重又需要文本对齐，代价比收益高。切点偶尔落在词中间的影响，
	// 远小于重复整句。需要精确边界的场景请用 Whisper。
	bool planChunks( size_t countSamples, size_t chunkSamples, std::vector<Chunk>& out )
	{
		out.clear();
		if( 0 == countSamples || 0 == chunkSamples )
			return false;

		size_t offset = 0;
		while( offset < countSamples )
		{
			Chunk c;
			c.beginSample = offset;
			size_t end = offset + chunkSamples;
			if( end > countSamples )
				end = countSamples;
			c.endSample = end;
			out.push_back( std::move( c ) );
			offset = end;
		}
		return !out.empty();
	}

	// 实际使用的线程数：不超过块数（多出来的线程会空转），也不超过配置的并行度。
	int scheduleChunkThreads( size_t chunkCount, int maxThreads )
	{
		if( chunkCount < 2 )
			return 1;
		const int limit = ( maxThreads > 0 ) ? maxThreads : 1;
		return ( (size_t)limit > chunkCount ) ? (int)chunkCount : limit;
	}

	const SherpaOnnxOfflineRecognizerResult* decodeOneChunk(
		SherpaApi& api,
		const void* recognizer,
		const float* samples,
		size_t countSamples )
	{		const SherpaOnnxOfflineStream* stream = static_cast<const SherpaOnnxOfflineStream*>( api.createOfflineStream( recognizer ) );
		if( nullptr == stream )
			return nullptr;

		api.acceptWaveformOffline( stream, 16000, samples, (int32_t)countSamples );
		api.decodeOfflineStream( recognizer, stream );
		const SherpaOnnxOfflineRecognizerResult* res =
			static_cast<const SherpaOnnxOfflineRecognizerResult*>( api.getOfflineStreamResult( stream ) );
		api.destroyOfflineStream( stream );
		return res;
	}

	// 把一段文本规整化：去掉首尾空白
	void trimText( std::string& text )
	{
		const size_t b = text.find_first_not_of( " \t\r\n" );
		if( b == std::string::npos )
		{
			text.clear();
			return;
		}
		const size_t e = text.find_last_not_of( " \t\r\n" );
		text = text.substr( b, e - b + 1 );
	}

	// ===============================================================
	// SenseVoiceEngine
	// ===============================================================

	SenseVoiceEngine::SenseVoiceEngine()
	{
		// 默认按 CPU 描述；每个实例 1 个内部线程，并行度交给批量调度器，
		// 避免 onnxruntime 线程池互相超订。内存按 int8 模型 + 运行时保守估算。
		engineInfo.id = L"sherpa-sensevoice";
		infoForDevice( false );
	}

	SenseVoiceEngine::~SenseVoiceEngine()
	{
		if( nullptr != recognizer )
		{
			auto& api = SherpaApi::instance();
			if( api.available() )
				api.destroyOfflineRecognizer( recognizer );
			recognizer = nullptr;
		}
	}

	bool SenseVoiceEngine::buildConfig( ::SherpaOnnxOfflineRecognizerConfig& config,
		const CStringA& modelA, const CStringA& tokensA, int threads, const char* providerName ) const
	{
		memset( &config, 0, sizeof( config ) );
		config.feat_config.sample_rate = 16000;
		config.feat_config.feature_dim = 80;
		config.model_config.sense_voice.model = modelA;
		config.model_config.sense_voice.language = "auto";
		config.model_config.sense_voice.use_itn = 1;
		config.model_config.tokens = tokensA;
		config.model_config.num_threads = ( threads > 0 ) ? threads : 1;
		config.model_config.provider = providerName;
		config.model_config.debug = 0;
		config.decoding_method = "greedy_search";
		return true;
	}

	const void* SenseVoiceEngine::createExtraRecognizer() const
	{
		if( loadedModelA.IsEmpty() || loadedTokensA.IsEmpty() )
			return nullptr;

		auto& api = SherpaApi::instance();
		if( !api.available() || nullptr == api.createOfflineRecognizer )
			return nullptr;

		const bool gpu = ( 0 == activeProvider.CompareNoCase( L"cuda" ) );
		SherpaOnnxOfflineRecognizerConfig config;
		buildConfig( config, loadedModelA, loadedTokensA, loadedThreads, gpu ? "cuda" : "cpu" );
		return api.createOfflineRecognizer( &config );
	}

	const char* SenseVoiceEngine::languageCodeFromKey( uint32_t key )
	{
		// SenseVoice 支持 auto / zh / en / ja / ko / yue。
		// 这里的 key 是 Whisper 的 makeLanguageKey() 结果，把 ASCII 代码按小端序塞进 uint32。
		char code[ 5 ] = {};
		code[ 0 ] = (char)( key & 0xFF );
		code[ 1 ] = (char)( ( key >> 8 ) & 0xFF );
		code[ 2 ] = (char)( ( key >> 16 ) & 0xFF );
		code[ 3 ] = (char)( ( key >> 24 ) & 0xFF );

		if( 0 == code[ 0 ] )
			return "auto";
		if( 0 == strcmp( code, "zh" ) )
			return "zh";
		if( 0 == strcmp( code, "en" ) )
			return "en";
		if( 0 == strcmp( code, "ja" ) )
			return "ja";
		if( 0 == strcmp( code, "ko" ) )
			return "ko";
		if( 0 == strcmp( code, "yue" ) )
			return "yue";

		// 不支持的语种回退到自动识别，避免用户选了个引擎不认识的代码导致识别质量下降
		return "auto";
	}

	HRESULT SenseVoiceEngine::load( LPCTSTR directory, bool useGpu )
	{
		return loadFromDirectory( directory, 0, useGpu ? L"cuda" : nullptr );
	}

	// 检测 CUDA 是否可用。这里的判断是「乐观」的：
	// 只要 sherpa-onnx 能创建出 cuda 识别器就算可用，实际能不能跑由创建时的返回值决定。
	// 单独抽出来是为了能在加载前给用户一个明确的提示，而不是加载失败后才发现。
	// 检测 CUDA 是否可用。
	//
	// 注意：不要用 LoadLibrary 直接加载 onnxruntime_providers_cuda.dll 来判断！
	// 那个 DLL 的 DllMain 依赖 onnxruntime_providers_shared 里由 ORT 注入的 host 接口，
	// 由其它模块裸加载时会稳定返回 1114（ERROR_DLL_INIT_FAILED），
	// 但 ORT 自己在内部加载同一个文件是能成功的 —— 直接 LoadLibrary 只会得到假阴性。
	//
	// 这里改成检查文件是否齐全（便宜、无副作用），真正能不能跑由
	// loadFromDirectory() 里创建 cuda 识别器的结果决定，失败会自动回退 CPU 并记录原因。
	bool SenseVoiceEngine::cudaAvailable( CString& reason )
	{
		loadOrtFromExeDir();
		ensureCudaRuntimeOnPath();

		// provider + cuDNN 必须在 exe 同目录，或者能在 PATH 里找到
		struct Dep
		{
			LPCTSTR file;
			LPCTSTR hint;
		};
		static const Dep deps[] = {
			{ L"onnxruntime_providers_cuda.dll", L"缺少 onnxruntime_providers_cuda.dll（CUDA 版运行库未安装）。" },
			{ L"cudnn64_9.dll", L"缺少 cuDNN 9 运行库 cudnn64_9.dll。" },
		};

		for( const Dep& d : deps )
		{
			const CString full = pathFromExeDirectory( d.file );
			bool found = !full.IsEmpty() && ::PathFileExistsW( full );
			if( !found )
			{
				// 再看 PATH
				wchar_t buf[ MAX_PATH ] = {};
				if( ::SearchPathW( nullptr, d.file, nullptr, MAX_PATH, buf, nullptr ) > 0 )
					found = true;
			}
			if( !found )
			{
				reason = d.hint;
				return false;
			}
		}

		reason.Empty();
		return true;
	}

	// 用 0.5 秒静音跑一次推理。CUDA 路径真能工作时会正常返回（文本可能是空串）；
	// CUDA 内核 / cuDNN 缺失时 ORT 会在 ExecuteKernel 里报错，结果拿不到。
	bool SenseVoiceEngine::probeCudaInference( const void* rec, SherpaApi& api, CString& error )
	{
		if( nullptr == rec )
		{
			error = L"识别器为空。";
			return false;
		}
		if( nullptr == api.createOfflineStream || nullptr == api.acceptWaveformOffline ||
			nullptr == api.decodeOfflineStream || nullptr == api.getOfflineStreamResult )
		{
			error = L"sherpa 缺少必要导出函数。";
			return false;
		}

		const void* stream = api.createOfflineStream( rec );
		if( nullptr == stream )
		{
			error = L"创建 CUDA stream 失败。";
			return false;
		}

		// 8000 个采样点 = 0.5 秒 @16kHz，足够触发一次完整的 encoder 前向
		std::vector<float> silence( 8000, 0.0f );
		api.acceptWaveformOffline( stream, 16000, silence.data(), (int32_t)silence.size() );
		api.decodeOfflineStream( rec, stream );
		const SherpaOnnxOfflineRecognizerResult* res =
			static_cast<const SherpaOnnxOfflineRecognizerResult*>( api.getOfflineStreamResult( stream ) );

		const bool ok = ( nullptr != res );
		if( !ok )
			error = L"CUDA 推理没有返回结果（通常是 cuDNN/cuBLAS 缺失或显卡不受支持）。";

		if( nullptr != res && nullptr != api.destroyOfflineRecognizerResult )
			api.destroyOfflineRecognizerResult( res );
		api.destroyOfflineStream( stream );
		return ok;
	}

	HRESULT SenseVoiceEngine::loadFromDirectory( LPCTSTR directory, int threads, LPCTSTR provider )
	{
		auto& api = SherpaApi::instance();
		if( !api.available() )
		{
			logError( u8"SenseVoice 引擎不可用：sherpa-onnx-c-api.dll 未加载" );
			return HRESULT_FROM_WIN32( ERROR_MOD_NOT_FOUND );
		}
		// provider 的 DllMain 需要能按名字找到 cublas / cudart 等，
		// 否则 ORT 内部的 CUDA provider 注册会失败（provider 仍会 "创建成功" 但建不出 CUDA session）。
		ensureCudaRuntimeOnPath();

		// 目录为空时用环境变量或默认位置
		CString dir = directory;
		if( dir.IsEmpty() )
		{
			wchar_t envBuf[ MAX_PATH ] = {};
			const DWORD n = ::GetEnvironmentVariableW( L"WHISPERDESKTOP_SENSEVOICE_MODEL", envBuf, MAX_PATH );
			if( n > 0 && n < MAX_PATH )
				dir = envBuf;
			else
				dir = pathFromExeDirectory( L"models\\sense-voice" );
		}

		static const LPCTSTR modelCandidates[] = { L"model.int8.onnx", L"model.onnx", L"model.fp32.onnx" };
		CString foundModel = findFirstExisting( dir, modelCandidates, _countof( modelCandidates ) );
		if( foundModel.IsEmpty() )
		{
			logError( u8"SenseVoice 模型目录中没有找到 model.int8.onnx / model.onnx" );
			return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
		}

		static const LPCTSTR tokenCandidates[] = { L"tokens.txt" };
		CString foundTokens = findFirstExisting( dir, tokenCandidates, _countof( tokenCandidates ) );
		if( foundTokens.IsEmpty() )
		{
			logError( u8"SenseVoice 模型目录中没有找到 tokens.txt" );
			return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
		}

		modelPath = foundModel;
		tokensPath = foundTokens;

		// sherpa 的 C API 要 UTF-8 的窄字符串路径
		CStringA modelA( CW2A( modelPath, CP_UTF8 ) );
		CStringA tokensA( CW2A( tokensPath, CP_UTF8 ) );
		const char* langStr = "auto";

		// 决定执行后端。请求 GPU 但失败时自动回退到 CPU，而不是整个加载失败——
		// 用户至少还能用 CPU 跑完这批任务。
		CString requested = ( provider && provider[ 0 ] ) ? provider : L"cpu";
		bool wantGpu = ( 0 == requested.CompareNoCase( L"cuda" ) );
		CString activeName = L"cpu";

		auto makeRecognizer = [ & ]( const char* providerName ) -> const SherpaOnnxOfflineRecognizer*
		{
			SherpaOnnxOfflineRecognizerConfig config;
			buildConfig( config, modelA, tokensA, threads, providerName );
			return static_cast<const SherpaOnnxOfflineRecognizer*>( api.createOfflineRecognizer( &config ) );
		};

		const SherpaOnnxOfflineRecognizer* rec = nullptr;
		fallbackReason.Empty();

		if( wantGpu )
		{
			CString reason;
			if( !cudaAvailable( reason ) )
			{
				fallbackReason = reason;
				logWarning( u8"CUDA 不可用，回退到 CPU" );
			}
			else
			{
				rec = makeRecognizer( "cuda" );
				if( nullptr != rec )
				{
					activeName = L"cuda";
					// 创建成功不代表真的跑在 GPU 上：
					// ORT 注册 CUDA provider 失败时 sherpa 仍会返回非空指针，然后悄悄用 CPU。
					// 这里跑一小段静音做真实探测，确认 CUDA 路径真的能出结果。
					CString probeError;
					if( !probeCudaInference( rec, api, probeError ) )
					{
						api.destroyOfflineRecognizer( rec );
						rec = nullptr;
						activeName = L"cpu";
						fallbackReason = L"CUDA 推理探测失败，已回退到 CPU：";
						fallbackReason += probeError;
						logWarning( u8"CUDA 推理探测失败，回退到 CPU" );
					}
				}
				else
				{
					fallbackReason = L"CUDA 版识别器创建失败（可能缺少 cuDNN 或显卡不支持），已回退到 CPU。";
					logWarning( u8"CUDA 识别器创建失败，回退到 CPU" );
				}
			}
		}

		if( nullptr == rec )
			rec = makeRecognizer( "cpu" );

		if( nullptr == rec )
		{
			logError( u8"SenseVoice 创建识别器失败，模型文件可能不完整" );
			return E_FAIL;
		}

		if( nullptr != recognizer )
			api.destroyOfflineRecognizer( recognizer );
		recognizer = rec;
		activeProvider = activeName;

		// 记录加载参数，分块并行时要用它复制出多个独立识别器
		loadedModelA = modelA;
		loadedTokensA = tokensA;
		loadedThreads = ( threads > 0 ) ? threads : 1;

		// 实际用上 GPU 后，能力标志要跟着变：同一块卡上多并发只会互相抢显存
		infoForDevice( 0 == activeName.CompareNoCase( L"cuda" ) );

		// 分块内部的并行度。
		// - GPU：受显存约束，开太多只会互相抢带宽；用较保守的 2，再高收益递减。
		// - CPU：每个识别器 1 个线程，所以可以按逻辑核数开，把一段长音频压到
		//   与「块数/核数」成正比的时间。留 2 个核给 UI 和音频解码。
		if( 0 == activeName.CompareNoCase( L"cuda" ) )
		{
			chunkParallelism = 2;
		}
		else
		{
			SYSTEM_INFO si{};
			::GetSystemInfo( &si );
			const int cores = (int)si.dwNumberOfProcessors;
			chunkParallelism = ( cores > 2 ) ? ( cores - 2 ) : 1;
			if( chunkParallelism < 1 )
				chunkParallelism = 1;
		}

		logInfo( u8"SenseVoice 引擎已加载（%s，分块并行度 %d）",
			( 0 == activeName.CompareNoCase( L"cuda" ) ) ? "CUDA" : "CPU", chunkParallelism );
		return S_OK;
	}

	void SenseVoiceEngine::splitIntoCaptions( const std::string& text, uint64_t totalTicks, std::vector<sAsrSegment>& out )
	{
		// SenseVoice 输出一整段没有时间戳的文本。为了让批量输出可读，这里按句末标点切片，
		// 太长时再按空格回退到词边界硬切，最后用「已消费字符数 / 总字符数」等比映射时间轴。
		// 这是估算，不保证对齐到真实发音位置；需要精确时间戳时应改用 Whisper。
		out.clear();
		if( text.empty() || 0 == totalTicks )
			return;

		constexpr size_t maxCharsPerCaption = 60;

		// 统计总字符数（按 UTF-8 码点，而不是字节）
		size_t totalChars = 0;
		for( size_t i = 0; i < text.size(); )
		{
			const int len = utf8CharLength( (unsigned char)text[ i ] );
			if( len <= 0 || i + (size_t)len > text.size() )
				break;
			totalChars++;
			i += (size_t)len;
		}
		if( 0 == totalChars )
			return;

		auto trimBounds = [ & ]( size_t& b, size_t& e )
		{
			while( b < e && ( text[ b ] == ' ' || text[ b ] == '\t' ) )
				b++;
			while( e > b && ( text[ e - 1 ] == ' ' || text[ e - 1 ] == '\t' || text[ e - 1 ] == '\r' || text[ e - 1 ] == '\n' ) )
				e--;
		};

		// 当前正在累积的片段
		size_t segBeginByte = 0;	// 片段起始字节偏移
		size_t segBeginChar = 0;	// 片段起始字符序号
		size_t charIndex = 0;		// 已扫描过的字符数
		size_t charsInCaption = 0;	// 当前片段已有的字符数
		size_t lastSpaceByte = 0;	// 当前片段内最后一个空格之后的字节偏移
		size_t lastSpaceChar = 0;	// 同上，对应的字符序号

		auto emit = [ & ]( size_t endByte, size_t endChar )
		{
			size_t b = segBeginByte, e = endByte;
			trimBounds( b, e );
			if( e > b )
			{
				sAsrSegment seg;
				seg.text = text.substr( b, e - b );
				seg.beginTicks = totalTicks * segBeginChar / totalChars;
				seg.endTicks = totalTicks * endChar / totalChars;
				out.push_back( seg );
			}
			segBeginByte = endByte;
			segBeginChar = endChar;
			charsInCaption = 0;
			lastSpaceByte = endByte;
			lastSpaceChar = endChar;
		};

		size_t i = 0;
		while( i < text.size() )
		{
			const int len = utf8CharLength( (unsigned char)text[ i ] );
			if( len <= 0 || i + (size_t)len > text.size() )
				break;

			const bool sentenceEnd = isSentenceEnd( text.data() + i, len );
			const bool isSpace = ( len == 1 && text[ i ] == ' ' );

			i += (size_t)len;
			charIndex++;
			charsInCaption++;

			if( isSpace )
			{
				lastSpaceByte = i;
				lastSpaceChar = charIndex;
			}

			if( sentenceEnd )
			{
				emit( i, charIndex );
				continue;
			}

			if( charsInCaption < maxCharsPerCaption )
				continue;

			// 到达长度上限但还没遇到句号：优先在最近的空格处断，避免把英文单词切两半
			if( lastSpaceByte > segBeginByte && lastSpaceByte < i )
				emit( lastSpaceByte, lastSpaceChar );
			else
				emit( i, charIndex );
		}

		// 收尾：最后一段没有以标点结束
		if( segBeginByte < text.size() )
			emit( text.size(), totalChars );

		if( !out.empty() )
		{
			out.front().beginTicks = 0;
			out.back().endTicks = totalTicks;
		}
	}

	HRESULT SenseVoiceEngine::run( const float* samples, size_t countSamples, const sAsrParams& params, sAsrResult& result )
	{
		result.clear();
		result.engineName = "SenseVoice";

		if( nullptr == recognizer )
			return E_UNEXPECTED;

		auto& api = SherpaApi::instance();
		if( !api.available() )
			return E_UNEXPECTED;

		if( nullptr == samples || 0 == countSamples )
			return S_FALSE;
		if( countSamples > (size_t)INT32_MAX )
			return DISP_E_OVERFLOW;

		auto canceled = [ & ]() -> bool
		{
			return params.shouldContinue && S_OK != params.shouldContinue();
		};

		if( canceled() )
			return S_FALSE;

		const uint64_t totalTicks = (uint64_t)countSamples * ticksPerSecond / 16000;

		// SenseVoice 是非自回归模型，整段音频会被 encoder 一次性编码。
		// 中间特征张量的大小与采样点数成正比，但 hidden 维度展开后放大约 4000 倍：
		// 4 小时的会议录音(2.3 亿采样点)会要求约 927GB 的临时缓冲，ONNX Runtime
		// 分配失败后 sherpa 只打印一行错误并返回空结果；多槽并行时多个实例同时
		// 冲击内存，进程会被 Windows 以 0xc0000409 直接终止（表现为「莫名关闭」）。
		//
		// 因此这里按固定时长切片，逐段识别后拼起来。切片长度取 30 秒：
		// 语音识别模型的训练片段通常在 15~30 秒，太长反而降低准确率；
		// 30 秒对应的临时缓冲只有几十 MB，与总时长完全解耦。
		constexpr size_t chunkSamples = 16000 * 30;

		if( countSamples <= chunkSamples * 2 )
		{
			// 短音频：沿用原来的单次识别路径，行为与之前完全一致
			const SherpaOnnxOfflineRecognizerResult* raw = decodeOneChunk( api, recognizer, samples, countSamples );
			if( nullptr == raw )
			{
				logError( u8"SenseVoice 未返回结果" );
				return E_FAIL;
			}

			struct ResultGuard
			{
				SherpaApi& api;
				const SherpaOnnxOfflineRecognizerResult* result;
				~ResultGuard()
				{
					if( nullptr != result )
						api.destroyOfflineRecognizerResult( result );
				}
			} resultGuard{ api, raw };

			if( canceled() )
				return S_FALSE;

			if( nullptr != raw->lang && 0 != raw->lang[ 0 ] )
			{
				// SenseVoice 返回的是 "<|zh|>" / "<|en|>" 这种带标记的格式，剥掉包装
				std::string lang = raw->lang;
				const size_t open = lang.find( '<' );
				if( open != std::string::npos )
				{
					size_t close = lang.find( '>', open );
					if( close != std::string::npos && close > open )
						lang = lang.substr( open + 1, close - open - 1 );
				}
				const size_t lb = lang.find_first_not_of( " |" );
				const size_t le = lang.find_last_not_of( " |" );
				if( lb != std::string::npos )
					result.detectedLanguage = lang.substr( lb, le - lb + 1 );
			}

			std::string text = ( nullptr != raw->text ) ? raw->text : "";
			trimText( text );

			if( text.empty() )
			{
				if( params.onProgress )
					params.onProgress( 1.0 );
				return S_OK;
			}

			if( raw->segment_count > 0 && nullptr != raw->segment_texts_arr && nullptr != raw->segment_timestamps )
			{
				for( int32_t i = 0; i < raw->segment_count; i++ )
				{
					const char* segText = raw->segment_texts_arr[ i ];
					if( nullptr == segText || 0 == segText[ 0 ] )
						continue;
					sAsrSegment seg;
					seg.text = segText;
					const double t0 = raw->segment_timestamps[ i ];
					const double dur = ( nullptr != raw->segment_durations ) ? raw->segment_durations[ i ] : 0.0;
					seg.beginTicks = (uint64_t)( t0 * (double)ticksPerSecond );
					seg.endTicks = (uint64_t)( ( t0 + dur ) * (double)ticksPerSecond );
					result.segments.push_back( seg );
				}
			}

			if( result.segments.empty() )
				splitIntoCaptions( text, totalTicks, result.segments );

			if( params.onSegment )
			{
				for( const sAsrSegment& seg : result.segments )
					params.onSegment( seg );
			}

			if( params.onProgress )
				params.onProgress( 1.0 );
			return S_OK;
		}

		// 长音频：分块识别。
		//
		// 块与块之间相互独立，所以可以并行。每个线程必须用**自己的识别器实例**：
		// sherpa 的 recognizer 内部持有 onnxruntime session，session 不是线程安全的，
		// 多线程共用一个会导致推理结果错乱甚至崩溃。
		// 主线程的 recognizer 由 run() 的调用者独占，这里额外创建 N-1 个副本。
		std::vector<Chunk> chunks;
		if( !planChunks( countSamples, chunkSamples, chunks ) )
			return E_INVALIDARG;

		std::atomic<int> doneChunks{ 0 };
		const int threads = scheduleChunkThreads( chunks.size(), chunkParallelism );
		std::vector<const void*> pool;
		pool.reserve( (size_t)threads );
		// 第 0 个槽位复用主实例；其余新开
		pool.push_back( recognizer );
		for( int i = 1; i < threads; i++ )
		{
			const void* extra = createExtraRecognizer();
			if( nullptr == extra )
			{
				// 造不出更多识别器就退回串行，不影响正确性
				logWarning( u8"SenseVoice 无法创建额外的识别器实例，分块回退为串行" );
				break;
			}
			pool.push_back( extra );
		}

		struct PoolReleaser
		{
			SherpaApi& api;
			std::vector<const void*>& pool;
			const void* primary;
			~PoolReleaser()
			{
				for( size_t i = 0; i < pool.size(); i++ )
				{
					if( pool[ i ] == primary || nullptr == pool[ i ] )
						continue;	// 主实例由 engine 自己管
					api.destroyOfflineRecognizer( pool[ i ] );
				}
			}
		} releaser{ api, pool, recognizer };

		auto worker = [ & ]( int workerIndex )
		{
			const void* rec = pool[ (size_t)workerIndex % pool.size() ];
			for( size_t ci = (size_t)workerIndex; ci < chunks.size(); ci += (size_t)pool.size() )
			{
				if( canceled() )
					return;
				Chunk& c = chunks[ ci ];

				const SherpaOnnxOfflineRecognizerResult* raw =
					decodeOneChunk( api, rec, samples + c.beginSample, c.endSample - c.beginSample );
				if( nullptr != raw )
				{
					std::string chunkText = ( nullptr != raw->text ) ? raw->text : "";
					trimText( chunkText );
					if( !chunkText.empty() )
						c.text = std::move( chunkText );
					api.destroyOfflineRecognizerResult( raw );
				}

				// 进度：分块阶段占 0.05 ~ 0.9
				if( params.onProgress )
				{
					const int done = doneChunks.fetch_add( 1 ) + 1;
					const double frac = (double)done / (double)chunks.size();
					params.onProgress( 0.05 + 0.85 * frac );
				}
			}
		};

		std::vector<std::thread> workers;
		workers.reserve( pool.size() );
		for( size_t i = 0; i < pool.size(); i++ )
			workers.emplace_back( worker, (int)i );
		for( auto& t : workers )
			t.join();

		if( canceled() )
			return S_FALSE;

		// 按块的原始顺序拼接（并行完成顺序是乱的，顺序不能靠完成先后决定）
		std::string combined;
		size_t nonEmpty = 0;
		for( const Chunk& c : chunks )
		{
			if( c.text.empty() )
				continue;
			nonEmpty++;
			// 中文不需要空格，英文在跨段时需要补一个空格
			if( !combined.empty() &&
				( (unsigned char)combined.back() < 0x80 ) && ( (unsigned char)c.text.front() < 0x80 ) )
				combined += ' ';
			combined += c.text;
		}

		if( combined.empty() )
		{
			// 全部分块都识别失败：与单段路径一样返回空结果，但留下日志方便排查
			logWarning( u8"SenseVoice 分块识别未得到任何文本（共 %zu 段）", chunks.size() );
			if( params.onProgress )
				params.onProgress( 1.0 );
			return S_OK;
		}

		logInfo( u8"SenseVoice 分块识别完成：%zu/%zu 段有文本，并行度 %zu",
			nonEmpty, chunks.size(), pool.size() );

		splitIntoCaptions( combined, totalTicks, result.segments );
		if( params.onSegment )
		{
			for( const sAsrSegment& seg : result.segments )
				params.onSegment( seg );
		}
		if( params.onProgress )
			params.onProgress( 1.0 );
		return S_OK;
	}
}
