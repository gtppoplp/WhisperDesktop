#include "stdafx.h"
#include "TranscribeDlg.h"
#include "Utils/logger.h"

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

	HRESULT hr = work.create( this );
	if( FAILED( hr ) )
	{
       reportError( m_hWnd, L"\u521B\u5EFA\u7EBF\u7A0B\u6C60\u5DE5\u4F5C\u5931\u8D25", nullptr, hr );
		EndDialog( IDCANCEL );
	}

	progressBar.SetRange32( 0, progressMaxInteger );
	progressBar.SetStep( 1 );

	lastInputPath = appState.stringLoad( regValInput );
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
	return 0;
}

void TranscribeDlg::printModelDescription()
{
	CString text;
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
{
    transcribeOutFormat.AddString( L"\u4E0D\u8F93\u51FA" );
	transcribeOutFormat.AddString( L"\u6587\u672C\u6587\u4EF6" );
	transcribeOutFormat.AddString( L"\u5E26\u65F6\u95F4\u6233\u7684\u6587\u672C" );
	transcribeOutFormat.AddString( L"SubRip \u5B57\u5E55" );
	transcribeOutFormat.AddString( L"WebVTT \u5B57\u5E55" );
}

// CBN_SELCHANGE notification for IDC_OUTPUT_FORMAT combobox
LRESULT TranscribeDlg::onOutFormatChange( UINT, INT, HWND, BOOL& bHandled )
{
	BOOL enabled = transcribeOutFormat.GetCurSel() != 0;
	useInputFolder.EnableWindow( enabled );
	const BOOL allowFolder = ( enabled && !isChecked( useInputFolder ) ) ? TRUE : FALSE;
	transcribeOutputPath.EnableWindow( allowFolder );
	transcribeOutputBrowse.EnableWindow( allowFolder );

	return 0;
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
	runningItem = -1;
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
	fileList.ResetContent();
	for( const BatchItem& item : batchItems )
	{
		CString text = formatStatus( item.state, item.result );
		text += L" - ";
		LPCTSTR name = PathFindFileName( item.inputPath );
		text += name;
		fileList.AddString( text );
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

	runningItem = -1;
	refreshQueueDisplay();
	return true;
}

bool TranscribeDlg::startNextItem()
{
	if( transcribeArgs.visualState != eVisualState::Running )
		return false;
	for( size_t i = 0; i < batchItems.size(); i++ )
	{
		BatchItem& item = batchItems[ i ];
		if( item.state != eBatchState::Pending )
			continue;
		item.state = eBatchState::Running;
		runningItem = (int)i;
		transcribeArgs.pathMedia = item.inputPath;
		transcribeArgs.pathOutput = item.outputPath;
		progressBar.SetPos( 0 );
		refreshQueueDisplay();
		HRESULT hr = work.post();
		if( FAILED( hr ) )
		{
			finalizeCurrentItem( hr );
          transcribeError( L"\u65E0\u6CD5\u5C06\u8F6C\u5F55\u4EFB\u52A1\u52A0\u5165\u961F\u5217", hr );
			return false;
		}
		return true;
	}
	return false;
}

void TranscribeDlg::finalizeCurrentItem( HRESULT hr )
{
	if( runningItem < 0 || runningItem >= (int)batchItems.size() )
		return;
	BatchItem& item = batchItems[ runningItem ];
	item.result = hr;
	item.state = FAILED( hr ) ? eBatchState::Failed : eBatchState::Completed;
	runningItem = -1;
	refreshQueueDisplay();
	if( FAILED( hr ) )
	{
     CString msg = L"\u8F6C\u5F55\u5931\u8D25";
		if( transcribeArgs.errorMessage.GetLength() > 0 )
		{
			msg += L"\n";
			msg += transcribeArgs.errorMessage;
		}
		transcribeError( msg, hr );
	}
}

void TranscribeDlg::finishBatch( bool canceled )
{
	transcribeArgs.visualState = eVisualState::Idle;
	setPending( false );
    transcribeButton.SetWindowText( L"\u8F6C\u5F55" );
	transcribeButton.EnableWindow( TRUE );
	progressBar.SetPos( 0 );
	updateQueueButtons();

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
	const BOOL allowFolder = ( transcribeOutFormat.GetCurSel() != 0 && !isChecked( useInputFolder ) ) ? TRUE : FALSE;
	transcribeOutputPath.EnableWindow( allowFolder );
	transcribeOutputBrowse.EnableWindow( allowFolder );
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

	transcribeArgs.language = languageSelector.selectedLanguage();
	transcribeArgs.translate = cbTranslate.checked();
	if( isInvalidTranslate( m_hWnd, transcribeArgs.language, transcribeArgs.translate ) )
		return;

	transcribeArgs.format = (eOutputFormat)(uint8_t)transcribeOutFormat.GetCurSel();
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
  transcribeButton.SetWindowText( L"\u505C\u6B62" );
	updateQueueButtons();
	if( !startNextItem() )
		finishBatch( true );
}

void __stdcall TranscribeDlg::poolCallback() noexcept
{
	HRESULT hr = transcribe();
	PostMessage( WM_CALLBACK_STATUS, (WPARAM)hr );
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

LRESULT TranscribeDlg::onCallbackStatus( UINT, WPARAM wParam, LPARAM, BOOL& )
{
	const HRESULT hr = (HRESULT)wParam;
	finalizeCurrentItem( hr );

	if( transcribeArgs.visualState == eVisualState::Stopping )
	{
		finishBatch( true );
		return 0;
	}

	if( startNextItem() )
		return 0;

	finishBatch( false );
	return 0;
}

void TranscribeDlg::getThreadError()
{
	getLastError( transcribeArgs.errorMessage );
}

#define CHECK_EX( hr ) { const HRESULT __hr = ( hr ); if( FAILED( __hr ) ) { getThreadError(); return __hr; } }

HRESULT TranscribeDlg::transcribe()
{
	transcribeArgs.startTime = GetTickCount64();
	clearLastError();
	transcribeArgs.errorMessage = L"";

	using namespace Whisper;
	CComPtr<iAudioReader> reader;

	CHECK_EX( appState.mediaFoundation->openAudioFile( transcribeArgs.pathMedia, false, &reader ) );

	const eOutputFormat format = transcribeArgs.format;
	CAtlFile outputFile;
	if( format != eOutputFormat::None )
		CHECK( outputFile.Create( transcribeArgs.pathOutput, GENERIC_WRITE, 0, CREATE_ALWAYS ) );

	transcribeArgs.resultFlags = eResultFlags::Timestamps | eResultFlags::Tokens;

	CComPtr<iContext> context;
	CHECK_EX( appState.model->createContext( &context ) );

	sFullParams fullParams;
	CHECK_EX( context->fullDefaultParams( eSamplingStrategy::Greedy, &fullParams ) );
	fullParams.language = transcribeArgs.language;
	fullParams.setFlag( eFullParamsFlags::Translate, transcribeArgs.translate );
	fullParams.resetFlag( eFullParamsFlags::PrintRealtime );

	// Setup the callbacks
	fullParams.new_segment_callback = &newSegmentCallbackStatic;
	fullParams.new_segment_callback_user_data = this;
	fullParams.encoder_begin_callback = &encoderBeginCallback;
	fullParams.encoder_begin_callback_user_data = this;

	// Setup the progress indication sink
	sProgressSink progressSink{ &progressCallbackStatic, this };
	// Run the transcribe
	CHECK_EX( context->runStreamed( fullParams, progressSink, reader ) );

	// Once finished, query duration of the audio.
	// The duration before the processing is sometimes different, by 20 seconds for the file in that issue:
	// https://github.com/Const-me/Whisper/issues/4
	CHECK_EX( reader->getDuration( transcribeArgs.mediaDuration ) );

	context->timingsPrint();

	if( format == eOutputFormat::None )
		return S_OK;

	CComPtr<iTranscribeResult> result;
	CHECK_EX( context->getResults( transcribeArgs.resultFlags, &result ) );

	sTranscribeLength len;
	CHECK_EX( result->getSize( len ) );
	const sSegment* const segments = result->getSegments();

	switch( format )
	{
	case eOutputFormat::Text:
		return writeTextFile( segments, len.countSegments, outputFile, false );
	case eOutputFormat::TextTimestamps:
		return writeTextFile( segments, len.countSegments, outputFile, true );
	case eOutputFormat::SubRip:
		return writeSubRip( segments, len.countSegments, outputFile );
	case eOutputFormat::WebVTT:
		return writeWebVTT( segments, len.countSegments, outputFile );
	default:
		return E_FAIL;
	}
}

#undef CHECK_EX

inline HRESULT TranscribeDlg::progressCallback( double p ) noexcept
{
	constexpr double mul = progressMaxInteger;
	int pos = lround( mul * p );
	progressBar.PostMessage( PBM_SETPOS, pos, 0 );
	return S_OK;
}

HRESULT __cdecl TranscribeDlg::progressCallbackStatic( double p, Whisper::iContext* ctx, void* pv ) noexcept
{
	TranscribeDlg* dlg = (TranscribeDlg*)pv;
	return dlg->progressCallback( p );
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


HRESULT TranscribeDlg::writeTextFile( const sSegment* const segments, const size_t length, CAtlFile& file, bool timestamps )
{
	using namespace Whisper;
	CHECK( writeUtf8Bom( file ) );
	CStringA line;
	for( size_t i = 0; i < length; i++ )
	{
		const sSegment& seg = segments[ i ];

		if( timestamps )
		{
			line = "[";
			printTime( line, seg.time.begin );
			line += " --> ";
			printTime( line, seg.time.end );
			line += "]  ";
		}
		else
			line = "";

		line += skipBlank( seg.text );
		line += "\r\n";
		CHECK( write( file, line ) );
	}
	return S_OK;
}

HRESULT TranscribeDlg::writeSubRip( const sSegment* const segments, const size_t length, CAtlFile& file )
{
	CHECK( writeUtf8Bom( file ) );
	CStringA line;
	for( size_t i = 0; i < length; i++ )
	{
		const sSegment& seg = segments[ i ];

		line.Format( "%zu\r\n", i + 1 );
		printTime( line, seg.time.begin, true );
		line += " --> ";
		printTime( line, seg.time.end, true );
		line += "\r\n";
		line += skipBlank( seg.text );
		line += "\r\n\r\n";
		CHECK( write( file, line ) );
	}
	return S_OK;
}

HRESULT TranscribeDlg::writeWebVTT( const sSegment* const segments, const size_t length, CAtlFile& file )
{
	CHECK( writeUtf8Bom( file ) );
	CStringA line;
	line = "WEBVTT\r\n\r\n";
	CHECK( write( file, line ) );

	for( size_t i = 0; i < length; i++ )
	{
		const sSegment& seg = segments[ i ];
		line = "";

		printTime( line, seg.time.begin, false );
		line += " --> ";
		printTime( line, seg.time.end, false );
		line += "\r\n";
		line += skipBlank( seg.text );
		line += "\r\n\r\n";
		CHECK( write( file, line ) );
	}
	return S_OK;
}

inline HRESULT TranscribeDlg::newSegmentCallback( Whisper::iContext* ctx, uint32_t n_new )
{
	using namespace Whisper;
	CComPtr<iTranscribeResult> result;
	CHECK( ctx->getResults( transcribeArgs.resultFlags, &result ) );
	return logNewSegments( result, n_new );
}

HRESULT __cdecl TranscribeDlg::newSegmentCallbackStatic( Whisper::iContext* ctx, uint32_t n_new, void* user_data ) noexcept
{
	TranscribeDlg* dlg = (TranscribeDlg*)user_data;
	return dlg->newSegmentCallback( ctx, n_new );
}

HRESULT __cdecl TranscribeDlg::encoderBeginCallback( Whisper::iContext* ctx, void* user_data ) noexcept
{
	TranscribeDlg* dlg = (TranscribeDlg*)user_data;
	const eVisualState visualState = dlg->transcribeArgs.visualState;
	switch( visualState )
	{
	case eVisualState::Idle:
		return E_NOT_VALID_STATE;
	case eVisualState::Running:
		return S_OK;
	case eVisualState::Stopping:
		return S_FALSE;
	default:
		return E_UNEXPECTED;
	}
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