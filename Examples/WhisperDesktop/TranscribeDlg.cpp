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

static const LPCTSTR regValInput = L"sourceMedia";
static const LPCTSTR regValOutFormat = L"resultFormat";
static const LPCTSTR regValOutPath = L"resultPath";
static const LPCTSTR regValUseInputFolder = L"useInputFolder";
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
		reportError( m_hWnd, L"CreateThreadpoolWork failed", nullptr, hr );
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
		text = L"Multilingual";
	else
		text = L"Single-language";
	text += L" model \"";
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
	text += L" on disk, ";
	text += implString( appState.source.impl );
	text += L" implementation";

	modelDesc.SetWindowText( text );
}

// Populate the "Output Format" combobox
void TranscribeDlg::populateOutputFormats()
{
	transcribeOutFormat.AddString( L"None" );
	transcribeOutFormat.AddString( L"Text file" );
	transcribeOutFormat.AddString( L"Text with timestamps" );
	transcribeOutFormat.AddString( L"SubRip subtitles" );
	transcribeOutFormat.AddString( L"WebVTT subtitles" );
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
		return L"Pending";
	case eBatchState::Running:
		return L"Running";
	case eBatchState::Completed:
		return L"Done";
	case eBatchState::Failed:
	{
		CString txt;
		txt.Format( L"Failed (0x%08X)", hr );
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
		transcribeError( L"Please choose an output folder." );
		return false;
	}
	DWORD attrs = GetFileAttributes( folder );
	if( attrs == INVALID_FILE_ATTRIBUTES || ( attrs & FILE_ATTRIBUTE_DIRECTORY ) == 0 )
	{
		CString msg = L"The output folder does not exist:\n";
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
		transcribeError( L"Please add at least one audio file." );
		return false;
	}
	for( const BatchItem& item : batchItems )
	{
		if( PathFileExists( item.inputPath ) )
			continue;
		CString msg = L"Input audio file does not exist:\n";
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
		const int resp = MessageBox( L"Some output files already exist.\nOverwrite all of them?", L"Confirm Overwrite", MB_ICONQUESTION | MB_YESNO );
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
			transcribeError( L"Unable to queue transcription", hr );
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
		CString msg = L"Transcribe failed";
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
	transcribeButton.SetWindowText( L"Transcribe" );
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
		summary = L"Batch stopped before processing all files.";
	else
		summary = L"Completed batch transcription.";
	summary.AppendFormat( L"\nDone: %zu\nFailed: %zu", completed, failed );
	if( pending > 0 )
		summary.AppendFormat( L"\nPending: %zu", pending );
	MessageBox( summary, L"Transcribe", MB_OK | MB_ICONINFORMATION );
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
	reportError( m_hWnd, text, L"Unable to transcribe audio", hr );
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
	transcribeButton.SetWindowText( L"Stop" );
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
		rdi.AppendFormat( L"%i days, %i hours", fields.days, (int)fields.hours );
		return;
	}
	if( ( fields.hours | fields.minutes ) != 0 )
	{
		rdi.AppendFormat( L"%02d:%02d:%02d", (int)fields.hours, (int)fields.minutes, (int)fields.seconds );
		return;
	}
	rdi.AppendFormat( L"%.3f seconds", (double)ticks / 1E7 );
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
	const int res = this->MessageBox( L"Transcribe is in progress.\nDo you want to quit anyway?", L"Confirm exit", flags );
	if( res != IDYES )
		return;

	// TODO: instead of ExitProcess(), implement another callback in the DLL API, for proper cancellation of the background task
	ExitProcess( 1 );
}