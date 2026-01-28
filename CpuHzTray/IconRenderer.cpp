#include "IconRenderer.h"

#include "SparklineRenderer.h"

#include <windows.h>
#include <wingdi.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>



static std::wstring GetExeDir()
{
	wchar_t path[MAX_PATH]{};
	auto n = GetModuleFileNameW(nullptr, path, MAX_PATH);
	if(n == 0 || n >= MAX_PATH) return L"";

	std::wstring p(path);
	auto pos = p.find_last_of(L"\\/");
	if(pos == std::wstring::npos) return L"";
	return p.substr(0, pos);
}

static std::vector<unsigned char> ReadAllBytes(const std::wstring& path)
{
	std::vector<unsigned char> data;

	auto h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if(h == INVALID_HANDLE_VALUE) return data;

	LARGE_INTEGER size{};
	if(!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > 50ll * 1024ll * 1024ll)
	{
		CloseHandle(h);
		return data;
	}

	data.resize((size_t)size.QuadPart);
	DWORD read = 0;
	if(!ReadFile(h, data.data(), (DWORD)data.size(), &read, nullptr) || read != (DWORD)data.size())
		data.clear();

	CloseHandle(h);
	return data;
}

static void FormatText(double ghz, wchar_t out[16])
{
	auto v = std::max(0.0, ghz);
	swprintf_s(out, 16, L"%.1f", v);
}

static HFONT CreateFontForTest(int heightPx, const wchar_t* faceName, int weight)
{
	LOGFONTW lf{};
	lf.lfHeight = -heightPx;
	lf.lfWeight = weight;
	lf.lfQuality = NONANTIALIASED_QUALITY;
	lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
	lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	lf.lfPitchAndFamily = DEFAULT_PITCH;
	wcscpy_s(lf.lfFaceName, faceName);
	return CreateFontIndirectW(&lf);
}

static bool MeasureFits(HDC hdc, const wchar_t* text, int heightPx, const wchar_t* faceName, int weight, int maxW, int maxH, SIZE& outExt)
{
	auto hFont = CreateFontForTest(heightPx, faceName, weight);
	if(!hFont) return false;

	auto old = (HFONT)SelectObject(hdc, hFont);

	SIZE ext{};
	GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &ext);

	SelectObject(hdc, old);
	DeleteObject(hFont);

	outExt = ext;
	return ext.cx <= maxW && ext.cy <= maxH;
}

static int FindBestFontHeight(HDC hdc, const wchar_t* text, const wchar_t* faceName, int weight, int minH, int maxH, int maxW, int maxHpx, SIZE& outExt)
{
	SIZE bestExt{};
	int lo = minH;
	int hi = maxH;
	int best = minH;

	while(lo <= hi)
	{
		int mid = (lo + hi) / 2;

		SIZE ext{};
		if(MeasureFits(hdc, text, mid, faceName, weight, maxW, maxHpx, ext))
		{
			best = mid;
			bestExt = ext;
			lo = mid + 1;
		}
		else
		{
			hi = mid - 1;
		}
	}

	outExt = bestExt;
	return best;
}

IconRenderer::IconRenderer()
{
}

IconRenderer::~IconRenderer()
{
	if(hFont_) { DeleteObject(hFont_); hFont_ = nullptr; }
	if(fontMemHandle_) { RemoveFontMemResourceEx(fontMemHandle_); fontMemHandle_ = nullptr; }
}

