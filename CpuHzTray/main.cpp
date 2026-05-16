#include "TrayApp.h"
#include "CpuFrequency.h"
#include "IconRenderer.h"

#include "HistoryBuffer.h"

// GDI+ relies on COM declarations like IStream.
#include <objidl.h>
#include <gdiplus.h>

#include <string>
#include <sstream>
#include <iomanip>

static const wchar_t* kWndClass = L"CpuHzTray.HiddenWindow";

static NOTIFYICONDATAW g_nid{};
static HICON g_hIcon = nullptr;
static bool g_trayIconAdded = false;
static UINT g_taskbarCreatedMsg = 0;

static CpuFrequency g_cpu;
static IconRenderer g_renderer;
static RingBufferD<30> g_historyMHz;

static ULONG_PTR g_gdiplusToken = 0;

static constexpr GUID kTrayIconGuid =
{ 0x7d8e1a64, 0x7a5e, 0x4d9b, { 0x9d, 0x45, 0x30, 0xa0, 0x48, 0xe8, 0x1f, 0x73 } };

static double ToGhz(double mhz) { return mhz / 1000.0; }

static constexpr bool kRedrawEverySampleForSparkline = false;

struct RedrawDecisionInput
{
	bool displayedTextChanged;
	bool tooltipChanged;
	bool readingOk;
	int previousHistoryCount;
	int currentHistoryCount;
	int samplesSinceIconRedraw;
};

struct RedrawDecision
{
	bool redrawIcon = false;
	bool updateTooltipOnly = false;
};

static RedrawDecision ComputeRedrawDecision(const RedrawDecisionInput& in)
{
	RedrawDecision d;

	// 1. Displayed GHz text changed -> always redraw icon.
	if(in.displayedTextChanged)
	{
		d.redrawIcon = true;
		return d;
	}

	// 2. Sparkline just became drawable (crossed <2 -> >=2 with a valid sample).
	if(in.readingOk && in.previousHistoryCount < 2 && in.currentHistoryCount >= 2)
	{
		d.redrawIcon = true;
		return d;
	}

	// 3. Throttled periodic redraw: redraw every 3 valid samples when sparkline is visible.
	if(in.currentHistoryCount >= 2 && in.samplesSinceIconRedraw >= 3)
	{
		d.redrawIcon = true;
		return d;
	}

	// 4. Tooltip changed without icon redraw conditions.
	if(in.tooltipChanged)
	{
		d.updateTooltipOnly = true;
		return d;
	}

	return d;
}

#ifdef _DEBUG
static bool RunRedrawDecisionTests()
{
	// Case 1: sparkline just became drawable (prev < 2, curr >= 2, readingOk)
	{
		RedrawDecisionInput in{};
		in.displayedTextChanged = false;
		in.tooltipChanged = false;
		in.readingOk = true;
		in.previousHistoryCount = 1;
		in.currentHistoryCount = 2;
		in.samplesSinceIconRedraw = 0;
		auto d = ComputeRedrawDecision(in);
		if(!d.redrawIcon || d.updateTooltipOnly) return false;
	}
	// Case 2: sparkline visible, not enough samples since redraw
	{
		RedrawDecisionInput in{};
		in.displayedTextChanged = false;
		in.tooltipChanged = false;
		in.readingOk = true;
		in.previousHistoryCount = 2;
		in.currentHistoryCount = 3;
		in.samplesSinceIconRedraw = 1;
		auto d = ComputeRedrawDecision(in);
		if(d.redrawIcon || d.updateTooltipOnly) return false;
	}
	// Case 3: throttle reached (>= 3 samples since last redraw)
	{
		RedrawDecisionInput in{};
		in.displayedTextChanged = false;
		in.tooltipChanged = false;
		in.readingOk = true;
		in.previousHistoryCount = 2;
		in.currentHistoryCount = 3;
		in.samplesSinceIconRedraw = 3;
		auto d = ComputeRedrawDecision(in);
		if(!d.redrawIcon || d.updateTooltipOnly) return false;
	}
	// Case 4: displayed text changed
	{
		RedrawDecisionInput in{};
		in.displayedTextChanged = true;
		auto d = ComputeRedrawDecision(in);
		if(!d.redrawIcon) return false;
	}
	// Case 5: tooltip-only when text unchanged and below throttle
	{
		RedrawDecisionInput in{};
		in.displayedTextChanged = false;
		in.tooltipChanged = true;
		in.readingOk = true;
		in.previousHistoryCount = 2;
		in.currentHistoryCount = 2;
		in.samplesSinceIconRedraw = 1;
		auto d = ComputeRedrawDecision(in);
		if(d.redrawIcon || !d.updateTooltipOnly) return false;
	}
	// Case 6: readingOk=false should not force sparkline redraw
	{
		RedrawDecisionInput in{};
		in.displayedTextChanged = false;
		in.tooltipChanged = false;
		in.readingOk = false;
		in.previousHistoryCount = 1;
		in.currentHistoryCount = 1;
		in.samplesSinceIconRedraw = 0;
		auto d = ComputeRedrawDecision(in);
		if(d.redrawIcon || d.updateTooltipOnly) return false;
	}
	// Edge: readingOk=false, prev < 2, curr >= 2 should NOT trigger sparkline redraw
	{
		RedrawDecisionInput in{};
		in.displayedTextChanged = false;
		in.tooltipChanged = false;
		in.readingOk = false;
		in.previousHistoryCount = 1;
		in.currentHistoryCount = 2;
		in.samplesSinceIconRedraw = 0;
		auto d = ComputeRedrawDecision(in);
		if(d.redrawIcon || d.updateTooltipOnly) return false;
	}
	return true;
}
#endif

