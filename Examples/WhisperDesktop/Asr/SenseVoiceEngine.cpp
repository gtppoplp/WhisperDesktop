#include "stdafx.h"
#include "SenseVoiceEngine.h"
#include "Utils/logger.h"

// 官方 C API 头文件，路径在 third_party/sherpa-onnx/include 下。
// 这个目录已经加进 WhisperDesktop.vcxproj 的 AdditionalIncludeDirectories。
#include <sherpa-onnx/c-api/c-api.h>

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

	// ===============================================================
	// SenseVoiceEngine
	// ===============================================================

	SenseVoiceEngine::SenseVoiceEngine()
	{
		engineInfo.id = L"sherpa-sensevoice";
		engineInfo.displayName = L"SenseVoice (CPU)";
		engineInfo.flags = UsesCpu | SupportsTimestamps | SupportsLanguage;
		// 每个实例只用 1 个内部线程：并行度交给批量调度器，
		// 这样 N 个实例一共用 N 个核心，不会出现 onnxruntime 线程池互相超订
		engineInfo.cpuThreadsPerInstance = 1;
		// int8 模型 239MB + onnxruntime 运行时约 60MB，再加上推理临时缓冲。
		// 这个值直接参与「按内存限制并发数」的计算，宁可略保守。
		engineInfo.memoryPerInstanceMb = 320;
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
	bool SenseVoiceEngine::cudaAvailable( CString& reason )
	{
		// CUDA 版 sherpa 依赖 cudart / cudnn 等一堆 DLL，缺任何一个都会创建失败。
		// 这里先粗查一下 onnxruntime 的 CUDA provider DLL 是否在搜索路径上。
		const HMODULE mod = ::LoadLibraryW( L"onnxruntime_providers_cuda.dll" );
		if( nullptr == mod )
		{
			reason = L"未找到 onnxruntime_providers_cuda.dll。需要 CUDA 版 sherpa-onnx 与 CUDA/cuDNN 运行库。";
			return false;
		}
		::FreeLibrary( mod );
		reason.Empty();
		return true;
	}

	HRESULT SenseVoiceEngine::loadFromDirectory( LPCTSTR directory, int threads, LPCTSTR provider )
	{
		auto& api = SherpaApi::instance();
		if( !api.available() )
		{
			logError( u8"SenseVoice 引擎不可用：sherpa-onnx-c-api.dll 未加载" );
			return HRESULT_FROM_WIN32( ERROR_MOD_NOT_FOUND );
		}

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
			memset( &config, 0, sizeof( config ) );
			config.feat_config.sample_rate = 16000;
			config.feat_config.feature_dim = 80;
			config.model_config.sense_voice.model = modelA;
			config.model_config.sense_voice.language = langStr;
			config.model_config.sense_voice.use_itn = 1;
			config.model_config.tokens = tokensA;
			config.model_config.num_threads = ( threads > 0 ) ? threads : 1;
			config.model_config.provider = providerName;
			config.model_config.debug = 0;
			config.decoding_method = "greedy_search";

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
					activeName = L"cuda";
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

		// 实际用上 GPU 后，能力标志要跟着变：同一块卡上多并发只会互相抢显存
		engineInfo.flags = UsesCpu | SupportsTimestamps | SupportsLanguage;
		if( 0 == activeName.CompareNoCase( L"cuda" ) )
		{
			engineInfo.flags = UsesGpu | SupportsTimestamps | SupportsLanguage;
			engineInfo.cpuThreadsPerInstance = 0;
			// 显存占用约 1GB，但调度器看的是系统内存，这里给个保守值
			engineInfo.memoryPerInstanceMb = 256;
		}

		logInfo( u8"SenseVoice 引擎已加载（%s）", ( 0 == activeName.CompareNoCase( L"cuda" ) ) ? "CUDA" : "CPU" );
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

		const SherpaOnnxOfflineStream* stream = static_cast<const SherpaOnnxOfflineStream*>( api.createOfflineStream( recognizer ) );
		if( nullptr == stream )
		{
			logError( u8"SenseVoice 创建 stream 失败" );
			return E_OUTOFMEMORY;
		}

		struct StreamGuard
		{
			SherpaApi& api;
			const SherpaOnnxOfflineStream* stream;
			~StreamGuard()
			{
				if( nullptr != stream )
					api.destroyOfflineStream( stream );
			}
		} guard{ api, stream };

		if( params.onProgress )
			params.onProgress( 0.05 );

		api.acceptWaveformOffline( stream, 16000, samples, (int32_t)countSamples );

		if( canceled() )
			return S_FALSE;

		if( params.onProgress )
			params.onProgress( 0.2 );

		// SenseVoice 是非自回归模型，解码基本是一次前向，这里没有中间进度可报
		api.decodeOfflineStream( recognizer, stream );

		if( canceled() )
			return S_FALSE;

		if( params.onProgress )
			params.onProgress( 0.9 );

		const SherpaOnnxOfflineRecognizerResult* raw = static_cast<const SherpaOnnxOfflineRecognizerResult*>( api.getOfflineStreamResult( stream ) );
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
		size_t b = text.find_first_not_of( " \t\r\n" );
		size_t e = text.find_last_not_of( " \t\r\n" );
		if( b == std::string::npos )
			text.clear();
		else
			text = text.substr( b, e - b + 1 );

		if( text.empty() )
		{
			if( params.onProgress )
				params.onProgress( 1.0 );
			return S_OK;
		}

		// 时间轴总长 = 采样点数 / 16000 秒
		const uint64_t totalTicks = (uint64_t)countSamples * ticksPerSecond / 16000;

		// 如果 sherpa 给了分段信息，优先用它的（更准确）
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
}