bool IconRenderer::LoadFontFromFile() const
{
	auto dir = GetExeDir();
	if(dir.empty()) return false;

	auto fontPath = dir + L"\\Fonts\\UniversCnRg.ttf";
	if(GetFileAttributesW(fontPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

	auto bytes = ReadAllBytes(fontPath);
	if(bytes.empty()) return false;

	DWORD nFonts = 0;
	fontMemHandle_ = AddFontMemResourceEx(bytes.data(), (DWORD)bytes.size(), nullptr, &nFonts);
	return fontMemHandle_ != nullptr;
}

void IconRenderer::EnsureInit() const
{
	if(initialized_) return;
	initialized_ = true;

	LoadFontFromFile();

	LOGFONTW lf{};
	lf.lfHeight = -44;
	lf.lfWeight = FW_BLACK;
	lf.lfQuality = NONANTIALIASED_QUALITY;
	lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
	lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	lf.lfPitchAndFamily = DEFAULT_PITCH;

	wcscpy_s(lf.lfFaceName, L"Univers");
	hFont_ = CreateFontIndirectW(&lf);

	if(!hFont_)
	{
		wcscpy_s(lf.lfFaceName, L"Univers LT Std");
		hFont_ = CreateFontIndirectW(&lf);
	}

	if(!hFont_)
	{
		wcscpy_s(lf.lfFaceName, L"Segoe UI");
		lf.lfWeight = FW_BLACK;
		lf.lfQuality = ANTIALIASED_QUALITY;
		hFont_ = CreateFontIndirectW(&lf);
	}
}

HICON IconRenderer::Render(const IconSpec& spec) const
{
	EnsureInit();

	// Render at native tray size.
	const int size = 32;

	BITMAPV5HEADER bi{};
	bi.bV5Size = sizeof(bi);
	bi.bV5Width = size;
	bi.bV5Height = -size;
	bi.bV5Planes = 1;
	bi.bV5BitCount = 32;
	bi.bV5Compression = BI_BITFIELDS;
	bi.bV5RedMask   = 0x00FF0000;
	bi.bV5GreenMask = 0x0000FF00;
	bi.bV5BlueMask  = 0x000000FF;
	bi.bV5AlphaMask = 0xFF000000;

	void* bits = nullptr;
	HDC hdcScreen = GetDC(nullptr);
	HBITMAP hbm = CreateDIBSection(hdcScreen, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	ReleaseDC(nullptr, hdcScreen);
	if(!hbm || !bits) { if(hbm) DeleteObject(hbm); return nullptr; }

	memset(bits, 0, (size_t)size * (size_t)size * 4);

	HDC hdc = CreateCompatibleDC(nullptr);
	auto oldBmp = (HBITMAP)SelectObject(hdc, hbm);

	SetBkMode(hdc, TRANSPARENT);

	// Draw sparkline first (GDI+ inside SparklineRenderer), then overlay text (GDI).
	if(spec.historyMHz && spec.historyMHz->Count() >= 2)
	{
		double samples[60]{};
		int n = (int)spec.historyMHz->Count();
		if(n > 60) n = 60;
		for(int i = 0; i < n; ++i) samples[i] = spec.historyMHz->GetOldestToNewest((size_t)i);

		RECT rc{ 1, 1, size - 1, size - 1 };
		SparklineStyle style;
		DrawAreaSparklineGdiPlus(hdc, rc, samples, n, style);
	}

	auto rgb = spec.overBase ? RGB(230, 40, 40) : RGB(40, 200, 80);
	SetTextColor(hdc, rgb);

	wchar_t text[16]{};
	FormatText(spec.ghz, text);

	LOGFONTW baseLf{};
	GetObjectW(hFont_, sizeof(baseLf), &baseLf);

	// Keep the original "maximize font size within icon" approach.
	const auto targetW = (int)(size * 0.98);
	const auto targetH = (int)(size * 0.92);

	SIZE ext{};
	const int bestH = FindBestFontHeight(hdc, text, baseLf.lfFaceName, FW_BLACK, 6, 30, targetW, targetH, ext);

	auto finalFont = CreateFontForTest(bestH, baseLf.lfFaceName, FW_BLACK);
	auto oldFont = (HFONT)SelectObject(hdc, finalFont);

	GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &ext);

	int x = (size - ext.cx) / 2;
	int y = (size - ext.cy) / 2;

	if(x < 1) x = 1;
	if(y < 1) y = 1;

	// Slight shadow improves readability over the plot.
	SetTextColor(hdc, RGB(0, 0, 0));
	TextOutW(hdc, x + 1, y + 1, text, (int)wcslen(text));
	SetTextColor(hdc, rgb);
	TextOutW(hdc, x, y, text, (int)wcslen(text));

	SelectObject(hdc, oldFont);
	SelectObject(hdc, oldBmp);
	DeleteDC(hdc);

	DeleteObject(finalFont);

	auto* px = (unsigned int*)bits;
	const auto n = (size_t)size * (size_t)size;

// Preserve per-pixel alpha produced by GDI+ (sparkline), but fix up pixels drawn via GDI (text),
// which typically leave alpha = 0. We only force alpha to 0xFF when the pixel has RGB != 0 AND alpha == 0.
	for(size_t i = 0; i < n; i++)
	{
		auto v = px[i];
		if(((v & 0x00FFFFFFu) != 0) && ((v & 0xFF000000u) == 0))
			px[i] = v | 0xFF000000u;
	}
// Build a fully-transparent AND mask (all 1s). This matters on some systems
	// where the mask is still consulted even for 32-bit icons.
	const auto maskStrideBytes = ((size + 31) / 32) * 4;
	std::vector<unsigned char> maskBits((size_t)maskStrideBytes * (size_t)size, 0xFF);
	HBITMAP hbmMask = CreateBitmap(size, size, 1, 1, maskBits.data());
	if(!hbmMask) { DeleteObject(hbm); return nullptr; }

	ICONINFO ii{};
	ii.fIcon = TRUE;
	ii.hbmColor = hbm;
	ii.hbmMask = hbmMask;

	auto hIcon = CreateIconIndirect(&ii);

	DeleteObject(hbmMask);
	DeleteObject(hbm);

	return hIcon;
}