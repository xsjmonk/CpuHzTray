#include "IconRenderer.h"
#include "SparklineRenderer.h"
#include "resource.h"


// Embedded font settings (easy to edit)
static const wchar_t* kEmbeddedFontFile = L"Fonts\\embedded.ttf";

#include <windows.h>
#include <wingdi.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <memory>
#include <stdexcept>

#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

// GDI+ needs a PrivateFontCollection to use an embedded font reliably.
// IMPORTANT:
// - Font bytes must remain alive for the whole process lifetime (some fonts access data lazily).
// - Do NOT enumerate FontFamily objects into STL containers.
static bool EnsureEmbeddedFont(Gdiplus::PrivateFontCollection*& outPfc)
{
	static bool inited = false;
	static bool ok = false;
	static Gdiplus::PrivateFontCollection pfc;
	static std::vector<BYTE> fontBytes; // must outlive the process

	if(inited)
	{
		outPfc = ok ? &pfc : nullptr;
		return ok;
	}
	inited = true;

	auto hMod = GetModuleHandleW(nullptr);
	if(!hMod) { ok = false; outPfc = nullptr; return false; }

	HRSRC hrsrc = FindResourceW(hMod, MAKEINTRESOURCEW(IDR_FONT_EMBEDDED), RT_RCDATA);
	if(!hrsrc) { ok = false; outPfc = nullptr; return false; }

	DWORD sz = SizeofResource(hMod, hrsrc);
	if(sz == 0) { ok = false; outPfc = nullptr; return false; }

	HGLOBAL hg = LoadResource(hMod, hrsrc);
	if(!hg) { ok = false; outPfc = nullptr; return false; }

	void* mem = LockResource(hg);
	if(!mem) { ok = false; outPfc = nullptr; return false; }

	fontBytes.resize(sz);
	memcpy(fontBytes.data(), mem, sz);

	pfc.AddMemoryFont(fontBytes.data(), (INT)fontBytes.size());
	ok = (pfc.GetFamilyCount() > 0);
	outPfc = ok ? &pfc : nullptr;
	return ok;
}

void IconRenderer::SetFontError(const std::wstring& msg) const
{
	if(fontError_.empty())
		fontError_ = msg;
}

const wchar_t* IconRenderer::GetFontError() const
{
	return fontError_.empty() ? nullptr : fontError_.c_str();
}
static void FormatText(double ghz, wchar_t out[16])
{
	auto v = std::max(0.0, ghz);
	swprintf_s(out, 16, L"%.2f", v);
}

