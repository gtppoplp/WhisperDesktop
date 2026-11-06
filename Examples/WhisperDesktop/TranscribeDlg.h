#pragma once
#include "AppState.h"
#include "Asr/iAsrEngine.h"
#include "Asr/asrEngineRegistry.h"
#include "Utils/WTL/atlddx.h"
#include "Utils/WTL/atlcrack.h"
#include "Utils/miscUtils.h"
#include "Utils/LanguageDropdown.h"
#include "Utils/TranslateCheckbox.h"
#include "Utils/PendingState.h"

class TranscribeDlg :
	public CDialogImpl<TranscribeDlg>,
	public CWinDataExchange<TranscribeDlg>
{
	AppState& appState;

public:
	static constexpr UINT IDD = IDD_TRANSCRIBE_DIALOG;
	static constexpr UINT WM_CALLBACK_STATUS = WM_APP + 1;

	TranscribeDlg( AppState& app ) : appState( app ) { }

	// Show this dialog modally, without parent.
	HRESULT show();

	// 消息处理函数必须在消息映射之前声明
	LRESULT onCallbackStatus( UINT, WPARAM wParam, LPARAM, BOOL& bHandled );
	LRESULT onTimer( UINT, WPARAM wParam, LPARAM, BOOL& bHandled );
	LRESULT onOutFormatChange( UINT, INT, HWND, BOOL& bHandled );
	LRESULT onQueueSelectionChanged( UINT, INT, HWND, BOOL& );
	LRESULT OnInitDialog( UINT nMessage, WPARAM wParam, LPARAM lParam, BOOL& bHandled );

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
		MESSAGE_HANDLER( WM_TIMER, onTimer )
		MSG_WM_CLOSE( onWmClose )
	END_MSG_MAP()

	BEGIN_DDX_MAP( LoadModelDlg )
		DDX_CONTROL_HANDLE( IDC_MODEL_DESC, modelDesc )
		DDX_CONTROL_HANDLE( IDC_FILE_LIST, fileList )
		DDX_CONTROL_HANDLE( IDC_OUTPUT_FORMAT, transcribeOutFormat )
		DDX_CONTROL_HANDLE( IDC_ENGINE, engineSelector )
		DDX_CONTROL_HANDLE( IDC_CONCURRENCY, concurrencyEdit )
		DDX_CONTROL_HANDLE( IDC_HYBRID, hybridCheck )
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
	CComboBox engineSelector;
	CEdit concurrencyEdit;
	CButton hybridCheck;
	// 下拉框每一项对应的引擎 id，与 combobox 索引一一对应
	std::vector<CString> engineIds;
	void populateEngines();
	void saveEngineSelection();
	CString selectedEngineId() const;
	CButton transcribeButton;
	CProgressBarCtrl progressBar;
	void populateOutputFormats();

	void onInputFolderCheck();
	void onAddFiles();
	void onRemoveFiles();
	void onClearFiles();
	void onBrowseOutput();
	// Despite the name, the method also handles the "Stop" button
	void onTranscribe();
	void onCapture()
	{
		// SenseVoice 模式没有 Whisper 模型，捕获界面不可用
		if( nullptr == appState.model )
			return;
		EndDialog( IDC_CAPTURE );
	}

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
		// 注意：并发模式下这些字段只在「启动批次」时设置一次，之后只读。
		uint32_t language = 0;
		bool translate = false;
		eOutputFormat format = eOutputFormat::None;
		Whisper::eResultFlags resultFlags = Whisper::eResultFlags::None;
		volatile eVisualState visualState = (eVisualState)0;
		// 使用哪个引擎；为空时用 Asr::defaultEngineId()
		CString engineId;
		// 每个任务的独立状态都在 BatchItem 里，这个结构体本身启动后只读
	};
	TranscribeArgs transcribeArgs;
	CString lastInputPath;

	struct BatchItem
	{
		CString inputPath;
		CString outputPath;
		volatile eBatchState state = eBatchState::Pending;
		HRESULT result = S_OK;
		// 执行该任务的槽位索引，-1 表示未在运行
		int slot = -1;
		// 该任务自己的错误信息（并发时不能用共享字段）
		CString errorMessage;
		// 该任务开始时间，用于显示耗时
		uint64_t startTime = 0;
	};
	std::vector<BatchItem> batchItems;

	// 一个工作槽：每个槽有自己的线程和引擎实例，互不干扰
	struct WorkerSlot
	{
		HANDLE thread = nullptr;
		// 该槽当前正在处理的队列索引，-1 表示空闲
		volatile int itemIndex = -1;
		// 该槽使用的引擎 id
		CString engineId;
		// 该槽是否使用 GPU（GPU 槽数量由显存预算决定，见 recommendedConcurrency）
		bool gpu = false;
		// 终止信号
		volatile bool quit = false;
		// 进度，0~100，供 UI 轮询显示
		volatile LONG progressPct = 0;
		// 认领该槽的线程 id，0 表示还没被认领。用于让线程找到自己该用哪个槽。
		volatile DWORD threadOwnerTid = 0;
	};
	std::vector<WorkerSlot> slots;
	// 保护 batchItems / 槽位分配的锁
	CComAutoCriticalSection batchCs;

	// 用户填的并发数（0 表示自动）。单引擎模式下是总槽位数上限，
	// 异构模式下只约束 CPU 槽，GPU 槽按显存单独推算。
	int requestedConcurrency = 1;
	// 是否启用异构并行：GPU 引擎与 CPU 引擎同时跑，共享同一个队列
	bool hybridParallel = false;
