#pragma once
#include <windows.h>

#include <string>

#include "HistoryBuffer.h"

// User-editable font configuration (embedded RCDATA font)
// 1) Put your .ttf at CpuHzTray/Fonts/embedded.ttf (project includes it as RCDATA)
// 2) Set this to the font *family name* reported by the PowerShell script.
inline constexpr wchar_t kEmbeddedFontFamilyName[] = L"Determination";

// Layout / text placement tuning (user-editable)
// - Plot height is ratio of tray icon height.
inline constexpr float kPlotHeightRatio = 0.30f;
// - Text is placed using tight glyph bounds, centered in the bottom region.
inline constexpr float kTextBottomMarginPx = 0.0f;
inline constexpr float kTextBottomSafetyPx = 0.0f;
// Single inset from the text RECT edges (fit box). Ink bleed is accounted separately in layout.
inline constexpr int kTextFitInsetPx = 1;
// Faux-bold / grid-fit can extend past path.GetBounds(); reserve this when fitting.
inline constexpr int kTextInkBleedPx = 1;

// Tray icon layout (used by IconRenderer)
inline constexpr int kIconMinSizePx = 16;
inline constexpr int kIconReferenceSizePx = 32;
inline constexpr int kMinPlotHeightPx = 5;
inline constexpr int kMinTextRegionHeightPx = 8;
inline constexpr int kSparklineMaxSamples = 60;
// Samples used for the visible sparkline. The timer records approximately one
// sample per second, so this keeps the vertical scale focused on recent data.
inline constexpr int kSparklineRecentSamples = 10;
inline constexpr int kTrayTextMaxChars = 16;
inline constexpr wchar_t kTrayGhzFormat[] = L"%.2f";

// GDI+ em size candidates for fitting the *current* display string.
inline constexpr float kFontSizeSearchMinEm = 5.0f;
inline constexpr float kFontSizeSearchMaxEm = 400.0f;
inline constexpr float kFontSizeSearchStepEm = 0.5f;

inline constexpr float kSparklineLineWidthMinPx = 1.4f;
inline constexpr float kSparklineLineWidthScale = 1.8f;


struct IconSpec
{
	double ghz = 0;
	double baseMHz = 0;
	bool overBase = false;
	const RingBufferD<30>* historyMHz = nullptr; // optional, oldest->newest
	// If non-null/non-empty, drawn as-is; otherwise ghz is formatted with kTrayGhzFormat.
	const wchar_t* displayText = nullptr;
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

	HICON Render(const IconSpec& spec, UINT dpi = 0) const; // caller owns; dpi=0 → auto-detect

	// Tray icon pixel size for 100% / 125% / 150% (96 / 120 / 144 DPI) and other scales.
	static UINT NormalizeTrayDpi(UINT dpi);
	static int GetTrayIconPixelSizeForDpi(UINT dpi);
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