static HFONT CreateFontForTest(int heightPx, const wchar_t* faceName, int weight)
{
	LOGFONTW lf{};
	lf.lfHeight = -heightPx;
	lf.lfWeight = weight;
	// Use non-antialiased text for pixel fonts and to avoid color fringes on transparent icons.
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
	TEXTMETRICW tm{};
	GetTextMetricsW(hdc, &tm);
	// Add safety for glyph overhang/AA so we never clip the last digit.
	ext.cx += (int)tm.tmOverhang + 1;
	ext.cy += 1;

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

bool IconRenderer::LoadFontFromResource() const
{
	// Load the font bytes from the exe's RCDATA resource.
	auto hMod = GetModuleHandleW(nullptr);
	if(!hMod) return false;

	HRSRC hrsrc = FindResourceW(hMod, MAKEINTRESOURCEW(IDR_FONT_EMBEDDED), RT_RCDATA);
	if(!hrsrc) return false;

	DWORD size = SizeofResource(hMod, hrsrc);
	if(size == 0) return false;

	HGLOBAL hglob = LoadResource(hMod, hrsrc);
	if(!hglob) return false;

	void* data = LockResource(hglob);
	if(!data) return false;

	DWORD nFonts = 0;
	fontMemHandle_ = AddFontMemResourceEx(data, size, nullptr, &nFonts);
	if(!fontMemHandle_) return false;
	return nFonts > 0;
}

void IconRenderer::EnsureInit() const
{
	if(initialized_) return;
	initialized_ = true;
	fontError_.clear();

	// Step 6.1: Explicit validation with explicit errors (no guessing / no fallback).
	// 1) RCDATA exists and non-empty.
	// 2) AddFontMemResourceEx registers at least one font.
	// 3) The configured family name exists in the embedded font.
	// 4) CreateFontIndirectW succeeds for that family name.
	{
		auto hMod = GetModuleHandleW(nullptr);
		if(!hMod) { SetFontError(L"GetModuleHandleW failed."); return; }
		HRSRC hrsrc = FindResourceW(hMod, MAKEINTRESOURCEW(IDR_FONT_EMBEDDED), RT_RCDATA);
		if(!hrsrc) { SetFontError(L"Embedded font RCDATA not found (IDR_FONT_EMBEDDED / RT_RCDATA). Ensure app.rc embeds Fonts\\embedded.ttf as IDR_FONT_EMBEDDED."); return; }
		DWORD sz = SizeofResource(hMod, hrsrc);
		if(sz == 0) { SetFontError(L"Embedded font RCDATA found but size is 0 bytes (IDR_FONT_EMBEDDED). The embedded.ttf may be missing or empty."); return; }
	}

	if(!LoadFontFromResource())
	{
		SetFontError(L"AddFontMemResourceEx failed or returned 0 fonts. Embedded font bytes were loaded but no fonts were registered.");
		return;
	}

	LOGFONTW lf{};
	lf.lfHeight = -44;
	lf.lfWeight = FW_BLACK;
	// Pixel font: keep it crisp (no AA) to avoid fringes and bold-looking strokes.
	lf.lfQuality = NONANTIALIASED_QUALITY;
	lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
	lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	lf.lfPitchAndFamily = DEFAULT_PITCH;	// Embedded font is required. Use the configured family name from header.
	Gdiplus::PrivateFontCollection* pfc = nullptr;
	if(!EnsureEmbeddedFont(pfc) || !pfc)
	{
		SetFontError(L"Embedded font bytes loaded but GDI+ PrivateFontCollection has 0 families. (AddMemoryFont produced no families.)");
		return;
	}
	// Validate the configured family name against the embedded font.
	{
		Gdiplus::FontFamily ff(kEmbeddedFontFamilyName, pfc);
		if(!ff.IsAvailable())
		{
			std::wstring msg = L"Embedded font family name not found inside embedded.ttf. Configured kEmbeddedFontFamilyName='";
			msg += kEmbeddedFontFamilyName;
			msg += L"'. Use font_name.ps1 to list the embedded font family name and update IconRenderer.h.";
			SetFontError(msg);
			return;
		}
	}
	wcscpy_s(lf.lfFaceName, kEmbeddedFontFamilyName);

	hFont_ = CreateFontIndirectW(&lf);

	if(!hFont_)
	{
		// Do not fallback: embedded font must be available (correct family name).
		std::wstring msg = L"CreateFontIndirectW failed for embedded font family name '";
		msg += kEmbeddedFontFamilyName;
		msg += L"'.";
		SetFontError(msg);
		return;
	}
}

HICON IconRenderer::Render(const IconSpec& spec) const
{
	EnsureInit();
	if(!hFont_)
		return nullptr;

	// Use embedded font only. No fallback.
	Gdiplus::PrivateFontCollection* pfc = nullptr;
	if(!EnsureEmbeddedFont(pfc) || !pfc)
	{
		SetFontError(L"Embedded font not available at render time (PrivateFontCollection empty)." );
		return nullptr;
	}

	// Render at native tray size.
	int size = GetSystemMetrics(SM_CXSMICON);
	int sizeY = GetSystemMetrics(SM_CYSMICON);
	if(sizeY > size) size = sizeY;
	if(size <= 0) size = 16;
	if(size < 16) size = 16;

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

	// Draw sparkline first (GDI+ inside SparklineRenderer), then overlay text (GDI+ for correct alpha).
	// Layout: top region is plot-only, bottom region is text-only.
	// 32px icon => bottom ~0.66 for text, top ~0.34 for plot.
		const int splitY = (int)std::lround(size * kPlotHeightRatio);
		int splitYClamped = splitY;
	{
		int minPlot = 4;
		int maxPlot = size - 6; // keep at least 6px for text
		if(splitYClamped < minPlot) splitYClamped = minPlot;
		if(splitYClamped > maxPlot) splitYClamped = maxPlot;
	}

	RECT plotRc{ 0, 0, size, splitYClamped };
	// Bottom region is for text only (as per your current two-section layout).
	RECT textRc{ 0, splitYClamped, size, size };

	if(spec.historyMHz && spec.historyMHz->Count() >= 2)
	{
		double samples[60]{};
		int n = (int)spec.historyMHz->Count();
		if(n > 60) n = 60;
		for(int i = 0; i < n; ++i) samples[i] = spec.historyMHz->GetOldestToNewest((size_t)i);
		SparklineStyle style;
		// Scale stroke width for the actual tray icon size (often 16x16/20x20).
		// Keep it thin but readable.
		const float scale = (float)size / 32.0f;
		style.lineWidth = std::max(1.2f, 1.6f * scale);
		DrawAreaSparklineGdiPlus(hdc, plotRc, samples, n, spec.baseMHz, style);
	}

	// Text color scheme:
	// - Below base: 1475FF
	// - Above base: AF1E2D
	COLORREF rgb = spec.overBase ? spec.textRgbOver : spec.textRgbBelow;

	wchar_t text[16]{};
	FormatText(spec.ghz, text);

	// Text size is cached per (iconSize, embedded font family) to avoid per-tick size jitter.
	// We safely assume the display format is always like "3.71" (4 chars).
	// Cache is computed using a worst-case width sample "8.88" (same length, typically widest digits).
	static int s_cachedIconSize = 0;
	static int s_cachedEm = 0;
	static Gdiplus::RectF s_cachedBounds{};
	static std::wstring s_cachedFamily;


	const auto targetW = (textRc.right - textRc.left);
	const auto targetH = (textRc.bottom - textRc.top);
	// Font sizing for the icon uses GDI+ tight glyph bounds below; do not mix
	// with GDI metrics here (different fonts/rendering modes can disagree).

	// Draw text LAST (top-most). Use GDI+ path so we can measure tight glyph bounds
	// and place the glyph box at the icon bottom with a 1px margin.
	{
		Gdiplus::Graphics g(hdc);
		g.ResetClip();
		g.SetClip(Gdiplus::Rect(0, 0, size, size));
		g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

		// Use embedded font only. No fallback.
		Gdiplus::FontFamily ff(kEmbeddedFontFamilyName, pfc);
		if(!ff.IsAvailable())
		{
			std::wstring msg = L"Embedded font family name not found at render time. Configured kEmbeddedFontFamilyName='";
			msg += kEmbeddedFontFamilyName;
			msg += L"'.";
			SetFontError(msg);
			SelectObject(hdc, oldBmp);
			DeleteDC(hdc);
			DeleteObject(hbm);
			return nullptr;
		}

		auto buildBoundsFor = [&](const wchar_t* t, int em, Gdiplus::RectF& outB)->bool
		{
			Gdiplus::GraphicsPath path;
			Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
			fmt.SetFormatFlags(fmt.GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap);
			path.AddString(t, (INT)wcslen(t), &ff, Gdiplus::FontStyleBold, (Gdiplus::REAL)em, Gdiplus::PointF(0, 0), &fmt);
			outB = Gdiplus::RectF();
			path.GetBounds(&outB);
			return outB.Width <= (float)targetW && outB.Height <= (float)targetH;
		};

		// Compute a stable text size only once per (iconSize, font family).
		// Use a worst-case sample for width so the cached size never exceeds the icon.
		const wchar_t* sampleText = L"8.88";
		// Cache key must include the text box height (because your icon is split into 2 vertical areas).
		static int s_cachedTextH = 0;
		if(s_cachedIconSize != size || s_cachedTextH != targetH || s_cachedFamily != kEmbeddedFontFamilyName || s_cachedEm <= 0)
		{
			int lo = 6, hi = 400, best = 6;
			Gdiplus::RectF bestB{};
			while(lo <= hi)
			{
				int mid = (lo + hi) / 2;
				Gdiplus::RectF b{};
				if(buildBoundsFor(sampleText, mid, b))
				{
					best = mid;
					bestB = b;
					lo = mid + 1;
				}
				else
				{
					hi = mid - 1;
				}
			}
			s_cachedIconSize = size;
			s_cachedTextH = targetH;
			s_cachedFamily = kEmbeddedFontFamilyName;
			s_cachedEm = best;
			s_cachedBounds = bestB;
		}

		int best = s_cachedEm;
		Gdiplus::RectF bestB{};
		// Measure current text bounds at the cached size (cheap) for accurate placement.
		buildBoundsFor(text, best, bestB);

		// Final hinting based on chosen size.
		g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

		// Place using the *tight glyph bounds* within the BOTTOM text region.
		// We center the tight glyph box vertically inside the bottom region to avoid
		// the "missing a couple of pixels" clipping caused by hinting/overhang.
		const float x = (float)textRc.left + ((float)targetW - bestB.Width) / 2.0f - bestB.X;
		const float y = (float)textRc.top + ((float)targetH - bestB.Height) / 2.0f - bestB.Y;
		// Snap to whole pixels to avoid per-frame hinting jitter/jaggies when the text changes.
		const float xs = (float)std::lround(x);
		const float ys = (float)std::lround(y);

		Gdiplus::GraphicsPath path;
		Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
		fmt.SetFormatFlags(fmt.GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap);
		const int style = Gdiplus::FontStyleBold;
		path.AddString(text, (INT)wcslen(text), &ff, style, (Gdiplus::REAL)best, Gdiplus::PointF(0, 0), &fmt);
		Gdiplus::Matrix m;
		m.Translate(xs, ys);
		path.Transform(&m);

		Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(rgb), GetGValue(rgb), GetBValue(rgb)));
		// Use SourceCopy so anti-aliased edge pixels don't blend with underlying plot pixels (prevents bluish/gray tinting).
		g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
		g.FillPath(&brush, &path);
	}
	SelectObject(hdc, oldBmp);
	DeleteDC(hdc);


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
