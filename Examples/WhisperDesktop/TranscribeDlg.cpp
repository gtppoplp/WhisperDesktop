#include "stdafx.h"
#include "TranscribeDlg.h"
#include "Asr/WhisperEngine.h"
#include "Utils/logger.h"
#include <dxgi1_4.h>

HRESULT TranscribeDlg::show()
{
	auto res = DoModal( nullptr );
	if( res == -1 )
		return HRESULT_FROM_WIN32( GetLastError() );
	switch( res )
	{
	case IDC_BACK:
		return SCREEN_MODEL;
	case IDC_CAPTURE:
		return SCREEN_CAPTURE;
	}
	return S_OK;
}

constexpr int progressMaxInteger = 1024 * 8;

static const LPCTSTR regValInput = L"\u6E90\u5A92\u4F53";
static const LPCTSTR regValOutFormat = L"\u7ED3\u679C\u683C\u5F0F";
static const LPCTSTR regValOutPath = L"\u7ED3\u679C\u8DEF\u5F84";
static const LPCTSTR regValUseInputFolder = L"\u4F7F\u7528\u8F93\u5165\u6587\u4EF6\u5939";
static const LPCTSTR regValEngine = L"引擎";
static const LPCTSTR regValConcurrency = L"并发数";
static const LPCTSTR regValHybrid = L"异构并行";
static const std::array<LPCTSTR, 4> outputExtensions =
{
	L".txt", L".txt", L".srt", L".vtt"
};

LRESULT TranscribeDlg::OnInitDialog( UINT nMessage, WPARAM wParam, LPARAM lParam, BOOL& bHandled )
{
	// First DDX call, hooks up variables to controls.
	DoDataExchange( false );
	printModelDescription();
	languageSelector.initialize( m_hWnd, IDC_LANGUAGE, appState );
	cbConsole.initialize( m_hWnd, IDC_CONSOLE, appState );
	cbTranslate.initialize( m_hWnd, IDC_TRANSLATE, appState );
	populateOutputFormats();
	populateEngines();

	pendingState.initialize(
		{
			languageSelector, GetDlgItem( IDC_TRANSLATE ),
			fileList, GetDlgItem( IDC_ADD_FILES ),
			GetDlgItem( IDC_REMOVE_FILE ), GetDlgItem( IDC_CLEAR_FILES ),
			transcribeOutFormat, useInputFolder,
			transcribeOutputPath, GetDlgItem( IDC_BROWSE_RESULT ),
			GetDlgItem( IDCANCEL ),
			GetDlgItem( IDC_BACK ),
			GetDlgItem( IDC_CAPTURE )
		},
		{
			progressBar, GetDlgItem( IDC_PENDING_TEXT )
		} );

	progressBar.SetRange32( 0, progressMaxInteger );
	progressBar.SetStep( 1 );

	lastInputPath = appState.stringLoad( regValInput );

	// 异构并行：两个引擎都在的时候才有意义
	hybridParallel = appState.boolLoad( regValHybrid );
	{
		HWND hHybrid = ::GetDlgItem( m_hWnd, IDC_HYBRID );
		if( ::IsWindow( hHybrid ) )
		{
			if( hybridCheck.m_hWnd != hHybrid )
			{
				if( ::IsWindow( hybridCheck.m_hWnd ) )
					hybridCheck.Detach();
				hybridCheck.Attach( hHybrid );
			}
			if( hybridParallel )
				hybridCheck.SetCheck( BST_CHECKED );
		}
	}
	{
		Asr::AppContext ctx;
		ctx.model = appState.model;
		ctx.mediaFoundation = appState.mediaFoundation;
		ctx.senseVoiceDevice = appState.senseVoiceDevice;
		ctx.senseVoiceDir = appState.senseVoiceDir;
		// 按「实际能跑在哪」判断：同一实现可能有 CPU / GPU 两个变体，
		// 只要各有一个就允许异构（例如 SenseVoice GPU + SenseVoice CPU）。
		bool haveGpu = false, haveCpu = false;
		for( const Asr::sAsrEngineInfo& e : Asr::listAvailableEngines( ctx ) )
		{
			if( e.execDevice == Asr::eEngineDevice::Gpu )
				haveGpu = true;
			else
				haveCpu = true;
		}
		const BOOL canHybrid = ( haveGpu && haveCpu ) ? TRUE : FALSE;
		if( ::IsWindow( hybridCheck.m_hWnd ) )
			hybridCheck.EnableWindow( canHybrid );
		if( !canHybrid )
		{
			if( ::IsWindow( hybridCheck.m_hWnd ) )
				hybridCheck.SetCheck( BST_UNCHECKED );
			hybridParallel = false;
		}
	}
	// 恢复上次的并发数，没有记录时留空表示「自动」
	{
		const uint32_t saved = appState.dwordLoad( regValConcurrency, 0 );
		if( saved > 0 )
		{
			CString tmp;
			tmp.Format( L"%u", saved );
			concurrencyEdit.SetWindowText( tmp );
		}
	}
	transcribeOutFormat.SetCurSel( (int)appState.dwordLoad( regValOutFormat, 0 ) );
	transcribeOutputPath.SetWindowText( appState.stringLoad( regValOutPath ) );
	if( appState.boolLoad( regValUseInputFolder ) )
		useInputFolder.SetCheck( BST_CHECKED );
	BOOL unused;
	onOutFormatChange( 0, 0, nullptr, unused );
	updateQueueButtons();

	appState.lastScreenSave( SCREEN_TRANSCRIBE );
	appState.setupIcon( this );
	ATLVERIFY( CenterWindow() );

	// SenseVoice 模式没有 Whisper 模型，麦克风捕获界面用不了，直接禁用入口
	if( nullptr == appState.model )
	{
		CWindow captureBtn = GetDlgItem( IDC_CAPTURE );
		if( captureBtn )
		{
			captureBtn.EnableWindow( FALSE );
			captureBtn.SetWindowText( L"\u9EA6\u514B\u98CE\uFF08\u4EC5 Whisper\uFF09" );
		}
	}

	return 0;
}

void TranscribeDlg::printModelDescription()
{
	CString text;

	// SenseVoice 模式：没有 Whisper 模型对象，直接显示引擎与模型目录
	if( nullptr == appState.model )
	{
		text = L"SenseVoice (CPU) \u6A21\u578B\u76EE\u5F55 \"";
		LPCTSTR dir = appState.source.path;
		dir = ::PathFindFileName( dir );
		text += dir;
		text += L"\"";
		modelDesc.SetWindowText( text );
		return;
	}

	if( S_OK == appState.model->isMultilingual() )
     text = L"\u591A\u79CD\u8BED\u8A00";
	else
      text = L"\u5355\u8BED\u8A00";
	text += L" \u6A21\u578B \"";
	LPCTSTR path = appState.source.path;
	path = ::PathFindFileName( path );
	text += path;
	text += L"\", ";
	const int64_t cb = appState.source.sizeInBytes;
	if( cb < 1 << 30 )
	{
		constexpr double mul = 1.0 / ( 1 << 20 );
		double mb = (double)cb * mul;
		text.AppendFormat( L"%.1f MB", mb );
	}
	else
	{
		constexpr double mul = 1.0 / ( 1 << 30 );
		double gb = (double)cb * mul;
		text.AppendFormat( L"%.2f GB", gb );
	}
 text += L" \u5728\u78C1\u76D8\u4E0A, ";
	text += implString( appState.source.impl );
 text += L" \u6267\u884C";

	modelDesc.SetWindowText( text );
}

