#pragma once
#include <windows.h>
#include <shellapi.h>

constexpr UINT WMAPP_TRAY = WM_APP + 1;
constexpr UINT_PTR TIMER_ID = 1;
constexpr UINT TIMER_INTERVAL_MS = 1000;

constexpr UINT ID_TRAY_EXIT = 1001;

inline void SafeDestroyIcon(HICON& h) noexcept
{
	if(h) { DestroyIcon(h); h = nullptr; }
}