static void FillTrayIconIdentity(NOTIFYICONDATAW& nid, HWND hwnd) noexcept
{
	nid.hWnd = hwnd;
	nid.uID = 1;
	nid.uFlags |= NIF_GUID;
	nid.guidItem = kTrayIconGuid;
}

#ifdef _DEBUG
static bool RunTrayIdentityTests()
{
	NOTIFYICONDATAW nid{};
	nid.cbSize = sizeof(nid);
	FillTrayIconIdentity(nid, reinterpret_cast<HWND>(static_cast<UINT_PTR>(0x1234)));
	if(nid.hWnd != reinterpret_cast<HWND>(static_cast<UINT_PTR>(0x1234))) return false;
	if(nid.uID != 1) return false;
	if((nid.uFlags & NIF_GUID) == 0) return false;
	if(!IsEqualGUID(nid.guidItem, kTrayIconGuid)) return false;
	return true;
}
#endif

static void DeleteTrayIconByIdentity(HWND hwnd) noexcept
{
	if(!hwnd) return;
	NOTIFYICONDATAW nid{};
	nid.cbSize = sizeof(nid);
	FillTrayIconIdentity(nid, hwnd);
	Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void RemoveTrayIcon()
{
	if(g_trayIconAdded)
	{
		DeleteTrayIconByIdentity(g_nid.hWnd);
		g_trayIconAdded = false;
	}
}

static const wchar_t* EffectiveTooltip(const wchar_t* tooltip) noexcept
{
	return (tooltip && *tooltip) ? tooltip : L"CPU Hz tray";
}

static bool AddTrayIcon(HWND hwnd, HICON icon, const wchar_t* tooltip)
{
	if(!hwnd || !icon) return false;

	DeleteTrayIconByIdentity(hwnd);

	g_trayIconAdded = false;

	NOTIFYICONDATAW nid{};
	nid.cbSize = sizeof(nid);
	FillTrayIconIdentity(nid, hwnd);
	nid.uFlags |= NIF_MESSAGE | NIF_ICON | NIF_TIP;
	nid.uCallbackMessage = WMAPP_TRAY;
	nid.hIcon = icon;
	wcsncpy_s(nid.szTip, EffectiveTooltip(tooltip), _TRUNCATE);

	if(!Shell_NotifyIconW(NIM_ADD, &nid))
		return false;

	g_trayIconAdded = true;
	g_nid = nid;
	g_nid.uVersion = NOTIFYICON_VERSION_4;
	Shell_NotifyIconW(NIM_SETVERSION, &g_nid);

	return true;
}

static bool ModifyTrayIcon(HWND hwnd, HICON icon, const wchar_t* tooltip)
{
	if(!g_trayIconAdded)
		return AddTrayIcon(hwnd, icon, tooltip);

	if(!hwnd || !icon) return false;

	NOTIFYICONDATAW nid = g_nid;
	nid.cbSize = sizeof(nid);
	FillTrayIconIdentity(nid, hwnd);
	nid.uFlags |= NIF_MESSAGE | NIF_ICON | NIF_TIP;
	nid.uCallbackMessage = WMAPP_TRAY;
	nid.hIcon = icon;
	wcsncpy_s(nid.szTip, EffectiveTooltip(tooltip), _TRUNCATE);

	if(Shell_NotifyIconW(NIM_MODIFY, &nid))
	{
		g_nid = nid;
		return true;
	}

	DeleteTrayIconByIdentity(hwnd);
	g_trayIconAdded = false;
	return AddTrayIcon(hwnd, icon, tooltip);
}

static void UpdateTrayIcon(HWND hwnd)
{
	auto reading = g_cpu.Read();

	// Push history and track throttle counter.
	static int s_samplesSinceIconRedraw = 0;
	if(reading.ok)
	{
		g_historyMHz.Push(reading.avgMHz);
		++s_samplesSinceIconRedraw;
	}

	// Build tooltip
	std::wstring newTooltip;
	{
		std::wstringstream ss;
		if(reading.ok)
		{
			ss << L"CPU Avg: " << std::fixed << std::setprecision(2) << ToGhz(reading.avgMHz) << L" GHz";
			ss << L"\nCPU Max: " << std::fixed << std::setprecision(2) << ToGhz(reading.maxMHz) << L" GHz";
			ss << L"\nCPU Min: " << std::fixed << std::setprecision(2) << ToGhz(reading.minMHz) << L" GHz";
			if(reading.baseMHz > 0)
				ss << L"\nBase: " << std::fixed << std::setprecision(2) << ToGhz(reading.baseMHz) << L" GHz";
			if(reading.validCoreCount > 0)
				ss << L"\nCores: " << reading.validCoreCount;
			if(!reading.accuracy.empty())
				ss << L"\nAccuracy: " << reading.accuracy;
			if(!reading.warning.empty())
				ss << L"\nWarning: " << reading.warning;
			ss << L"\nSource: " << reading.source;
		}
		else
		{
			ss << L"CPU: --";
			if(!reading.source.empty())
				ss << L"\nSource: " << reading.source;
		}
		newTooltip = ss.str();
	}

	bool tooltipChanged = (newTooltip != g_nid.szTip);

	// Track display key change.
	wchar_t displayKey[16]{};
	swprintf_s(displayKey, L"%.2f", ToGhz(reading.ok ? reading.avgMHz : 0.0));

	static std::wstring s_lastDisplayKey;
	bool displayKeyChanged = (displayKey != s_lastDisplayKey);

	// Sparkline drawability tracking.
	static int s_prevHistoryCount = 0;
	int currHistoryCount = (int)g_historyMHz.Count();

	// Compute redraw decision.
	RedrawDecisionInput in{};
	in.displayedTextChanged = displayKeyChanged;
	in.tooltipChanged = tooltipChanged;
	in.readingOk = reading.ok;
	in.previousHistoryCount = s_prevHistoryCount;
	in.currentHistoryCount = currHistoryCount;
	in.samplesSinceIconRedraw = s_samplesSinceIconRedraw;

	auto decision = ComputeRedrawDecision(in);
	s_prevHistoryCount = currHistoryCount;

	if(!decision.redrawIcon && !decision.updateTooltipOnly)
		return;

	if(decision.redrawIcon)
	{
		IconSpec spec{};
		if(reading.ok)
		{
			spec.ghz = ToGhz(reading.avgMHz);
			spec.baseMHz = reading.baseMHz;
			spec.overBase = (reading.baseMHz > 0) ? (reading.avgMHz > reading.baseMHz) : false;
			spec.historyMHz = &g_historyMHz;
		}
		else
		{
			spec.ghz = 0;
			spec.baseMHz = 0;
			spec.overBase = false;
			spec.historyMHz = nullptr;
		}

		HICON next = g_renderer.Render(spec);
		if(!next)
		{
			static bool s_shown = false;
			if(!s_shown)
			{
				s_shown = true;
				auto err = g_renderer.GetFontError();
				if(err && *err)
				{
					MessageBoxW(hwnd, err, L"CpuHzTray - Embedded font error", MB_OK | MB_ICONERROR);
					DestroyWindow(hwnd);
				}
			}
			return;
		}

		s_lastDisplayKey = displayKey;

		if(ModifyTrayIcon(hwnd, next, newTooltip.c_str()))
		{
			SafeDestroyIcon(g_hIcon);
			g_hIcon = next;
			s_samplesSinceIconRedraw = 0;
		}
		else
		{
			SafeDestroyIcon(next);
		}
	}
	else
	{
		ModifyTrayIcon(hwnd, g_hIcon, newTooltip.c_str());
	}
}

static void ShowContextMenu(HWND hwnd, POINT pt)
{
	HMENU menu = CreatePopupMenu();
	if(!menu) return;

	AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");

	SetForegroundWindow(hwnd);
	TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
	DestroyMenu(menu);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if(g_taskbarCreatedMsg != 0 && msg == g_taskbarCreatedMsg)
	{
		g_trayIconAdded = false;
		if(g_hIcon)
			AddTrayIcon(hwnd, g_hIcon, g_nid.szTip[0] ? g_nid.szTip : L"CPU Hz tray");
		UpdateTrayIcon(hwnd);
		return 0;
	}

	switch(msg)
	{
	case WM_CREATE:
		SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL_MS, nullptr);
		return 0;

	case WM_TIMER:
		if(wParam == TIMER_ID)
			UpdateTrayIcon(hwnd);
		return 0;

	case WM_QUERYENDSESSION:
		RemoveTrayIcon();
		return TRUE;

	case WM_ENDSESSION:
		if(wParam)
			RemoveTrayIcon();
		return 0;

	case WM_COMMAND:
		if(LOWORD(wParam) == ID_TRAY_EXIT)
		{
			DestroyWindow(hwnd);
			return 0;
		}
		break;

	case WMAPP_TRAY:
		if(LOWORD(lParam) == WM_RBUTTONUP)
		{
			POINT pt;
			GetCursorPos(&pt);
			ShowContextMenu(hwnd, pt);
			return 0;
		}
		return 0;

	case WM_DESTROY:
		KillTimer(hwnd, TIMER_ID);
		RemoveTrayIcon();
		SafeDestroyIcon(g_hIcon);
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
	HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"Local\\CpuHzTray.SingleInstance");
	if(!hMutex) return 1;
	if(GetLastError() == ERROR_ALREADY_EXISTS)
	{
		CloseHandle(hMutex);
		return 0;
	}

	// Parse --diagnose-hz flag
	{
		int argc = 0;
		auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
		if(argv)
		{
			for(int i = 0; i < argc; ++i)
			{
				if(_wcsicmp(argv[i], L"--diagnose-hz") == 0)
				{
					g_cpu.EnableDiagnosticLogger(L"candidate_readings.csv");
					break;
				}
			}
			LocalFree(argv);
		}
	}
	// GDI+ is used for sparkline rendering.
	Gdiplus::GdiplusStartupInput gdiplusStartupInput;
	if(Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr) != Gdiplus::Ok)
		g_gdiplusToken = 0;

	g_cpu.Initialize();