// Populate the "Output Format" combobox
void TranscribeDlg::populateOutputFormats()
{    transcribeOutFormat.AddString( L"\u4E0D\u8F93\u51FA" );
	transcribeOutFormat.AddString( L"\u6587\u672C\u6587\u4EF6" );
	transcribeOutFormat.AddString( L"\u5E26\u65F6\u95F4\u6233\u7684\u6587\u672C" );
	transcribeOutFormat.AddString( L"SubRip \u5B57\u5E55" );
	transcribeOutFormat.AddString( L"WebVTT \u5B57\u5E55" );
}

// 依据当前模型 / 已安装的引擎填充下拉框
void TranscribeDlg::populateEngines()
{
	engineIds.clear();

	// 中文资源曾漏掉 IDC_ENGINE，DDX 绑不到时 ResetContent 会断言。这里强制附着。
	HWND hEngine = ::GetDlgItem( m_hWnd, IDC_ENGINE );
	if( ::IsWindow( hEngine ) )
	{
		if( engineSelector.m_hWnd != hEngine )
		{
			if( ::IsWindow( engineSelector.m_hWnd ) )
				engineSelector.Detach();
			engineSelector.Attach( hEngine );
		}
		::SendMessageW( hEngine, CB_RESETCONTENT, 0, 0 );
	}

	Asr::AppContext ctx;
	ctx.model = appState.model;
	ctx.mediaFoundation = appState.mediaFoundation;
	ctx.senseVoiceDevice = appState.senseVoiceDevice;
	ctx.senseVoiceDir = appState.senseVoiceDir;

	const std::vector<Asr::sAsrEngineInfo> engines = Asr::listAvailableEngines( ctx );
	for( const Asr::sAsrEngineInfo& e : engines )
	{
		engineIds.push_back( e.id );
		if( ::IsWindow( hEngine ) )
			::SendMessageW( hEngine, CB_ADDSTRING, 0, (LPARAM)(LPCWSTR)e.displayName );
	}

	if( engineIds.empty() )
		return;

	// 恢复上次的选择：优先用「加载模型」界面上选的引擎
	CString saved = appState.engineId;
	if( saved.IsEmpty() )
		saved = appState.stringLoad( regValEngine );
	int sel = -1;
	for( size_t i = 0; i < engineIds.size(); i++ )
	{
		if( 0 == engineIds[ i ].CompareNoCase( saved ) )
		{
			sel = (int)i;
			break;
		}
	}
	if( sel < 0 )
	{
		// 没有记录时用默认引擎
		const CString def = Asr::defaultEngineId();
		for( size_t i = 0; i < engineIds.size(); i++ )
		{
			if( 0 == engineIds[ i ].CompareNoCase( def ) )
			{
				sel = (int)i;
				break;
			}
		}
	}
	if( sel < 0 )
		sel = 0;

	if( ::IsWindow( hEngine ) )
		::SendMessageW( hEngine, CB_SETCURSEL, sel, 0 );
}

CString TranscribeDlg::selectedEngineId() const
{
	HWND hEngine = ::GetDlgItem( m_hWnd, IDC_ENGINE );
	const int sel = ::IsWindow( hEngine )
		? (int)::SendMessageW( hEngine, CB_GETCURSEL, 0, 0 )
		: -1;
	if( sel >= 0 && sel < (int)engineIds.size() )
		return engineIds[ sel ];
	return Asr::defaultEngineId();
}

void TranscribeDlg::saveEngineSelection()
{
	const CString id = selectedEngineId();
	if( !id.IsEmpty() )
		appState.stringStore( regValEngine, id );
}

// CBN_SELCHANGE notification for IDC_OUTPUT_FORMAT combobox
LRESULT TranscribeDlg::onOutFormatChange( UINT, INT, HWND, BOOL& bHandled )
{
	refreshOutputControls();
	return 0;
}

// 输出区的三件套联动：格式为「不输出」时整区置灰；
// 勾选「保存到源文件所在目录」时路径框与浏览按钮置灰。
void TranscribeDlg::refreshOutputControls()
{
	const BOOL enabled = transcribeOutFormat.GetCurSel() != 0;
	useInputFolder.EnableWindow( enabled );
	const BOOL allowFolder = ( enabled && !isChecked( useInputFolder ) ) ? TRUE : FALSE;
	transcribeOutputPath.EnableWindow( allowFolder );
	transcribeOutputBrowse.EnableWindow( allowFolder );
}

void TranscribeDlg::onAddFiles()
{
	std::vector<CString> paths;
	CString initialDir = lastInputPath;
	if( initialDir.GetLength() > 0 )
	{
		wchar_t* buf = initialDir.GetBuffer();
		if( !PathRemoveFileSpec( buf ) )
			initialDir.Empty();
		initialDir.ReleaseBuffer();
	}
	LPCTSTR seed = initialDir.GetLength() > 0 ? (LPCTSTR)initialDir : nullptr;
	if( !pickAudioFiles( m_hWnd, paths, seed ) )
		return;

	for( const CString& p : paths )
	{
		BatchItem item;
		item.inputPath = p;
		item.outputPath = L"";
		item.state = eBatchState::Pending;
		item.result = S_OK;
		batchItems.emplace_back( item );
	}
	lastInputPath = paths.front();
	refreshQueueDisplay();
	updateQueueButtons();
}

void TranscribeDlg::onRemoveFiles()
{
	const int selected = fileList.GetSelCount();
	if( selected <= 0 )
		return;
	std::vector<int> indices( selected );
	if( fileList.GetSelItems( selected, indices.data() ) == LB_ERR )
		return;
	std::sort( indices.begin(), indices.end(), []( int left, int right ) { return left > right; } );
	for( int idx : indices )
	{
		if( idx < 0 || idx >= (int)batchItems.size() )
			continue;
		batchItems.erase( batchItems.begin() + idx );
	}
	refreshQueueDisplay();
	updateQueueButtons();
}

void TranscribeDlg::onClearFiles()
{
	batchItems.clear();
	refreshQueueDisplay();
	updateQueueButtons();
}

LRESULT TranscribeDlg::onQueueSelectionChanged( UINT, INT, HWND, BOOL& )
{
	updateQueueButtons();
	return 0;
}

CString TranscribeDlg::formatStatus( eBatchState state, HRESULT hr )
{
	switch( state )
	{
	case eBatchState::Pending:
      return L"\u5F85\u529E\u7684";
	case eBatchState::Running:
      return L"\u6267\u884C\u4E2D";
	case eBatchState::Completed:
       return L"\u5B8C\u6210";
	case eBatchState::Failed:
	{
		CString txt;
       txt.Format( L"\u5931\u8D25 (0x%08X)", hr );
		return txt;
	}
	default:
		return L"";
	}
}

