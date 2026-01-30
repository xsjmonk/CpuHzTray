#pragma once
#include <windows.h>

#include <string>

#include "HistoryBuffer.h"

// User-editable font configuration (embedded RCDATA font)
// 1) Put your .ttf at CpuHzTray/Fonts/embedded.ttf (project includes it as RCDATA)
// 2) Set this to the font *family name* reported by the PowerShell script.
inline constexpr wchar_t kEmbeddedFontFamilyName[] = L"Topic";

// Layout / text placement tuning (user-editable)
// - Plot height is ratio of tray icon height.
inline constexpr float kPlotHeightRatio = 0.35f;
// - Text is placed using tight glyph bounds, aligned to bottom.
//   Some fonts paint a couple of pixels beyond their bounds due to hinting;
//   kTextBottomSafetyPx provides a small extra guard.
inline constexpr float kTextBottomMarginPx = 1.0f;
inline constexpr float kTextBottomSafetyPx = 1.0f;


struct IconSpec
{
	double ghz = 0;
	double baseMHz = 0;
	bool overBase = false;
	const RingBufferD<30>* historyMHz = nullptr; // optional, oldest->newest
	// Text colors (explicit variables)
	// Requested scheme:
	// - Above base: AF1E2D
	// - Below base: 1475FF
	COLORREF textRgbNormal = RGB(0xAF, 0x1E, 0x2D);
	COLORREF textRgbOver   = RGB(0xAF, 0x1E, 0x2D);
	COLORREF textRgbBelow  = RGB(0x14, 0x75, 0xFF);
};

class IconRenderer
{
public:
	IconRenderer();
	~IconRenderer();

	HICON Render(const IconSpec& spec) const; // caller owns, must DestroyIcon
	const wchar_t* GetFontError() const;

private:
	void EnsureInit() const;
	bool LoadFontFromResource() const;
	void SetFontError(const std::wstring& msg) const;

	mutable bool initialized_ = false;
	mutable HANDLE fontMemHandle_ = nullptr; // RemoveFontMemResourceEx in dtor
	mutable HFONT hFont_ = nullptr;
	mutable std::wstring fontError_;
};
