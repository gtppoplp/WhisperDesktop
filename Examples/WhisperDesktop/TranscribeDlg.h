#pragma once
#include "AppState.h"
#include "Utils/WTL/atlddx.h"
#include "Utils/WTL/atlcrack.h"
#include "Utils/miscUtils.h"
#include "Utils/LanguageDropdown.h"
#include "Utils/TranslateCheckbox.h"
#include "Utils/PendingState.h"

class TranscribeDlg :
	public CDialogImpl<TranscribeDlg>,
	public CWinDataExchange<TranscribeDlg>,
	public iThreadPoolCallback
{
	AppState& appState;

public:
	static constexpr UINT IDD = IDD_TRANSCRIBE_DIALOG;
	static constexpr UINT WM_CALLBACK_STATUS = WM_APP + 1;

	TranscribeDlg( AppState& app ) : appState( app ) { }

	// Show this dialog modally, without parent.
	HRESULT show();

	BEGIN_MSG_MAP( LoadModelDlg )
		MESSAGE_HANDLER( WM_INITDIALOG, OnInitDialog )
		ON_BUTTON_CLICK( IDC_CONSOLE, cbConsole.click )
		ON_BUTTON_CLICK( IDCANCEL, onClose )
		ON_BUTTON_CLICK( IDC_BACK, onBack )
		ON_BUTTON_CLICK( IDC_USE_INPUT_FOLDER, onInputFolderCheck )
		ON_BUTTON_CLICK( IDC_ADD_FILES, onAddFiles )
		ON_BUTTON_CLICK( IDC_REMOVE_FILE, onRemoveFiles )
		ON_BUTTON_CLICK( IDC_CLEAR_FILES, onClearFiles )
		ON_BUTTON_CLICK( IDC_BROWSE_RESULT, onBrowseOutput )
		ON_BUTTON_CLICK( IDC_TRANSCRIBE, onTranscribe )
		ON_BUTTON_CLICK( IDC_CAPTURE, onCapture )
		COMMAND_HANDLER( IDC_OUTPUT_FORMAT, CBN_SELCHANGE, onOutFormatChange )
		COMMAND_HANDLER( IDC_FILE_LIST, LBN_SELCHANGE, onQueueSelectionChanged )
		MESSAGE_HANDLER( WM_CALLBACK_STATUS, onCallbackStatus )
		MSG_WM_CLOSE( onWmClose )
	END_MSG_MAP()

	BEGIN_DDX_MAP( LoadModelDlg )
		DDX_CONTROL_HANDLE( IDC_MODEL_DESC, modelDesc )
		DDX_CONTROL_HANDLE( IDC_FILE_LIST, fileList )
		DDX_CONTROL_HANDLE( IDC_OUTPUT_FORMAT, transcribeOutFormat )
		DDX_CONTROL_HANDLE( IDC_USE_INPUT_FOLDER, useInputFolder )
		DDX_CONTROL_HANDLE( IDC_PATH_RESULT, transcribeOutputPath )
		DDX_CONTROL_HANDLE( IDC_BROWSE_RESULT, transcribeOutputBrowse );
		DDX_CONTROL_HANDLE( IDC_TRANSCRIBE, transcribeButton );
		DDX_CONTROL_HANDLE( IDC_TRANSCRIBE_PROGRESS, progressBar );
	END_DDX_MAP()

private:
	PendingState pendingState;
	void setPending( bool nowPending );
	void transcribeError( LPCTSTR text, HRESULT hr = S_FALSE );

	LRESULT OnInitDialog( UINT nMessage, WPARAM wParam, LPARAM lParam, BOOL& bHandled );

	void onClose()
	{
		ATLVERIFY( EndDialog( IDCANCEL ) );
	}
	void onBack()
	{
		ATLVERIFY( EndDialog( IDC_BACK ) );
	}

	void printModelDescription();
	CStatic modelDesc;
	ConsoleCheckbox cbConsole;

	LanguageDropdown languageSelector;
	TranslateCheckbox cbTranslate;

	CListBox fileList;
	CButton useInputFolder;
	CEdit transcribeOutputPath;
	CButton transcribeOutputBrowse;
	CComboBox transcribeOutFormat;
	CButton transcribeButton;
	CProgressBarCtrl progressBar;
	void populateOutputFormats();

	LRESULT onOutFormatChange( UINT, INT, HWND, BOOL& bHandled );
	void onInputFolderCheck();
	void onAddFiles();
	void onRemoveFiles();
	void onClearFiles();
	LRESULT onQueueSelectionChanged( UINT, INT, HWND, BOOL& );
	void onBrowseOutput();
	// Despite the name, the method also handles the "Stop" button
	void onTranscribe();
	void onCapture()
	{
		EndDialog( IDC_CAPTURE );
	}

	ThreadPoolWork work;

	// The enum values should match 0-based indices of the combobox items
	enum struct eOutputFormat : uint8_t
	{
		None = 0,
		Text = 1,
		TextTimestamps = 2,
		SubRip = 3,
		WebVTT = 4,
	};

	enum struct eVisualState : uint8_t
	{
		Idle = 0,
		Running = 1,
		Stopping = 2
	};

	enum struct eBatchState : uint8_t
	{
		Pending = 0,
		Running = 1,
		Completed = 2,
		Failed = 3
	};

	struct TranscribeArgs
	{
		CString pathMedia;
		CString pathOutput;
		uint32_t language;
		bool translate;
		eOutputFormat format;
		Whisper::eResultFlags resultFlags;
		volatile eVisualState visualState = (eVisualState)0;
		uint64_t startTime;
		int64_t mediaDuration;
		CString errorMessage;
	};
	TranscribeArgs transcribeArgs;
	CString lastInputPath;

	struct BatchItem
	{
		CString inputPath;
		CString outputPath;
		eBatchState state = eBatchState::Pending;
		HRESULT result = S_OK;
	};
	std::vector<BatchItem> batchItems;
	int runningItem = -1;

	static CString formatStatus( eBatchState state, HRESULT hr );
	void refreshQueueDisplay();
	void updateQueueButtons();
	bool prepareBatchItems( const CString& explicitFolder );
	bool ensureOutputFolder( CString& folder );
	CString composeOutputPath( const CString& input, const CString& explicitFolder ) const;
	bool startNextItem();
	void finalizeCurrentItem( HRESULT hr );
	void finishBatch( bool canceled );

	void __stdcall poolCallback() noexcept override final;

	LRESULT onCallbackStatus( UINT, WPARAM wParam, LPARAM, BOOL& bHandled );

	HRESULT transcribe();
	void getThreadError();
	
	static HRESULT writeTextFile( const Whisper::sSegment* const segments, const size_t length, CAtlFile& file, bool timestamps );
	static HRESULT writeSubRip( const Whisper::sSegment* const segments, const size_t length, CAtlFile& file );
	static HRESULT writeWebVTT( const Whisper::sSegment* const segments, const size_t length, CAtlFile& file );

	static HRESULT __cdecl newSegmentCallbackStatic( Whisper::iContext* ctx, uint32_t n_new, void* user_data ) noexcept;
	static HRESULT __cdecl encoderBeginCallback( Whisper::iContext* ctx, void* user_data ) noexcept;
	HRESULT newSegmentCallback( Whisper::iContext* ctx, uint32_t n_new );

	static HRESULT __cdecl progressCallbackStatic( double p, Whisper::iContext* ctx, void* pv ) noexcept;
	HRESULT progressCallback( double p ) noexcept;

	void onWmClose();
};