void TranscribeDlg::refreshQueueDisplay()
{
	// 保留当前选中项，避免刷新时选中状态丢失
	const int selCount = fileList.GetSelCount();
	std::vector<int> sel( selCount > 0 ? selCount : 0 );
	if( selCount > 0 )
		fileList.GetSelItems( selCount, sel.data() );

	fileList.ResetContent();
	double totalProgress = 0.0;
	size_t runningCount = 0;

	for( const BatchItem& item : batchItems )
	{
		CString text = formatStatus( item.state, item.result );
		text += L" - ";
		LPCTSTR name = PathFindFileName( item.inputPath );
		text += name;

		if( item.state == eBatchState::Running && item.slot >= 0 && item.slot < (int)slots.size() )
		{
			// 运行时显示进度百分比和耗时
			const int pct = (int)slots[ item.slot ].progressPct;
			const uint64_t elapsed = ( item.startTime != 0 && ::GetTickCount64() > item.startTime )
				? ( ::GetTickCount64() - item.startTime ) / 1000 : 0;
			CString extra;
			extra.Format( L"  [%d%%  %llus]", pct, (unsigned long long)elapsed );
			text += extra;
			totalProgress += pct / 100.0;
			runningCount++;
		}
		fileList.AddString( text );
	}

	// 恢复选中
	for( int i : sel )
	{
		if( i >= 0 && i < fileList.GetCount() )
			fileList.SetSel( i, TRUE );
	}

	// 进度条显示所有运行中任务的合计进度
	if( runningCount > 0 )
	{
		const double avg = totalProgress / (double)runningCount;
		progressBar.SetPos( (int)lround( avg * progressMaxInteger ) );
	}
}

void TranscribeDlg::updateQueueButtons()
{
	const BOOL hasItems = batchItems.empty() ? FALSE : TRUE;
	const int selection = fileList.GetSelCount();
	const BOOL canEdit = ( transcribeArgs.visualState == eVisualState::Idle ) ? TRUE : FALSE;
	CWindow addBtn = GetDlgItem( IDC_ADD_FILES );
	CWindow removeBtn = GetDlgItem( IDC_REMOVE_FILE );
	CWindow clearBtn = GetDlgItem( IDC_CLEAR_FILES );
	if( addBtn )
		addBtn.EnableWindow( canEdit );
	if( removeBtn )
		removeBtn.EnableWindow( canEdit && selection > 0 );
	if( clearBtn )
		clearBtn.EnableWindow( canEdit && hasItems );

	// 主操作按钮：没有待转录的文件时置灰，避免点了才弹提示
	CWindow goBtn = GetDlgItem( IDC_TRANSCRIBE );
	if( goBtn && canEdit )
		goBtn.EnableWindow( hasItems );
}

bool TranscribeDlg::ensureOutputFolder( CString& folder )
{
	folder.Trim();
	if( folder.IsEmpty() )
	{
        transcribeError( L"\u8BF7\u9009\u62E9\u8F93\u51FA\u6587\u4EF6\u5939." );
		return false;
	}
	DWORD attrs = GetFileAttributes( folder );
	if( attrs == INVALID_FILE_ATTRIBUTES || ( attrs & FILE_ATTRIBUTE_DIRECTORY ) == 0 )
	{
       CString msg = L"\u8F93\u51FA\u6587\u4EF6\u5939\u4E0D\u5B58\u5728:\n";
		msg += folder;
		transcribeError( msg );
		return false;
	}
	return true;
}

CString TranscribeDlg::composeOutputPath( const CString& input, const CString& explicitFolder ) const
{
	CString folder = explicitFolder;
	if( folder.IsEmpty() )
	{
		folder = input;
		wchar_t* buf = folder.GetBuffer();
		PathRemoveFileSpec( buf );
		folder.ReleaseBuffer();
	}

	CString fileName = PathFindFileName( input );
	CString baseName = fileName;
	wchar_t* base = baseName.GetBuffer();
	PathRemoveExtension( base );
	baseName.ReleaseBuffer();
	const int formatIndex = (int)transcribeArgs.format - 1;
	LPCTSTR ext = ( formatIndex >= 0 && formatIndex < (int)outputExtensions.size() ) ? outputExtensions[ formatIndex ] : L".txt";
	CString targetName = baseName;
	targetName += ext;

	wchar_t combined[ MAX_PATH ] = {};
	CString result;
	if( PathCombine( combined, folder, targetName ) != nullptr )
		result = combined;
	else
	{
		result = folder;
		if( !result.IsEmpty() && result[ result.GetLength() - 1 ] != L'\\' )
			result += L'\\';
		result += targetName;
	}
	return result;
}

bool TranscribeDlg::prepareBatchItems( const CString& explicitFolder )
{
	if( batchItems.empty() )
	{
       transcribeError( L"\u8BF7\u81F3\u5C11\u6DFB\u52A0\u4E00\u4E2A\u97F3\u9891\u6587\u4EF6\u3002" );
		return false;
	}
	for( const BatchItem& item : batchItems )
	{
		if( PathFileExists( item.inputPath ) )
			continue;
      CString msg = L"\u8F93\u5165\u97F3\u9891\u6587\u4EF6\u4E0D\u5B58\u5728:\n";
		msg += item.inputPath;
		transcribeError( msg, HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND ) );
		return false;
	}

	bool needOverwritePrompt = false;
	for( BatchItem& item : batchItems )
	{
		item.state = eBatchState::Pending;
		item.result = S_OK;
		if( transcribeArgs.format == eOutputFormat::None )
		{
			item.outputPath.Empty();
			continue;
		}
		item.outputPath = composeOutputPath( item.inputPath, explicitFolder );
		if( PathFileExists( item.outputPath ) )
			needOverwritePrompt = true;
	}

	if( needOverwritePrompt )
	{
      const int resp = MessageBox( L"\u90E8\u5206\u8F93\u51FA\u6587\u4EF6\u5DF2\u5B58\u5728\u3002\n\u8981\u5168\u90E8\u8986\u76D6\u5417?", L"\u786E\u8BA4\u8986\u76D6", MB_ICONQUESTION | MB_YESNO );
		if( resp != IDYES )
			return false;
	}

	refreshQueueDisplay();
	return true;
}

// 查询当前可用的物理内存（字节）。失败时返回 0，调用方应退回只按核心数估算。
static uint64_t availablePhysicalMemory()
{
	MEMORYSTATUSEX ms{};
	ms.dwLength = sizeof( ms );
	if( !::GlobalMemoryStatusEx( &ms ) )
		return 0;
	return ms.ullAvailPhys;
}

// 查询当前可用的显存（MB）。失败时返回 0，调用方应退回保守值。
// 走 DXGI 而不是 CUDA：不需要额外依赖，也不需要显卡真的支持 CUDA。
static uint32_t availableVramMb()
{
	IDXGIFactory1* factory = nullptr;
	if( FAILED( ::CreateDXGIFactory1( __uuidof( IDXGIFactory1 ), (void**)&factory ) ) || nullptr == factory )
		return 0;

	uint32_t best = 0;
	for( UINT i = 0; ; i++ )
	{
		IDXGIAdapter1* adapter = nullptr;
		if( FAILED( factory->EnumAdapters1( i, &adapter ) ) )
			break;
		if( nullptr == adapter )
			break;

		DXGI_ADAPTER_DESC1 desc{};
		if( SUCCEEDED( adapter->GetDesc1( &desc ) ) )
		{
			// 跳过软件适配器（WARP / Microsoft Basic Render）
			if( 0 == ( desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE ) )
			{
				ULARGE_INTEGER budget{};
				budget.QuadPart = desc.DedicatedVideoMemory;

				// Win10 1709+ 起系统会报告「本进程可用的显存预算」，比总显存更准：
				// 它扣掉了其它进程和其它程序已占用的部分。
				IDXGIAdapter3* adapter3 = nullptr;
				if( SUCCEEDED( adapter->QueryInterface( __uuidof( IDXGIAdapter3 ), (void**)&adapter3 ) ) && nullptr != adapter3 )
				{
					DXGI_QUERY_VIDEO_MEMORY_INFO info{};
					if( SUCCEEDED( adapter3->QueryVideoMemoryInfo( 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info ) ) )
					{
						if( info.Budget > 0 && info.Budget < budget.QuadPart )
							budget.QuadPart = info.Budget;
					}
					adapter3->Release();
				}

				const uint64_t mb = budget.QuadPart / ( 1024ull * 1024ull );
				if( mb > best )
					best = (uint32_t)std::min<uint64_t>( mb, 1024ull * 1024ull );
			}
		}
		adapter->Release();
	}
	factory->Release();
	return best;
}