public:
	// 按引擎能力与当前硬件（逻辑核数 / 可用内存 / 可用显存）推算推荐的并发数。
	// 需要拿到真实的 CPU / 显存数据，所以是公开的静态函数。
	static int recommendedConcurrency( const Asr::sAsrEngineInfo& engineInfo );

private:
	// 规划工作槽：返回每个槽位应使用的引擎 id
	std::vector<CString> planSlots( const CString& engineId, int requested, size_t pendingCount );

	// 由 planSlots 的结果算出每个槽是不是 GPU 槽（按引擎的 execDevice 判断）
	std::vector<bool> resolveGpuSlots( const std::vector<CString>& slotEngines );

	// 把队列里的下一个待办任务分配给空闲槽，返回分配到的槽索引，没有任务时返回 -1
	int dispatchNextInSlot();
	static unsigned __stdcall workerThreadProc( void* pv );
void workerLoop( int slotIndex );
	void finalizeItem( int itemIndex, HRESULT hr, const CString& error, int slotIndex );

	CString formatStatus( eBatchState state, HRESULT hr );
	void refreshQueueDisplay();
	void updateQueueButtons();
	void refreshOutputControls();
	bool prepareBatchItems( const CString& explicitFolder );
	bool ensureOutputFolder( CString& folder );
	CString composeOutputPath( const CString& input, const CString& explicitFolder ) const;
	void finishBatch( bool canceled );

	// 启动 / 停止工作线程池
	// slotEngines 里每一项对应一个工作线程要用的引擎 id，
	// 异构并行时数组里会混有 GPU 引擎和 CPU 引擎。
	// gpuSlots 与它一一对应，标记该槽是不是 GPU 槽（决定任务认领方向和引擎设备）。
	HRESULT startWorkers( const std::vector<CString>& slotEngines, const std::vector<bool>& gpuSlots );
	void stopWorkers();

	// 线程启动参数，避免用「认领」时序来推断槽位
	struct SlotLaunchParam
	{
		TranscribeDlg* dialog;
		int slotIndex;
	};

	// 在一个工作线程里转写一个文件（线程内调用，不外抛）。
	// engine 复用，避免每个文件重复加载模型。
	HRESULT transcribeFileInWorker( Asr::iAsrEngine& engine, const CString& mediaPath, const CString& outputPath, int slotIndex );

	// 用指定引擎完成一次转写。Whisper 走文件流式路径；其它引擎走内存 PCM 路径。
	// 返回后 result 里是完整的、与引擎无关的转写结果。
	HRESULT transcribeWithEngine( Asr::iAsrEngine& engine, LPCTSTR mediaPath, const Asr::sAsrParams& params, Asr::sAsrResult& result );

	static HRESULT writeTextFile( const Asr::sAsrSegment* const segments, const size_t length, CAtlFile& file, bool timestamps );
	static HRESULT writeSubRip( const Asr::sAsrSegment* const segments, const size_t length, CAtlFile& file );
	static HRESULT writeWebVTT( const Asr::sAsrSegment* const segments, const size_t length, CAtlFile& file );

	void onWmClose();
};