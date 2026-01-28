#include "TrayApp.h"
#include "CpuFrequency.h"
#include "IconRenderer.h"

#include <string>
#include <sstream>
#include <iomanip>

static const wchar_t* kWndClass = L"CpuHzTray.HiddenWindow";

static NOTIFYICONDATAW g_nid{};
static HICON g_hIcon = nullptr;

static CpuFrequency g_cpu;
static IconRenderer g_renderer;

static double ToGhz(double mhz) { return mhz / 1000.0; }

static void UpdateTrayIcon(HWND hwnd)
{
	auto reading = g_cpu.Read();

	IconSpec spec{};
	if(reading.ok)
	{
		spec.ghz = ToGhz(reading.currentMHz);
		spec.overBase = (reading.baseMHz > 0) ? (reading.currentMHz > reading.baseMHz) : false;
	}
	else
	{
		spec.ghz = 0;
		spec.overBase = false;
	}

	HICON next = g_renderer.Render(spec);
	if(!next) return;

	// Update tooltip
	std::wstringstream ss;
	ss << L"CPU: ";
	if(reading.ok)
	{
		ss << std::fixed << std::setprecision(2) << ToGhz(reading.currentMHz) << L" GHz";
		if(reading.baseMHz > 0)
			ss << L" (base " << std::fixed << std::setprecision(2) << ToGhz(reading.baseMHz) << L" GHz)";
		ss << L" [" << reading.source << L"]";
	}
	else
	{
		ss << L"--";
	}

	// Replace icon safely (no leaks)
	g_nid.hIcon = next;
	wcsncpy_s(g_nid.szTip, ss.str().c_str(), _TRUNCATE);

	Shell_NotifyIconW(NIM_MODIFY, &g_nid);

	SafeDestroyIcon(g_hIcon);
	g_hIcon = next;
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
	g_hIcon = g_renderer.Render(IconSpec{ 0, false });
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

	return 0;
}