// 根据引擎能力与当前硬件给出推荐的并发数。
//
// CPU 引擎受两个方向约束：
// 1) 核心数：每个实例内部可能还有自己的线程池（Whisper CPU 后端会吃满所有核），
//    所以按「逻辑核 × 2/3」封顶，留一部分给 UI、音频解码和其它进程。
//    以前这里还有一个硬编码的 8，大核机器上会被白白压死，现在去掉了，
//    真正的天花板交给内存和逻辑核数。
// 2) 可用内存：单实例常驻（实测 SenseVoice int8 约 300MB）+ 临时缓冲余量，
//    按这个值算能开几个，避免把机器压进交换区。
//
// GPU 引擎受显存约束：以前一律固定 1 个，但 int8 小模型单实例的显存占用远小于
// 整卡显存，多开几个反而能把 GPU 的 SM 喂满。现在按「引擎自报的显存 × 可用显存」
// 推算，并且同样不超过逻辑核数（否则多出来的线程只是在抢同一个 GPU，没有收益）。
int TranscribeDlg::recommendedConcurrency( const Asr::sAsrEngineInfo& engineInfo )
{
	const bool gpu = engineInfo.execDevice == Asr::eEngineDevice::Gpu;

	SYSTEM_INFO si{};
	::GetSystemInfo( &si );
	int cores = (int)si.dwNumberOfProcessors;
	if( cores < 1 )
		cores = 1;

	// 按核心数：留一部分给 UI、音频解码和其它进程。
	// 一个实例能被一个线程推着跑，所以线程数不超过逻辑核数才有意义。
	int byCores = cores * 2 / 3;
	if( byCores < 1 )
		byCores = 1;
	if( byCores > cores )
		byCores = cores;

	if( gpu )
	{
		// 没报显存占用的 GPU 引擎（例如 Whisper）：保守地只开 1 个。
		if( engineInfo.gpuMemoryPerInstanceMb < 64 )
			return 1;

		const uint32_t availVram = availableVramMb();
		if( 0 == availVram )
			return 1;

		// 单实例预算：引擎自报值 * 1.25（arena 碎片余量），但至少 128MB。
		uint64_t perInstanceMb = (uint64_t)engineInfo.gpuMemoryPerInstanceMb * 5 / 4;
		if( perInstanceMb < 128 )
			perInstanceMb = 128;

		// 只允许用掉可用显存的一部分：留 512MB 给桌面合成和其它程序，最多用 70%。
		// 显存不像内存那样可以「借用」，超了会直接分配到系统内存，性能断崖式下跌。
		constexpr uint64_t reserveMb = 512;
		const uint64_t usableMb = ( availVram > reserveMb ) ? ( ( availVram - reserveMb ) * 7 / 10 ) : 0;
		const int vramLimit = (int)( usableMb / perInstanceMb );
		if( vramLimit < 1 )
			return 1;

		return std::min( byCores, vramLimit );
	}

	// 按内存：单个实例的预算 = 引擎自报的常驻内存，加上推理期间的临时缓冲余量
	int byMemory = byCores;
	const uint64_t avail = availablePhysicalMemory();
	if( avail > 0 )
	{
		// 单实例预算：引擎自报值 * 1.5（临时缓冲/碎片余量）；没报值时按 300MB 算
		uint64_t perInstanceMb = ( engineInfo.memoryPerInstanceMb > 0 )
			? (uint64_t)engineInfo.memoryPerInstanceMb * 3 / 2
			: 300;
		if( perInstanceMb < 128 )
			perInstanceMb = 128;

		// 只允许用掉可用内存的一部分：留 1.5GB 给系统和其它程序，并且最多用 70% 可用量。
		// 这样即便别的程序临时要内存，也不会立刻把整机拖进交换。
		constexpr uint64_t reserveBytes = 1536ull * 1024 * 1024;
		uint64_t usable = ( avail > reserveBytes ) ? ( avail - reserveBytes ) : 0;
		usable = usable * 7 / 10;

		const uint64_t perInstanceBytes = perInstanceMb * 1024 * 1024;
		int memLimit = (int)( usable / perInstanceBytes );
		if( memLimit < 1 )
			memLimit = 1;
		byMemory = memLimit;
	}

	const int recommended = std::min( byCores, byMemory );
	return ( recommended < 1 ) ? 1 : recommended;
}

// 按 GPU 的推荐值推算异构模式下的 GPU 槽数量。
// 单引擎 GPU 模式沿用 recommendedConcurrency，保证两边算出来的上限一致。
static int gpuSlotLimit( const Asr::sAsrEngineInfo& gpuInfo )
{
	const int n = TranscribeDlg::recommendedConcurrency( gpuInfo );
	return ( n < 1 ) ? 1 : n;
}

HRESULT TranscribeDlg::startWorkers( const std::vector<CString>& slotEngines, const std::vector<bool>& gpuSlots )
{
	stopWorkers();

	// 上一个批次的线程如果还没退干净，不能复用 slots（会造成悬垂访问）
	if( !slots.empty() )
	{
		logError( u8"上一批工作线程尚未退出，无法启动新批次" );
		return E_UNEXPECTED;
	}

	if( slotEngines.empty() || slotEngines.size() != gpuSlots.size() )
	{
		logError( u8"没有可用的引擎槽位" );
		return E_INVALIDARG;
	}

	slots.clear();
	slots.resize( slotEngines.size() );

	for( size_t i = 0; i < slots.size(); i++ )
	{
		WorkerSlot& s = slots[ i ];
		s.engineId = slotEngines[ i ];
		s.gpu = gpuSlots[ i ];
		s.itemIndex = -1;
		s.quit = false;
		s.progressPct = 0;
		s.threadOwnerTid = 0;

		// 线程参数传槽位索引，避免依赖「认领」时序
		auto* param = new SlotLaunchParam{ this, (int)i };
		s.thread = (HANDLE)_beginthreadex( nullptr, 0, &TranscribeDlg::workerThreadProc, param, 0, nullptr );
		if( nullptr == s.thread )
		{
			delete param;
			logError( u8"创建工作线程失败" );
			stopWorkers();
			return E_FAIL;
		}
	}

	return S_OK;
}

void TranscribeDlg::stopWorkers()
{
	if( slots.empty() )
		return;

	// 先下发退出信号
	for( WorkerSlot& s : slots )
		s.quit = true;

	// 再逐个等待。注意：线程里会访问 slots[mySlot]，所以必须等全部线程真正退出后
	// 才能销毁 slots；超时的情况下不强销毁，只把句柄放掉，让 slots 保留到下次 startWorkers。
	bool allExited = true;
	for( WorkerSlot& s : slots )
	{
		if( nullptr == s.thread )
			continue;
		const DWORD res = ::WaitForSingleObject( s.thread, 10000 );
		if( res != WAIT_OBJECT_0 )
		{
			allExited = false;
			// 不 CloseHandle，也不清 slots：线程可能还在跑，清掉会造成悬垂访问
			continue;
		}
		::CloseHandle( s.thread );
		s.thread = nullptr;
	}

	// 只有在确认全部退出后才销毁槽位数组
	if( allExited )
		slots.clear();
	else
		logError( u8"部分工作线程未能按时退出，保留槽位以避免悬垂访问" );
}

