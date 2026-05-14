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

static CpuFrequency g_cpu;
static IconRenderer g_renderer;
static RingBufferD<30> g_historyMHz;

static ULONG_PTR g_gdiplusToken = 0;

static double ToGhz(double mhz) { return mhz / 1000.0; }

static constexpr bool kRedrawEverySampleForSparkline = false;

static void UpdateTrayIcon(HWND hwnd)
{
	auto reading = g_cpu.Read();
	if(reading.ok)
		g_historyMHz.Push(reading.avgMHz);

	// Build tooltip
	std::wstring newTooltip;
	{
		std::wstringstream ss;
		if(reading.ok)
		{
			ss << L"CPU Avg: " << std::fixed << std::setprecision(2) << ToGhz(reading.avgMHz) << L" GHz";
			ss << L"\nCPU Max: " << std::fixed << std::setprecision(2) << ToGhz(reading.maxMHz) << L" GHz";
			if(reading.baseMHz > 0)
				ss << L"\nBase: " << std::fixed << std::setprecision(2) << ToGhz(reading.baseMHz) << L" GHz";
			if(reading.validCoreCount > 0)
				ss << L"\nCores: " << reading.validCoreCount;
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

	// Determine if icon needs redrawing (only when displayed GHz text changes).
	wchar_t displayKey[16]{};
	swprintf_s(displayKey, L"%.2f", ToGhz(reading.ok ? reading.avgMHz : 0.0));

	static std::wstring s_lastDisplayKey;
	bool iconNeedsUpdate = kRedrawEverySampleForSparkline || (displayKey != s_lastDisplayKey);

	if(!iconNeedsUpdate && !tooltipChanged)
		return;

	if(iconNeedsUpdate)
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

		g_nid.hIcon = next;
		s_lastDisplayKey = displayKey;
		wcsncpy_s(g_nid.szTip, newTooltip.c_str(), _TRUNCATE);

		if(Shell_NotifyIconW(NIM_MODIFY, &g_nid))
		{
			SafeDestroyIcon(g_hIcon);
			g_hIcon = next;
		}
		else
		{
			SafeDestroyIcon(next);
		}
	}
	else
	{
		wcsncpy_s(g_nid.szTip, newTooltip.c_str(), _TRUNCATE);
		Shell_NotifyIconW(NIM_MODIFY, &g_nid);
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
	switch(msg)
	{
	case WM_CREATE:
		SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL_MS, nullptr);
		return 0;

	case WM_TIMER:
		if(wParam == TIMER_ID)
			UpdateTrayIcon(hwnd);
		return 0;

	case WM_COMMAND:
		if(LOWORD(wParam) == ID_TRAY_EXIT)
		{
			DestroyWindow(hwnd);
			return 0;
		}
		break;

	case WMAPP_TRAY:
		// Left click: no-op. Right click: context menu.
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

		Shell_NotifyIconW(NIM_DELETE, &g_nid);
		SafeDestroyIcon(g_hIcon);

		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
	// GDI+ is used for sparkline rendering.
	Gdiplus::GdiplusStartupInput gdiplusStartupInput;
	if(Gdiplus::GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, nullptr) != Gdiplus::Ok)
		g_gdiplusToken = 0;

	g_cpu.Initialize();

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
	if(!hwnd) return 1;

	// Add tray icon
	g_nid = {};
	g_nid.cbSize = sizeof(g_nid);
	g_nid.hWnd = hwnd;
	g_nid.uID = 1;
	g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
	g_nid.uCallbackMessage = WMAPP_TRAY;

	// Initial icon
	g_hIcon = g_renderer.Render(IconSpec{});
	if(!g_hIcon)
	{
		auto err = g_renderer.GetFontError();
		if(err && *err)
			MessageBoxW(nullptr, err, L"CpuHzTray - Embedded font error", MB_OK | MB_ICONERROR);
		else
			MessageBoxW(nullptr, L"Failed to render tray icon.", L"CpuHzTray", MB_OK | MB_ICONERROR);
		return 1;
	}
	g_nid.hIcon = g_hIcon;
	wcsncpy_s(g_nid.szTip, L"CPU Hz tray", _TRUNCATE);

	Shell_NotifyIconW(NIM_ADD, &g_nid);

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

	return 0;
}
