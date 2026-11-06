#pragma once
#include "iAsrEngine.h"

namespace Asr
{
	// 引擎工厂需要的共享依赖。由调用方（通常是 AppState）在每次创建引擎时填充，
	// 生命周期由调用方保证，工厂不持有引用。
	class AppContext
	{
	public:
		Whisper::iModel* model = nullptr;
		Whisper::iMediaFoundation* mediaFoundation = nullptr;
		// SenseVoice 的设备选择："cpu" / "gpu"，空表示 cpu
		CString senseVoiceDevice;
		// SenseVoice 的模型目录（界面上选过的路径）；空则回退到环境变量 / exe 目录
		CString senseVoiceDir;
	};

	// 按 id 创建引擎实例。每次调用都返回一个全新的、互不共享状态的实例，
	// 这样调度器可以把多个实例丢到不同线程上并行运行。
	// 返回 nullptr 表示该 id 不可用（未注册，或依赖的组件还没准备好）。
	std::unique_ptr<iAsrEngine> createEngine( LPCTSTR id, const AppContext& app );

	// 所有当前可用的引擎描述，用于填充 UI 的下拉框
	std::vector<sAsrEngineInfo> listAvailableEngines( const AppContext& app );

	// 推荐默认使用的引擎 id
	CString defaultEngineId();

	// 是否为 GPU 引擎（调度器据此决定是否允许并发）
	bool isGpuEngine( LPCTSTR id );

	// 不依赖 id 的版本：查引擎列表得知这条条目跑在 GPU 还是 CPU 上。
	// id 里带 "-gpu" 之类的后缀只是约定，判断时以能力描述为准。
	bool isGpuEngineInfo( const std::vector<sAsrEngineInfo>& engines, LPCTSTR id );

	// 同一实现的 CPU 变体 id。例如 sherpa-sensevoice-gpu -> sherpa-sensevoice。
	// 没有对应变体时返回空串。
	CString cpuVariantId( const std::vector<sAsrEngineInfo>& engines, LPCTSTR id );
}