unsigned __stdcall TranscribeDlg::workerThreadProc( void* pv )
{
	// 参数由 startWorkers 分配，线程自己负责释放
	std::unique_ptr<SlotLaunchParam> param( static_cast<SlotLaunchParam*>( pv ) );
	if( !param || nullptr == param->dialog )
		return 0;
	param->dialog->workerLoop( param->slotIndex );
	return 0;
}

void TranscribeDlg::workerLoop( int mySlot )
{
	if( mySlot < 0 || mySlot >= (int)slots.size() )
		return;
	slots[ mySlot ].threadOwnerTid = ::GetCurrentThreadId();

	Asr::AppContext appCtx;
	appCtx.model = appState.model;
	appCtx.mediaFoundation = appState.mediaFoundation;
	appCtx.senseVoiceDir = appState.senseVoiceDir;
	// 设备按槽位决定，而不是全局：异构并行时同一个进程里会同时存在
	// SenseVoice 的 GPU 实例和 CPU 实例。引擎 id 里已经带了设备信息
	// （sherpa-sensevoice / sherpa-sensevoice-gpu），这里不再覆盖。
	appCtx.senseVoiceDevice = slots[ mySlot ].gpu ? L"gpu" : L"cpu";

	// 每个线程持有自己的引擎实例，避免上下文串味
	std::unique_ptr<Asr::iAsrEngine> engine = Asr::createEngine( slots[ mySlot ].engineId, appCtx );
	HRESULT engineHr = E_UNEXPECTED;
	CString engineError;
	if( !engine )
	{
		engineError = L"无法创建引擎实例，请检查模型是否已加载";
	}
	else
	{
		engineHr = engine->load( nullptr, true );
		if( FAILED( engineHr ) )
			engineError = L"引擎加载模型失败";
	}
	if( !engineError.IsEmpty() )
	{
		logError( u8"工作线程无法初始化引擎，整批任务将被标记为失败" );
		// 把队列里所有待办任务直接标记为失败，避免用户面对一堆无意义的报错
		{
			CComCritSecLock<CComAutoCriticalSection> lock( batchCs );
			for( size_t i = 0; i < batchItems.size(); i++ )
			{
				BatchItem& item = batchItems[ i ];
				if( item.state == eBatchState::Pending )
				{
					item.state = eBatchState::Failed;
					item.result = engineHr;
					item.errorMessage = engineError;
				}
			}
		}
		if( ::IsWindow( m_hWnd ) )
			::PostMessage( m_hWnd, WM_CALLBACK_STATUS, (WPARAM)S_FALSE, (LPARAM)-1 );
		return;
	}

	while( true )
	{
		if( slots[ mySlot ].quit )
			break;

		// 领取一个待办任务
		int index = -1;
		CString inputPath, outputPath;
		bool notRunning = false;
		{
			CComCritSecLock<CComAutoCriticalSection> lock( batchCs );

			// 注意：不能在持有锁的情况下 Sleep()，否则会卡住其他工作线程
			if( transcribeArgs.visualState != eVisualState::Running )
				notRunning = true;
			else
			{
				// 异构模式下按「从两头向中间」的顺序取任务：
				// GPU 槽从队头取，CPU 槽从队尾取。
				// 这样慢的 GPU 和快的 CPU 各自认领一半，不会出现 GPU 堆积、
				// CPU 早早空闲的情况；两边在中点会合后，先做完的继续抢剩下的。
				const bool fromHead = slots[ mySlot ].gpu || !hybridParallel;
				const size_t count = batchItems.size();
				for( size_t k = 0; k < count; k++ )
				{
					const size_t i = fromHead ? k : ( count - 1 - k );
					BatchItem& item = batchItems[ i ];
					if( item.state != eBatchState::Pending )
						continue;
					item.state = eBatchState::Running;
					item.slot = mySlot;
					item.startTime = ::GetTickCount64();
					index = (int)i;
					inputPath = item.inputPath;
					outputPath = item.outputPath;
					break;
				}
			}
		}

		// 不在运行状态：短暂等待后重试（可能刚启动）
		if( notRunning )
		{
			::Sleep( 50 );
			continue;
		}

		if( index < 0 )
		{
			// 没有待办任务：如果整批已经结束，就等主线程发退出信号
			::Sleep( 50 );
			continue;
		}

		slots[ mySlot ].itemIndex = index;
		slots[ mySlot ].progressPct = 0;

		HRESULT hr = transcribeFileInWorker( *engine, inputPath, outputPath, mySlot );
		CString error;

		// 如果引擎自己在工作线程里报告了错误，这里补上
		if( FAILED( hr ) && error.IsEmpty() )
			getLastError( error );

		slots[ mySlot ].itemIndex = -1;
		finalizeItem( index, hr, error, mySlot );
	}

	// 线程退出前清掉认领标记
	{
		CComCritSecLock<CComAutoCriticalSection> lock( batchCs );
		if( mySlot >= 0 && mySlot < (int)slots.size() )
			slots[ mySlot ].threadOwnerTid = 0;
	}
}

void TranscribeDlg::finalizeItem( int itemIndex, HRESULT hr, const CString& error, int slotIndex )
{
	bool allDone = false;
	size_t counts[ 4 ] = {};
	{
		CComCritSecLock<CComAutoCriticalSection> lock( batchCs );
		if( itemIndex < 0 || itemIndex >= (int)batchItems.size() )
			return;

		BatchItem& item = batchItems[ itemIndex ];
		item.result = hr;
		item.state = FAILED( hr ) ? eBatchState::Failed : eBatchState::Completed;
		item.errorMessage = error;
		item.slot = -1;

		if( slotIndex >= 0 && slotIndex < (int)slots.size() )
			slots[ slotIndex ].progressPct = 0;

		// 统计是否全部结束
		bool finished = true;
		size_t done = 0, failed = 0, pending = 0;
		for( const BatchItem& it : batchItems )
		{
			switch( it.state )
			{
			case eBatchState::Completed: done++; break;
			case eBatchState::Failed: failed++; break;
			case eBatchState::Pending: pending++; finished = false; break;
			default: finished = false; break;
			}
		}
		counts[ 0 ] = done;
		counts[ 1 ] = failed;
		counts[ 2 ] = pending;
		allDone = finished;
	}

	// 通知主线程刷新 UI
	if( ::IsWindow( m_hWnd ) )
		::PostMessage( m_hWnd, WM_CALLBACK_STATUS, (WPARAM)hr, (LPARAM)itemIndex );

	if( allDone )
	{
		// 让主线程收尾
		if( ::IsWindow( m_hWnd ) )
			::PostMessage( m_hWnd, WM_CALLBACK_STATUS, (WPARAM)S_FALSE, (LPARAM)-1 );
	}
}