#ifdef _DEBUG
	if(!RunRedrawDecisionTests())
	{
		MessageBoxW(nullptr, L"RedrawDecision self-tests failed.", L"CpuHzTray", MB_OK | MB_ICONERROR);
		CloseHandle(hMutex);
		return 1;
	}
	if(!RunTrayIdentityTests())
	{
		MessageBoxW(nullptr, L"Tray identity self-tests failed.", L"CpuHzTray", MB_OK | MB_ICONERROR);
		CloseHandle(hMutex);
		return 1;
	}
#endif

	g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInstance;
	wc.lpszClassName = kWndClass;
	RegisterClassExW(&wc);

	HWND hwnd = CreateWindowExW(
		0, kWndClass, L"CpuHzTray", 0,
		0, 0, 0, 0,
		nullptr, nullptr, hInstance, nullptr
	);
	if(!hwnd) { CloseHandle(hMutex); return 1; }

	// Initial icon
	g_hIcon = g_renderer.Render(IconSpec{});
	if(!g_hIcon)
	{
		auto err = g_renderer.GetFontError();
		if(err && *err)
			MessageBoxW(nullptr, err, L"CpuHzTray - Embedded font error", MB_OK | MB_ICONERROR);
		else
			MessageBoxW(nullptr, L"Failed to render tray icon.", L"CpuHzTray", MB_OK | MB_ICONERROR);
		DestroyWindow(hwnd);
		if(g_gdiplusToken) Gdiplus::GdiplusShutdown(g_gdiplusToken);
		CloseHandle(hMutex);
		return 1;
	}

	if(!AddTrayIcon(hwnd, g_hIcon, L"CPU Hz tray"))
	{
		SafeDestroyIcon(g_hIcon);
		DestroyWindow(hwnd);
		if(g_gdiplusToken) Gdiplus::GdiplusShutdown(g_gdiplusToken);
		CloseHandle(hMutex);
		return 1;
	}

	// First update immediately
	UpdateTrayIcon(hwnd);

	MSG m;
	while(GetMessageW(&m, nullptr, 0, 0))
	{
		TranslateMessage(&m);
		DispatchMessageW(&m);
	}

	if(g_gdiplusToken)
		Gdiplus::GdiplusShutdown(g_gdiplusToken);

	CloseHandle(hMutex);
	return 0;
}
