#include "stdafx.h"
#include "LoadModelDlg.h"
#include "Asr/SenseVoiceEngine.h"
#include "Utils/miscUtils.h"
#include "Utils/logger.h"
#include "ModelAdvancedDlg.h"

constexpr int progressMaxInteger = 1024 * 8;

namespace
{
	// 在指定目录里找模型文件的完整路径，找不到返回空
	CString findSenseVoiceModel( const CString& dir )
	{
		if( dir.IsEmpty() )
			return CString();
		static const LPCTSTR candidates[] = { L"model.int8.onnx", L"model.onnx", L"model.fp32.onnx" };
		wchar_t buf[ MAX_PATH ] = {};
		for( LPCTSTR name : candidates )
		{
			if( ::PathCombineW( buf, dir, name ) && ::PathFileExistsW( buf ) )
				return CString( buf );
		}
		return CString();
	}

	CString defaultSenseVoiceDir()
	{
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
}

HRESULT LoadModelDlg::show()
{
	auto res = DoModal( nullptr );
	if( res == -1 )
		return HRESULT_FROM_WIN32( GetLastError() );
	if( res == IDOK )
	{
		HRESULT hr = appState.lastScreenLoad();
		switch( hr )
		{
		case SCREEN_TRANSCRIBE:
		case SCREEN_CAPTURE:
			return hr;
		default:
			return SCREEN_TRANSCRIBE;
		}
	}
	return S_OK;
}

LRESULT LoadModelDlg::OnInitDialog( UINT nMessage, WPARAM wParam, LPARAM lParam, BOOL& bHandled )
{
	// First DDX call, hooks up variables to controls.
	DoDataExchange( false );

	cbConsole.initialize( m_hWnd, IDC_CONSOLE, appState );
	implPopulateCombobox( cbModelType, appState.source.impl );
	populateKinds();
	applyKindUI();
	modelPath.SetWindowTextW( appState.source.path );

	HRESULT hr = work.create( this );
	if( FAILED( hr ) )
	{
		CString text = L"CreateThreadpoolWork failed\n";
		text += formatErrorMessage( hr );
		::MessageBox( m_hWnd, text, L"Unable to load the model", MB_OK | MB_ICONWARNING );
		return TRUE;
	}

	editorsWindows.reserve( 6 );
	editorsWindows = { modelPath, cbModelType, GetDlgItem( IDC_BROWSE ), GetDlgItem( IDC_MODEL_ADV ), GetDlgItem( IDOK ), GetDlgItem( IDCANCEL ) };
	pendingWindows.reserve( 2 );
	pendingWindows = { GetDlgItem( IDC_PENDING_TEXT ), progressBar };

	progressBar.SetRange32( 0, progressMaxInteger );
	progressBar.SetStep( 1 );

	appState.setupIcon( this );
	ATLVERIFY( CenterWindow() );
	if( !appState.source.found || !appState.automaticallyLoadModel )
		return 0;

	// AppState.findModelSource() method has located model parameters in registry;
	// Post a notification identical to the "OK" button click event.
	PostMessage( WM_COMMAND, IDOK, (LPARAM)( GetDlgItem( IDOK ).m_hWnd ) );

	return 0;
}

LRESULT LoadModelDlg::OnBrowse( UINT, INT, HWND, BOOL& bHandled )
{
	bHandled = TRUE;

	if( isSenseVoiceKind() )
	{
		// SenseVoice 选的是「模型目录」，不是单个文件
		CString folder;
		modelPath.GetWindowText( folder );
		if( folder.IsEmpty() )
			folder = appState.senseVoiceDir.IsEmpty() ? defaultSenseVoiceDir() : appState.senseVoiceDir;
		if( !browseForFolder( m_hWnd, folder ) )
			return 0;

		// 用户可能选到了模型目录的上层，往下一层看看
		CString found = findSenseVoiceModel( folder );
		if( found.IsEmpty() )
		{
			wchar_t sub[ MAX_PATH ] = {};
			if( ::PathCombineW( sub, folder, L"sense-voice" ) && ::PathFileExistsW( sub ) )
			{
				const CString deeper = findSenseVoiceModel( sub );
				if( !deeper.IsEmpty() )
					folder = sub;
			}
		}

		modelPath.SetWindowText( folder );
		appState.senseVoiceDir = folder;
		return 0;
	}

	CString path;
	modelPath.GetWindowText( path );
	if( !getOpenFileName( m_hWnd, L"Select a GGML Model File", L"Binary files (*.bin)\0*.bin\0\0", path ) )
		return 0;

	modelPath.SetWindowText( path );
	appState.source.path = path;
	return 0;
}

void LoadModelDlg::populateKinds()
{
	cbModelKind.ResetContent();
	// 顺序必须与 eModelKind 一致
	cbModelKind.AddString( L"Whisper (GPU)" );
	cbModelKind.AddString( L"SenseVoice (CPU)" );

	if( 0 == appState.engineId.CompareNoCase( L"sherpa-sensevoice" ) )
		cbModelKind.SetCurSel( (int)eModelKind::SenseVoice );
	else
		cbModelKind.SetCurSel( (int)eModelKind::Whisper );
}

LoadModelDlg::eModelKind LoadModelDlg::selectedKind() const
{
	const int sel = cbModelKind.GetCurSel();
	if( sel == (int)eModelKind::SenseVoice )
		return eModelKind::SenseVoice;
	return eModelKind::Whisper;
}

bool LoadModelDlg::isSenseVoiceKind() const
{
	return selectedKind() == eModelKind::SenseVoice;
}

// 根据当前引擎类型切换界面：文案、路径框内容、以及「模型实现」是否可用
void LoadModelDlg::applyKindUI()
{
	const bool sense = isSenseVoiceKind();

	// Whisper 专属的「模型实现」下拉框与「高级」按钮，在 SenseVoice 模式下没有意义
	cbModelType.EnableWindow( !sense );
	::EnableWindow( GetDlgItem( IDC_MODEL_ADV ), !sense );

	// 切换时把路径框换成对应引擎上次使用的路径
	if( sense )
	{
		CString dir = appState.senseVoiceDir;
		if( dir.IsEmpty() )
			dir = defaultSenseVoiceDir();
		modelPath.SetWindowText( dir );
	}
	else
	{
		modelPath.SetWindowText( appState.source.path );
	}
}

LRESULT LoadModelDlg::onKindChange( UINT, INT, HWND, BOOL& bHandled )
{
	bHandled = TRUE;
	applyKindUI();
	return 0;
}

LRESULT LoadModelDlg::validationError( LPCTSTR message )
{
	reportError( m_hWnd, message, L"Unable to load the model" );
	return 0;
}

LRESULT LoadModelDlg::validationError( LPCTSTR message, HRESULT hr )
{
	reportError( m_hWnd, message, L"Unable to load the model", hr );
	return 0;
}

void LoadModelDlg::setPending( bool nowPending )
{
	const BOOL enable = nowPending ? FALSE : TRUE;
	for( HWND w : editorsWindows )
		::EnableWindow( w, enable );

	const int show = nowPending ? SW_NORMAL : SW_HIDE;
	for( HWND w : pendingWindows )
		::ShowWindow( w, show );

	if( nowPending )
		progressBar.SetMarquee( TRUE, 0 );
	else
		progressBar.SetMarquee( FALSE, 0 );
}

LRESULT LoadModelDlg::OnOk( UINT, INT, HWND, BOOL& bHandled )
{
	modelPath.GetWindowText( path );

	// ---- SenseVoice 分支：只校验目录，不做 Whisper 的 GGML 加载 ----
	if( isSenseVoiceKind() )
	{
		path.Trim();
		if( path.IsEmpty() )
			return validationError( L"请选择 SenseVoice 模型目录" );

		const CString modelFile = findSenseVoiceModel( path );
		if( modelFile.IsEmpty() )
		{
			CString msg = L"该目录下没有找到 SenseVoice 模型文件。\n\n"
				L"需要以下文件：\n"
				L"  model.int8.onnx（或 model.onnx）\n"
				L"  tokens.txt\n\n"
				L"当前目录：\n";
			msg += path;
			return validationError( msg );
		}

		wchar_t tokens[ MAX_PATH ] = {};
		if( !::PathCombineW( tokens, path, L"tokens.txt" ) || !::PathFileExistsW( tokens ) )
		{
			CString msg = L"该目录下缺少 tokens.txt：\n";
			msg += path;
			return validationError( msg );
		}

		// 校验 sherpa 库是否就位，否则进去也用不了
		if( !Asr::SherpaApi::instance().available() )
		{
			CString msg = L"找不到 sherpa-onnx-c-api.dll（或其依赖的 onnxruntime.dll）。\n\n";
			msg += Asr::SherpaApi::instance().lastError();
			msg += L"\n\n请把这些 DLL 放到程序同一目录。";
			return validationError( msg );
		}

		appState.engineId = L"sherpa-sensevoice";
		appState.senseVoiceDir = path;
		appState.saveModelSource();
		EndDialog( IDOK );
		return 0;
	}

	// ---- Whisper 分支：原有逻辑 ----
	if( path.GetLength() <= 0 )
		return validationError( L"Please select a model GGML file" );

	{
		CAtlFile file;
		HRESULT hr = file.Create( path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING );
		if( FAILED( hr ) )
			return validationError( L"Unable to open the model file", hr );

		ULONGLONG cb = 0;
		file.GetSize( cb );
		appState.source.sizeInBytes = cb;
	}

	impl = implGetValue( cbModelType );
	if( impl == (Whisper::eModelImplementation)0 )
		return validationError( L"Please select a model type" );

	setPending( true );
	work.post();
	return 0;
}

static HRESULT loadModel( const wchar_t* path, Whisper::sModelSetup setup, const Whisper::sLoadModelCallbacks* callbacks, Whisper::iModel** pp )
{
	constexpr bool dbgTestClone = false;

	if constexpr( dbgTestClone )
		setup.flags |= (uint32_t)Whisper::eGpuModelFlags::Cloneable;

	CComPtr<Whisper::iModel> res;
	CHECK( Whisper::loadModel( path, setup, callbacks, &res ) );

	if constexpr( dbgTestClone )
	{
		CComPtr<Whisper::iModel> clone;
		CHECK( res->clone( &clone ) );
		*pp = clone.Detach();
		return S_OK;
	}
	else
	{
		*pp = res.Detach();
		return S_OK;
	}
}

void __stdcall LoadModelDlg::poolCallback() noexcept
{
	CComPtr<Whisper::iModel> model;
	clearLastError();
	loadError = L"";
	Whisper::sLoadModelCallbacks lmcb;
	lmcb.cancel = nullptr;
	lmcb.progress = &LoadModelDlg::progressCallback;
	lmcb.pv = this;
	Whisper::sModelSetup setup;
	setup.impl = impl;
	setup.flags = appState.gpuFlagsLoad();
	CString adapter = appState.stringLoad( L"gpu" );
	setup.adapter = adapter;
	HRESULT hr = ::loadModel( path, setup, &lmcb, &model );
	if( SUCCEEDED( hr ) )
		appState.model = model;
	else
		getLastError( loadError );

	this->PostMessage( WM_CALLBACK_STATUS, (WPARAM)hr );
}

HRESULT __stdcall LoadModelDlg::progressCallback( double val, void* pv ) noexcept
{
	LoadModelDlg& dialog = *(LoadModelDlg*)pv;
	constexpr double mul = progressMaxInteger;
	int pos = lround( mul * val );
	dialog.progressBar.PostMessage( PBM_SETPOS, pos, 0 );
	return S_OK;
}

LRESULT LoadModelDlg::OnCallbackStatus( UINT, WPARAM wParam, LPARAM, BOOL& bHandled )
{
	setPending( false );

	bHandled = TRUE;
	const HRESULT hr = (HRESULT)wParam;
	if( FAILED( hr ) )
	{
		LPCTSTR failMessage = L"Error loading the model";
		if( loadError.GetLength() > 0 )
		{
			CString tmp = failMessage;
			tmp += L"\n";
			tmp += loadError;
			return validationError( tmp, hr );
		}
		else
			return validationError( failMessage, hr );
	}

	appState.source.path = path;
	appState.source.impl = impl;
	appState.engineId = L"whisper-gpu";
	appState.saveModelSource();

	EndDialog( IDOK );
	return 0;
}

LRESULT LoadModelDlg::OnHyperlink( int idCtrl, LPNMHDR pnmh, BOOL& bHandled )
{
	const UINT code = pnmh->code;
	switch( code )
	{
	case NM_CLICK:
	case NM_RETURN:
		break;
	default:
		return 0;
	}

	PNMLINK pNMLink = (PNMLINK)pnmh;
	LPCTSTR url = pNMLink->item.szUrl;
	ShellExecute( NULL, L"open", url, NULL, NULL, SW_SHOW );
	bHandled = TRUE;
	return 0;
}

void LoadModelDlg::onModelAdvanced()
{
	ModelAdvancedDlg dlg{ appState };
	dlg.show( m_hWnd );
}