void TranscribeDlg::finishBatch( bool canceled )
{
	transcribeArgs.visualState = eVisualState::Idle;
	stopWorkers();
	KillTimer( 1 );

	setPending( false );
	transcribeButton.SetWindowText( L"\u8F6C\u5F55" );
	transcribeButton.EnableWindow( TRUE );
	progressBar.SetPos( 0 );
	updateQueueButtons();
	refreshQueueDisplay();

	size_t completed = 0;
	size_t failed = 0;
	size_t pending = 0;
	for( const BatchItem& item : batchItems )
	{
		switch( item.state )
		{
		case eBatchState::Completed:
			completed++;
			break;
		case eBatchState::Failed:
			failed++;
			break;
		case eBatchState::Pending:
			pending++;
			break;
		default:
			break;
		}
	}
	CString summary;
	if( canceled )
		summary = L"\u6279\u91CF\u4EFB\u52A1\u5728\u5904\u7406\u5B8C\u6240\u6709\u6587\u4EF6\u524D\u5DF2\u505C\u6B62\u3002";
	else
		summary = L"\u6279\u91CF\u8F6C\u5F55\u5DF2\u5B8C\u6210\u3002";
	summary.AppendFormat( L"\n\u5B8C\u6210: %zu\n\u5931\u8D25: %zu", completed, failed );
	if( pending > 0 )
		summary.AppendFormat( L"\n\u5F85\u5904\u7406: %zu", pending );
	MessageBox( summary, L"\u8F6C\u5F55", MB_OK | MB_ICONINFORMATION );
}

void TranscribeDlg::onInputFolderCheck()
{
	refreshOutputControls();
}

void TranscribeDlg::onBrowseOutput()
{
	CString folder;
	transcribeOutputPath.GetWindowText( folder );
	if( !browseForFolder( m_hWnd, folder ) )
		return;
	transcribeOutputPath.SetWindowText( folder );
}

void TranscribeDlg::setPending( bool nowPending )
{
	pendingState.setPending( nowPending );
}

void TranscribeDlg::transcribeError( LPCTSTR text, HRESULT hr )
{
    reportError( m_hWnd, text, L"\u65E0\u6CD5\u8F6C\u5F55\u97F3\u9891", hr );
}

void TranscribeDlg::onTranscribe()
{
	switch( transcribeArgs.visualState )
	{
	case eVisualState::Running:
		transcribeArgs.visualState = eVisualState::Stopping;
		transcribeButton.EnableWindow( FALSE );
		return;
	case eVisualState::Stopping:
		return;
	}

	transcribeArgs.language = languageSelector.selectedLanguage();	transcribeArgs.translate = cbTranslate.checked();
	if( isInvalidTranslate( m_hWnd, transcribeArgs.language, transcribeArgs.translate ) )
		return;

	transcribeArgs.format = (eOutputFormat)(uint8_t)transcribeOutFormat.GetCurSel();
	transcribeArgs.engineId = selectedEngineId();
	saveEngineSelection();
	CString explicitFolder;
	if( transcribeArgs.format == eOutputFormat::None )
		cbConsole.ensureChecked();
	else if( !isChecked( useInputFolder ) )
	{
		transcribeOutputPath.GetWindowText( explicitFolder );
		if( !ensureOutputFolder( explicitFolder ) )
			return;
	}

	appState.dwordStore( regValOutFormat, (uint32_t)(int)transcribeArgs.format );
	appState.boolStore( regValUseInputFolder, isChecked( useInputFolder ) );
	
	// 读取用户指定的并发数，0 或非法值表示用推荐值
	{
		CString concStr;
		GetDlgItem( IDC_CONCURRENCY ).GetWindowText( concStr );
		requestedConcurrency = _ttoi( concStr );
		if( requestedConcurrency < 0 )
			requestedConcurrency = 0;
		if( requestedConcurrency > 0 )
			appState.dwordStore( regValConcurrency, (uint32_t)requestedConcurrency );
	}
	hybridParallel = isChecked( hybridCheck );
	appState.boolStore( regValHybrid, hybridParallel );
	languageSelector.saveSelection( appState );
	cbTranslate.saveSelection( appState );
	if( !explicitFolder.IsEmpty() )
		appState.stringStore( regValOutPath, explicitFolder );
	if( !batchItems.empty() )
		appState.stringStore( regValInput, batchItems.front().inputPath );

	if( !prepareBatchItems( explicitFolder ) )
		return;

	setPending( true );
	transcribeArgs.visualState = eVisualState::Running;
	transcribeButton.SetWindowText( L"\u505C\u6B62\u8F6C\u5F55" );
	updateQueueButtons();

	// 并发数：按引擎能力与硬件推算（CPU 看核数/内存，GPU 看显存），用户可覆盖
	int recommended = 1;	Asr::sAsrEngineInfo activeInfo;
	bool haveInfo = false;
	{
		Asr::AppContext ctx;
		ctx.model = appState.model;
		ctx.mediaFoundation = appState.mediaFoundation;
		const std::vector<Asr::sAsrEngineInfo> engines = Asr::listAvailableEngines( ctx );
		for( const Asr::sAsrEngineInfo& e : engines )
		{
			if( 0 == e.id.CompareNoCase( transcribeArgs.engineId ) )
			{
				recommended = recommendedConcurrency( e );
				activeInfo = e;
				haveInfo = true;
				break;
			}
		}
	}

	// 异构模式下用户填的并发数只约束 CPU 槽，GPU 槽单独按显存推算，
	// 所以拿 CPU 变体的上限来封顶；否则填个 2 就会把 CPU 槽一起压成 2 个。
	int cpuCap = recommended;
	if( haveInfo && activeInfo.execDevice == Asr::eEngineDevice::Gpu && hybridParallel )
	{
		Asr::AppContext ctx;
		ctx.model = appState.model;
		ctx.mediaFoundation = appState.mediaFoundation;
		const std::vector<Asr::sAsrEngineInfo> engines = Asr::listAvailableEngines( ctx );
		const CString cpuId = Asr::cpuVariantId( engines, activeInfo.id );
		for( const Asr::sAsrEngineInfo& e : engines )
		{
			if( !cpuId.IsEmpty() && 0 == e.id.CompareNoCase( cpuId ) )
			{
				cpuCap = recommendedConcurrency( e );
				break;
			}
		}
	}

	int concurrency = recommended;
	if( requestedConcurrency > 0 )
	{
		concurrency = requestedConcurrency;
		// 用户手填的值也要过一遍硬件限制，否则填个 64 会把机器直接打爆
		if( haveInfo )
		{
			const int cap = ( activeInfo.execDevice == Asr::eEngineDevice::Gpu ) ? cpuCap : recommended;
			if( concurrency > cap )
			{
				logWarning( u8"并发数超过硬件建议上限，已下调" );
				concurrency = cap;
			}
		}
	}

	// 统计待办文件数：并发数不应超过它，
	// 多出来的线程会白加载一遍模型（每个约 1.6 秒 / 320MB），没有收益。
	size_t pendingCount = 0;
	for( const BatchItem& item : batchItems )
	{
		if( item.state == eBatchState::Pending )
			pendingCount++;
	}
	if( pendingCount == 0 )
	{
		finishBatch( false );
		return;
	}

	// 组装槽位：每个槽位绑定一个引擎。
	// 异构模式下 GPU 槽和 CPU 槽（可能是同一个实现的两种设备）共享同一个任务队列。
	std::vector<CString> slotEngines = planSlots( transcribeArgs.engineId, concurrency, pendingCount );
	const std::vector<bool> gpuSlots = resolveGpuSlots( slotEngines );

	HRESULT hr = startWorkers( slotEngines, gpuSlots );
	if( FAILED( hr ) )
	{
		transcribeError( L"无法创建工作线程", hr );
		finishBatch( true );
		return;
	}

	// 运行期间每 500ms 刷新一次进度显示
	SetTimer( 1, 500 );

	// 把实际使用的并发数写回输入框，让用户看到「自动」到底算出了多少
	{
		CString tmp;
		tmp.Format( L"%d", (int)slotEngines.size() );
		concurrencyEdit.SetWindowText( tmp );
	}
}

