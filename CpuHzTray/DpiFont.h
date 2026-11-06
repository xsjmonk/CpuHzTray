#pragma once

#include <windows.h>

#include <string>

// Selects the embedded font resource, text style, and render settings for a
// display DPI. embedded.ttf is used at 100% DPI; embedded_highdpi.ttf is used
// above 100%.
struct TextRenderSettings
{
	bool antiAlias = false;
	bool renderToArgbBitmap = true;
	int textInkBleedPx = 1;
};

struct TextFontSelection
{
	const void* privateFontCollection = nullptr;
	const wchar_t* familyName = nullptr;
	int style = 0;
	TextRenderSettings rendering{};
};

class DpiFont
{
public:
	DpiFont();
	~DpiFont();

	DpiFont(const DpiFont&) = delete;
	DpiFont& operator=(const DpiFont&) = delete;

	bool Select(UINT dpi, TextFontSelection& outSelection);
	const wchar_t* GetError() const;

	static UINT NormalizeDpi(UINT dpi);
	static int GetTrayIconPixelSizeForDpi(UINT dpi);

private:
	struct Impl;
	bool EnsureInitialized();
	static int ResolveTextStyle(UINT dpi);

	Impl* impl_ = nullptr;
};