// 规划工作槽。
//
// 单引擎模式：所有槽用同一个引擎，数量按硬件推荐值，且不超过待办文件数。
// 异构模式：GPU 槽数量按显存推算（int8 小模型可以开多个，把 SM 喂饱），
//          CPU 槽数量按核心数/内存推算。两边共享同一个队列，谁空闲谁取下一个文件。
// 选中的是 GPU 变体时，优先用「同一实现的 CPU 变体」做异构
// （例如 SenseVoice GPU + SenseVoice CPU），这样两边的速度可比、文本风格一致。
std::vector<CString> TranscribeDlg::planSlots( const CString& engineId, int requested, size_t pendingCount )
{
	std::vector<CString> result;

	Asr::AppContext ctx;
	ctx.model = appState.model;
	ctx.mediaFoundation = appState.mediaFoundation;
	ctx.senseVoiceDevice = appState.senseVoiceDevice;
	ctx.senseVoiceDir = appState.senseVoiceDir;

	const std::vector<Asr::sAsrEngineInfo> engines = Asr::listAvailableEngines( ctx );

	Asr::sAsrEngineInfo chosen;
	bool haveChosen = false;
	for( const Asr::sAsrEngineInfo& e : engines )
	{
		if( 0 == e.id.CompareNoCase( engineId ) )
		{
			chosen = e;
			haveChosen = true;
			break;
		}
	}

	// 找出可用的 GPU 引擎与 CPU 引擎（用于异构）。
	// 只看 execDevice：UsesGpu/UsesCpu 是「实现支持哪些后端」，入口处判断异构可用性时用它；
	// 这里要的是「这条条目实际跑在哪」，必须用 execDevice，否则同一实现的两个变体会互相顶掉。
	const Asr::sAsrEngineInfo* gpuInfo = nullptr;
	const Asr::sAsrEngineInfo* cpuInfo = nullptr;
	for( const Asr::sAsrEngineInfo& e : engines )
	{
		if( e.execDevice == Asr::eEngineDevice::Gpu && nullptr == gpuInfo )
			gpuInfo = &e;
		else if( e.execDevice == Asr::eEngineDevice::Cpu && nullptr == cpuInfo )
			cpuInfo = &e;
	}

	// 选中的是 CPU 变体、但同类实现还有 GPU 变体时，也可以组成异构
	if( nullptr != gpuInfo && haveChosen && chosen.execDevice == Asr::eEngineDevice::Cpu )
	{
		for( const Asr::sAsrEngineInfo& e : engines )
		{
			if( e.execDevice != Asr::eEngineDevice::Gpu )
				continue;
			if( (uint32_t)e.flags == (uint32_t)chosen.flags )
			{
				gpuInfo = &e;
				break;
			}
		}
	}

	// 勾选了异构并行、且两类引擎都存在时才走异构
	const bool wantHybrid = hybridParallel && nullptr != gpuInfo && nullptr != cpuInfo;

	if( wantHybrid )
	{
		// CPU 槽数量：用户指定值，否则按 CPU 引擎的硬件推荐值
		int cpuSlots = ( requested > 0 ) ? requested : recommendedConcurrency( *cpuInfo );
		if ( cpuSlots < 1 )
			cpuSlots = 1;

		// GPU 槽数量：按显存推算，用户填的值不参与（显存不够开多了只会掉到系统内存）
		const int gpuSlots = gpuSlotLimit( *gpuInfo );

		// 注意顺序：CPU 槽在前、GPU 槽在后。
		// 后面会按待办文件数截断，把 CPU 放前面可以保证「文件少时优先用快的 CPU 引擎」——
		// SenseVoice 是非自回归模型，单文件比 Whisper 快一个数量级。
		for( int i = 0; i < cpuSlots; i++ )
			result.push_back( cpuInfo->id );
		for( int i = 0; i < gpuSlots; i++ )
			result.push_back( gpuInfo->id );
	}
	else
	{
		const int cap = haveChosen ? recommendedConcurrency( chosen ) : 1;
		int count = ( requested > 0 ) ? std::min( requested, cap ) : cap;
		if( count < 1 )
			count = 1;
		for( int i = 0; i < count; i++ )
			result.push_back( haveChosen ? chosen.id : engineId );
	}

	// 槽位数不超过待办文件数，避免空转线程白加载一遍模型
	if( result.size() > pendingCount )
		result.resize( pendingCount );
	if( result.empty() )
		result.push_back( engineId );

	return result;
}

std::vector<bool> TranscribeDlg::resolveGpuSlots( const std::vector<CString>& slotEngines )
{
	Asr::AppContext context;
	context.model = appState.model;
	context.mediaFoundation = appState.mediaFoundation;
	context.senseVoiceDir = appState.senseVoiceDir;
	const std::vector<Asr::sAsrEngineInfo> engines = Asr::listAvailableEngines( context );

	std::vector<bool> result;
	result.reserve( slotEngines.size() );
	for( const CString& id : slotEngines )
		result.push_back( Asr::isGpuEngineInfo( engines, id ) );
	return result;
}

static void printTime( CString& rdi, int64_t ticks )
{
	const Whisper::sTimeSpan ts{ (uint64_t)ticks };
	const Whisper::sTimeSpanFields fields = ts;

	if( fields.days != 0 )
	{
       rdi.AppendFormat( L"%i \u5929, %i \u5C0F\u65F6", fields.days, (int)fields.hours );
		return;
	}
	if( ( fields.hours | fields.minutes ) != 0 )
	{
		rdi.AppendFormat( L"%02d:%02d:%02d", (int)fields.hours, (int)fields.minutes, (int)fields.seconds );
		return;
	}
   rdi.AppendFormat( L"%.3f \u79D2", (double)ticks / 1E7 );
}

// 运行期间定时刷新队列显示（进度、耗时）
LRESULT TranscribeDlg::onTimer( UINT, WPARAM wParam, LPARAM, BOOL& )
{
	constexpr UINT_PTR timerId = 1;
	if( wParam == timerId && transcribeArgs.visualState == eVisualState::Running )
		refreshQueueDisplay();
	return 0;
}

// 工作线程完成任务后回投的消息。
// wParam = 该任务的 HRESULT，lParam = 队列索引；lParam == -1 表示整批结束。
LRESULT TranscribeDlg::onCallbackStatus( UINT, WPARAM wParam, LPARAM lParam, BOOL& )
{
	const int itemIndex = (int)lParam;
	const HRESULT hr = (HRESULT)wParam;

	// 刷新队列显示（状态已在工作线程里更新好了）
	refreshQueueDisplay();

	// 如果是「整批结束」的通知
	if( itemIndex < 0 )
	{
		if( transcribeArgs.visualState == eVisualState::Stopping )
		{
			finishBatch( true );
			return 0;
		}
		finishBatch( false );
		return 0;
	}

	// 单个任务失败时，把该任务自己的错误信息提示出来，但不中断整批
	if( FAILED( hr ) && itemIndex < (int)batchItems.size() )
	{
		const BatchItem& item = batchItems[ itemIndex ];
		CString msg = L"\u8F6C\u5F55\u5931\u8D25:\n";
		msg += PathFindFileName( item.inputPath );
		if( !item.errorMessage.IsEmpty() )
		{
			msg += L"\n";
			msg += item.errorMessage;
		}
		transcribeError( msg, hr );
	}

	return 0;
}

// 在工作线程里把单个文件转成文本。
// engine 由调用方持有并复用，避免每个文件都重新加载模型。
// 这里不碰 UI 控件，所有跨线程通知都通过槽位 + PostMessage。
HRESULT TranscribeDlg::transcribeFileInWorker( Asr::iAsrEngine& engine, const CString& mediaPath, const CString& outputPath, int slotIndex )
{
	clearLastError();

	Asr::sAsrParams params;
	params.language = transcribeArgs.language;
	params.translate = transcribeArgs.translate;
	params.wantTimestamps = ( transcribeArgs.format == eOutputFormat::TextTimestamps ||
		transcribeArgs.format == eOutputFormat::SubRip ||
		transcribeArgs.format == eOutputFormat::WebVTT );

	// 进度：换算成百分比存进槽位，主线程定时刷新进度条
	params.onProgress = [ this, slotIndex ]( double p ) -> HRESULT
	{
		if( slotIndex >= 0 && slotIndex < (int)slots.size() )
			slots[ slotIndex ].progressPct = (LONG)lround( p * 100.0 );
		return ( transcribeArgs.visualState == eVisualState::Running ) ? S_OK : S_FALSE;
	};

	params.onSegment = [ this ]( const Asr::sAsrSegment& seg ) -> HRESULT
	{
		// 与改动前 newSegmentCallback -> logNewSegments 的输出格式保持一致
		CStringA line;
		line = "[";
		printTime( line, Whisper::sTimeSpan{ seg.beginTicks } );
		line += " --> ";
		printTime( line, Whisper::sTimeSpan{ seg.endTicks } );
		line += "]  ";
		line += seg.text.c_str();
		logInfo( u8"%s", cstr( line ) );
		return S_OK;
	};

	params.shouldContinue = [ this ]() -> HRESULT
	{
		return ( transcribeArgs.visualState == eVisualState::Running ) ? S_OK : S_FALSE;
	};

	Asr::sAsrResult asrResult;
	const HRESULT hr = transcribeWithEngine( engine, mediaPath, params, asrResult );
	if( FAILED( hr ) )
		return hr;

	// 用户点了停止，不写半截文件
	if( transcribeArgs.visualState != eVisualState::Running &&
		transcribeArgs.visualState != eVisualState::Idle )
		return S_FALSE;

	if( transcribeArgs.format == eOutputFormat::None )
		return S_OK;

	CAtlFile outputFile;
	CHECK( outputFile.Create( outputPath, GENERIC_WRITE, 0, CREATE_ALWAYS ) );

	const Asr::sAsrSegment* const segments = asrResult.segments.data();
	const size_t count = asrResult.segments.size();

	switch( transcribeArgs.format )
	{
	case eOutputFormat::Text:
		return writeTextFile( segments, count, outputFile, false );
	case eOutputFormat::TextTimestamps:
		return writeTextFile( segments, count, outputFile, true );
	case eOutputFormat::SubRip:
		return writeSubRip( segments, count, outputFile );
	case eOutputFormat::WebVTT:
		return writeWebVTT( segments, count, outputFile );
	default:
		return E_FAIL;
	}
}

HRESULT TranscribeDlg::transcribeWithEngine( Asr::iAsrEngine& engine, LPCTSTR mediaPath, const Asr::sAsrParams& params, Asr::sAsrResult& result )
{
	// Whisper 的 GPU 引擎内部自带 Media Foundation 增量解码（边读边算，不需要把整段 PCM 读进内存），
	// 对长文件更省内存，也保留了原有的流式进度上报。
	Asr::WhisperEngine* whisperImpl = dynamic_cast<Asr::WhisperEngine*>( &engine );
	if( nullptr != whisperImpl )
		return whisperImpl->transcribeFile( mediaPath, params, result );

	// 其它引擎（SenseVoice 等）统一走「整段 PCM in / 结果 out」的路径。
	CComPtr<Whisper::iAudioBuffer> buffer;
	HRESULT hr = appState.mediaFoundation->loadAudioFile( mediaPath, false, &buffer );
	if( FAILED( hr ) )
		return hr;

	const float* const pcm = buffer->getPcmMono();
	const uint32_t count = buffer->countSamples();
	if( nullptr == pcm || 0 == count )
		return E_INVALIDARG;

	return engine.run( pcm, count, params, result );
}

namespace
{
	HRESULT write( CAtlFile& file, const CStringA& line )
	{
		if( line.GetLength() > 0 )
			CHECK( file.Write( cstr( line ), (DWORD)line.GetLength() ) );
		return S_OK;
	}

	const char* skipBlank( const char* rsi )
	{
		while( true )
		{
			const char c = *rsi;
			if( c == ' ' || c == '\t' )
			{
				rsi++;
				continue;
			}
			return rsi;
		}
	}
}

using Whisper::sSegment;
using Asr::sAsrSegment;

HRESULT TranscribeDlg::writeTextFile( const sAsrSegment* const segments, const size_t length, CAtlFile& file, bool timestamps )
{
	using namespace Whisper;
	CHECK( writeUtf8Bom( file ) );
	CStringA line;
	for( size_t i = 0; i < length; i++ )
	{
		const sAsrSegment& seg = segments[ i ];

		if( timestamps )
		{
			line = "[";
			printTime( line, sTimeSpan{ seg.beginTicks } );
			line += " --> ";
			printTime( line, sTimeSpan{ seg.endTicks } );
			line += "]  ";
		}
		else
			line = "";

		line += skipBlank( seg.text.c_str() );
		line += "\r\n";
		CHECK( write( file, line ) );
	}
	return S_OK;
}

HRESULT TranscribeDlg::writeSubRip( const sAsrSegment* const segments, const size_t length, CAtlFile& file )
{
	using namespace Whisper;
	CHECK( writeUtf8Bom( file ) );
	CStringA line;
	for( size_t i = 0; i < length; i++ )
	{
		const sAsrSegment& seg = segments[ i ];

		line.Format( "%zu\r\n", i + 1 );
		printTime( line, sTimeSpan{ seg.beginTicks }, true );
		line += " --> ";
		printTime( line, sTimeSpan{ seg.endTicks }, true );
		line += "\r\n";
		line += skipBlank( seg.text.c_str() );
		line += "\r\n\r\n";
		CHECK( write( file, line ) );
	}
	return S_OK;
}

HRESULT TranscribeDlg::writeWebVTT( const sAsrSegment* const segments, const size_t length, CAtlFile& file )
{
	using namespace Whisper;
	CHECK( writeUtf8Bom( file ) );
	CStringA line;
	line = "WEBVTT\r\n\r\n";
	CHECK( write( file, line ) );

	for( size_t i = 0; i < length; i++ )
	{
		const sAsrSegment& seg = segments[ i ];
		line = "";

		printTime( line, sTimeSpan{ seg.beginTicks }, false );
		line += " --> ";
		printTime( line, sTimeSpan{ seg.endTicks }, false );
		line += "\r\n";
		line += skipBlank( seg.text.c_str() );
		line += "\r\n\r\n";
		CHECK( write( file, line ) );
	}
	return S_OK;
}

void TranscribeDlg::onWmClose()
{
	if( GetDlgItem( IDCANCEL ).IsWindowEnabled() )
	{
		EndDialog( IDCANCEL );
		return;
	}

	constexpr UINT flags = MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2;
    const int res = this->MessageBox( L"\u6B63\u5728\u8F6C\u5F55\u4E2D\u3002\n\u4ECD\u7136\u8981\u9000\u51FA\u5417?", L"\u786E\u8BA4\u9000\u51FA", flags );
	if( res != IDYES )
		return;

	// TODO: instead of ExitProcess(), implement another callback in the DLL API, for proper cancellation of the background task
	ExitProcess( 1 );
}