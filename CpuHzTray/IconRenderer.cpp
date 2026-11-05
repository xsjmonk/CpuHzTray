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
#include <limits>

#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

/*
Text-rendering maintenance rules:
  1. Measure and render with the same per-character GraphicsPath::AddString
     call, font family, style, and em size. Do not mix GDI text metrics,
     Graphics::MeasureString, or a hard-coded sample string with this path.
  2. Choose the em size from the actual display text. The vertical tight
     bounds determine the candidate that fits; horizontal compression is
     applied only after that candidate has been selected.
  3. Pixel-grid snapping is a placement/rasterization step only. It must not
     be included in the size search or be allowed to reduce the chosen em.
  4. BuildPixelSnappedTextPath returns a path already positioned in icon
     coordinates. It applies scale and translation exactly once. The caller
     must fill that path directly and must not transform it again.
*/

// Measure and draw through the same per-character GraphicsPath geometry.
// This deliberately does not use Graphics::MeasureString: that API includes
// line/layout metrics that vary significantly between font families.
static int ResolveTrayFontStyle(const Gdiplus::FontFamily&)
{
	// Beautiful Police Officer contains a regular face only. Request Bold
	// explicitly so GDI+ applies its bold style instead of silently selecting
	// the regular face. If a future embedded font contains a native bold face,
	// the same request selects that face.
	return Gdiplus::FontStyleBold;
}

struct TrayTextLayout
{
	float em = kFontSizeSearchMinEm;
	int characterCount = 0;
	float characterCellWidth = 0.0f;
	float horizontalScale = 1.0f;
	float originX = 0.0f;
	float originY = 0.0f;
	float remainingHeight = std::numeric_limits<float>::max();
};

struct TrayGlyphMeasurement
{
	Gdiplus::RectF bounds{};
	bool visible = false;
};

static bool MeasurePerCharacterText(
	Gdiplus::FontFamily& ff,
	const wchar_t* text,
	int textLen,
	float em,
	int style,
	TrayGlyphMeasurement* glyphs,
	int glyphCapacity,
	float& outCellWidth,
	Gdiplus::RectF& outBounds)
{
	if(textLen <= 0 || textLen > glyphCapacity)
		return false;

	Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
	fmt.SetFormatFlags(fmt.GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap);
	outCellWidth = 0.0f;
	float top = std::numeric_limits<float>::max();
	float bottom = std::numeric_limits<float>::lowest();
	bool hasVisibleGlyph = false;

	for(int i = 0; i < textLen; ++i)
	{
		Gdiplus::GraphicsPath glyph;
		const auto status = glyph.AddString(
			&text[i],
			1,
			&ff,
			style,
			(Gdiplus::REAL)em,
			Gdiplus::PointF(0.0f, 0.0f),
			&fmt);
		if(status != Gdiplus::Ok)
			return false;

		Gdiplus::RectF glyphBounds{};
		glyph.GetBounds(&glyphBounds);
		glyphs[i].bounds = glyphBounds;
		glyphs[i].visible = glyphBounds.Width > 0.0f && glyphBounds.Height > 0.0f;
		if(glyphBounds.Width <= 0.0f || glyphBounds.Height <= 0.0f)
			continue;

		outCellWidth = std::max(outCellWidth, glyphBounds.Width);
		top = std::min(top, glyphBounds.Y);
		bottom = std::max(bottom, glyphBounds.Y + glyphBounds.Height);
		hasVisibleGlyph = true;
	}

	if(!hasVisibleGlyph)
		return false;

	outBounds = Gdiplus::RectF(
		0.0f,
		top,
		outCellWidth * (float)textLen,
		bottom - top);
	return outBounds.Width > 0.0f && outBounds.Height > 0.0f;
}

static bool BuildPixelSnappedTextPath(
	Gdiplus::FontFamily& ff,
	const wchar_t* text,
	int textLen,
	float em,
	int style,
	const TrayTextLayout& layout,
	Gdiplus::GraphicsPath& outPath)
{
	// The returned path is in final icon coordinates. Keep all per-glyph
	// transforms here so measurement, snapping, and drawing have one owner.
	// Do not add another transform at the call site.
	TrayGlyphMeasurement glyphs[kTrayTextMaxChars]{};
	float measuredCellWidth = 0.0f;
	Gdiplus::RectF measuredBounds{};
	if(!MeasurePerCharacterText(
		ff,
		text,
		textLen,
		em,
		style,
		glyphs,
		kTrayTextMaxChars,
		measuredCellWidth,
		measuredBounds))
	{
		return false;
	}

	Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
	fmt.SetFormatFlags(fmt.GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap);
	outPath.Reset();

	for(int i = 0; i < textLen; ++i)
	{
		if(!glyphs[i].visible)
			continue;

		Gdiplus::GraphicsPath glyph;
		if(glyph.AddString(
			&text[i],
			1,
			&ff,
			style,
			(Gdiplus::REAL)em,
			Gdiplus::PointF(0.0f, 0.0f),
			&fmt) != Gdiplus::Ok)
		{
			return false;
		}

		const float cellLeft = layout.originX
			+ (float)i * layout.characterCellWidth * layout.horizontalScale;
		const float cellRight = layout.originX
			+ (float)(i + 1) * layout.characterCellWidth * layout.horizontalScale;
		const float snappedCellLeft = (float)std::lround(cellLeft);
		const float snappedCellRight = (float)std::lround(cellRight);
		const float snappedCellWidth = std::max(1.0f, snappedCellRight - snappedCellLeft);
		const float scaledGlyphWidth = glyphs[i].bounds.Width * layout.horizontalScale;

		// Snap the visible left edge of each glyph. The baseline/origin Y is
		// shared so all digits remain vertically aligned.
		const float desiredVisibleLeft = snappedCellLeft
			+ (snappedCellWidth - scaledGlyphWidth) / 2.0f;
		const float snappedVisibleLeft = static_cast<float>(std::lround(desiredVisibleLeft));
		const float glyphOriginX = snappedVisibleLeft
			- glyphs[i].bounds.X * layout.horizontalScale;

		Gdiplus::Matrix glyphTransform(
			layout.horizontalScale,
			0.0f,
			0.0f,
			1.0f,
			glyphOriginX,
			layout.originY);
		glyph.Transform(&glyphTransform);
		outPath.AddPath(&glyph, FALSE);
	}

	return true;
}

static bool TryPerCharacterLayout(
	Gdiplus::FontFamily& ff,
	const wchar_t* text,
	int textLen,
	float em,
	int style,
	float regionLeft,
	float regionTop,
	float regionRight,
	float regionBottom,
	TrayTextLayout& out)
{
	TrayGlyphMeasurement glyphs[kTrayTextMaxChars]{};
	float characterCellWidth = 0.0f;
	Gdiplus::RectF bounds{};
	if(!MeasurePerCharacterText(
		ff,
		text,
		textLen,
		em,
		style,
		glyphs,
		kTrayTextMaxChars,
		characterCellWidth,
		bounds))
		return false;

	const float regionWidth = regionRight - regionLeft;
	const float regionHeight = regionBottom - regionTop;
	const float bleed = (float)kTextInkBleedPx;
	const float usableWidth = regionWidth - 2.0f * bleed;
	const float usableHeight = regionHeight - 2.0f * bleed;
	if(usableWidth <= 0.0f || usableHeight <= 0.0f)
		return false;
	if(bounds.Height > usableHeight)
		return false;

	// Preserve the font's normal width when possible. If the actual summed
	// glyph width is too large, compress only horizontally so vertical size
	// can still use the available block height.
	const float naturalWidth = characterCellWidth * (float)textLen;
	const float horizontalScale = std::min(1.0f, usableWidth / naturalWidth);
	const float scaledWidth = naturalWidth * horizontalScale;
	const float contentLeft = regionLeft + (regionWidth - scaledWidth) / 2.0f;
	const float centeredY = regionTop + (regionHeight - bounds.Height) / 2.0f - bounds.Y;
	float originX = static_cast<float>(std::lround(contentLeft));
	float originY = static_cast<float>(std::lround(centeredY));

	auto fits = [&](float candidateX, float candidateY, bool snappedX) -> bool
	{
		const float left = (snappedX ? static_cast<float>(std::lround(candidateX)) : candidateX) - bleed;
		const float right = (snappedX
			? static_cast<float>(std::lround(candidateX + scaledWidth))
			: candidateX + scaledWidth) + bleed;
		const float top = candidateY + bounds.Y - bleed;
		const float bottom = candidateY + bounds.Y + bounds.Height + bleed;
		return left >= regionLeft
			&& top >= regionTop
			&& right <= regionRight
			&& bottom <= regionBottom;
	};

	// Prefer integer placement for crisp rasterization. If no integer origin
	// fits the exact bounds, retain the precise calculated origin. Pixel
	// snapping must never reduce the selected font size.
	if(!fits(originX, originY, true))
	{
		originX = contentLeft;
		originY = centeredY;
	}

	out.em = em;
	out.characterCount = textLen;
	out.characterCellWidth = characterCellWidth;
	out.horizontalScale = horizontalScale;
	out.originX = originX;
	out.originY = originY;
	out.remainingHeight = usableHeight - bounds.Height;
	return true;
}

static TrayTextLayout FindMaxTrayTextLayout(
	Gdiplus::FontFamily& ff,
	const wchar_t* text,
	int textLen,
	int style,
	float regionLeft,
	float regionTop,
	float regionRight,
	float regionBottom)
{
	TrayTextLayout best{};
	bool found = false;
	const int candidateCount = static_cast<int>(
		std::floor(
			(kFontSizeSearchMaxEm - kFontSizeSearchMinEm)
			/ kFontSizeSearchStepEm));

	// Test every half-em candidate. Keep this exhaustive candidate list:
	// glyph bounds and hinting are font-specific, and a binary search can
	// skip the best candidate when the fit predicate is not perfectly
	// monotonic. Width is handled independently by horizontalScale, so the
	// largest visible height is selected first.
	for(int candidateIndex = 0; candidateIndex <= candidateCount; ++candidateIndex)
	{
		const float em = kFontSizeSearchMinEm
			+ (float)candidateIndex * kFontSizeSearchStepEm;
		TrayTextLayout candidate{};
		if(TryPerCharacterLayout(
			ff, text, textLen, em, style,
			regionLeft, regionTop, regionRight, regionBottom,
			candidate))
		{
			if(!found
				|| candidate.remainingHeight < best.remainingHeight
				|| (candidate.remainingHeight == best.remainingHeight && candidate.em > best.em))
			{
				best = candidate;
				found = true;
			}
		}
	}

	if(!found)
	{
		TryPerCharacterLayout(
			ff,
			text,
			textLen,
			kFontSizeSearchMinEm,
			style,
			regionLeft,
			regionTop,
			regionRight,
			regionBottom,
			best);
	}

	return best;
}

UINT IconRenderer::NormalizeTrayDpi(UINT dpi)
{
	if(dpi == 0)
		dpi = 96;
	if(dpi <= 102)
		return 96;
	if(dpi <= 132)
		return 120;
	if(dpi <= 156)
		return 144;
	return dpi;
}

int IconRenderer::GetTrayIconPixelSizeForDpi(UINT dpi)
{
	dpi = NormalizeTrayDpi(dpi);
	HMODULE user32 = GetModuleHandleW(L"user32.dll");
	if(user32)
	{
		using GetSystemMetricsForDpiFn = int(WINAPI*)(int, UINT);
		auto fn = (GetSystemMetricsForDpiFn)GetProcAddress(user32, "GetSystemMetricsForDpi");
		if(fn)
		{
			const int cx = fn(SM_CXSMICON, dpi);
			const int cy = fn(SM_CYSMICON, dpi);
			return std::max({cx, cy, kIconMinSizePx});
		}
	}
	return std::max(MulDiv(16, (int)dpi, 96), kIconMinSizePx);
}

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
static void FormatGhzText(double ghz, wchar_t out[kTrayTextMaxChars])
{
	auto v = std::max(0.0, ghz);
	swprintf_s(out, kTrayTextMaxChars, kTrayGhzFormat, v);
}

static void ResolveDisplayText(const IconSpec& spec, wchar_t out[kTrayTextMaxChars])
{
	if(spec.displayText && spec.displayText[0] != L'\0')
	{
		wcsncpy_s(out, kTrayTextMaxChars, spec.displayText, _TRUNCATE);
		return;
	}
	FormatGhzText(spec.ghz, out);
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

HICON IconRenderer::Render(const IconSpec& spec, UINT dpiHint) const
{
	EnsureInit();
	if(!hFont_)
		return nullptr;

	const UINT dpi = IconRenderer::NormalizeTrayDpi(dpiHint != 0 ? dpiHint : [&]() -> UINT
	{
		HMODULE user32 = GetModuleHandleW(L"user32.dll");
		if(user32)
		{
			using GetDpiForSystemFn = UINT(WINAPI*)();
			auto fn = (GetDpiForSystemFn)GetProcAddress(user32, "GetDpiForSystem");
			if(fn)
				return fn();
		}
		HDC hdcDpi = GetDC(nullptr);
		UINT fallback = (UINT)GetDeviceCaps(hdcDpi, LOGPIXELSX);
		ReleaseDC(nullptr, hdcDpi);
		return fallback;
	}());

	// Use embedded font only. No fallback.
	Gdiplus::PrivateFontCollection* pfc = nullptr;
	if(!EnsureEmbeddedFont(pfc) || !pfc)
	{
		SetFontError(L"Embedded font not available at render time (PrivateFontCollection empty)." );
		return nullptr;
	}

	int size = GetTrayIconPixelSizeForDpi(dpi);
	if(size <= 0) size = kIconMinSizePx;
	if(size < kIconMinSizePx) size = kIconMinSizePx;

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
		int minPlot = kMinPlotHeightPx;
		int minTextH = std::max(kMinTextRegionHeightPx, size / 4);
		int maxPlot = size - minTextH;
		if(splitYClamped < minPlot) splitYClamped = minPlot;
		if(splitYClamped > maxPlot) splitYClamped = maxPlot;
	}

	RECT plotRc{ 0, 0, size, splitYClamped };
	// Bottom region is for text only (as per your current two-section layout).
	RECT textRc{ 0, splitYClamped, size, size };

	if(spec.historyMHz && spec.historyMHz->Count() >= 2)
	{
		double samples[kSparklineMaxSamples]{};
		const int historyCount = (int)spec.historyMHz->Count();
		const int n = std::min({
			historyCount,
			kSparklineRecentSamples,
			kSparklineMaxSamples});
		const int firstRecentIndex = historyCount - n;
		for(int i = 0; i < n; ++i)
		{
			samples[i] = spec.historyMHz->GetOldestToNewest(
				(size_t)(firstRecentIndex + i));
		}
		SparklineStyle style;
		// Scale stroke width for the actual tray icon size (often 16x16/20x20).
		// Keep it thin but readable.
		const float scale = (float)size / (float)kIconReferenceSizePx;
		style.lineWidth = std::max(kSparklineLineWidthMinPx, kSparklineLineWidthScale * scale);
		DrawAreaSparklineGdiPlus(hdc, plotRc, samples, n, spec.baseMHz, style);
	}

	// Text color scheme:
	// - Below base: 1475FF
	// - Above base: AF1E2D
	COLORREF rgb = spec.overBase ? spec.textRgbOver : spec.textRgbBelow;

	wchar_t text[kTrayTextMaxChars]{};
	ResolveDisplayText(spec, text);

	const float regionLeft = (float)textRc.left + (float)kTextFitInsetPx;
	const float regionTop = (float)textRc.top + (float)kTextFitInsetPx;
	const float regionRight = (float)textRc.right - (float)kTextFitInsetPx;
	const float regionBottom = (float)textRc.bottom - kTextBottomMarginPx - kTextBottomSafetyPx;
	// Font sizing for the icon uses GDI+ tight glyph bounds below; do not mix
	// with GDI metrics here (different fonts/rendering modes can disagree).

	// Draw text LAST (top-most). Use GDI+ path so we can measure tight glyph bounds
	// and place the glyph box at the icon bottom with a 1px margin.
	{
		Gdiplus::Graphics g(hdc);
		g.SetPageUnit(Gdiplus::UnitPixel);
		g.ResetClip();
		g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
		g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
		g.SetTextRenderingHint(Gdiplus::TextRenderingHintSingleBitPerPixelGridFit);

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

		const int textLen = (int)wcslen(text);
		const int fontStyle = ResolveTrayFontStyle(ff);

		const TrayTextLayout layout = FindMaxTrayTextLayout(
			ff,
			text,
			textLen,
			fontStyle,
			regionLeft,
			regionTop,
			regionRight,
			regionBottom);

		Gdiplus::GraphicsPath path;
		if(!BuildPixelSnappedTextPath(
			ff,
			text,
			textLen,
			layout.em,
			fontStyle,
			layout,
			path))
		{
			SelectObject(hdc, oldBmp);
			DeleteDC(hdc);
			DeleteObject(hbm);
			return nullptr;
		}

		// BuildPixelSnappedTextPath has already applied layout.horizontalScale,
		// layout.originX, and layout.originY. Fill the path as-is: applying
		// another matrix here scales/translates the finished icon geometry a
		// second time and leaves only a small clipped fragment visible.
		Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(rgb), GetGValue(rgb), GetBValue(rgb)));
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

	// IMPORTANT: The tray icon pipeline expects *premultiplied* alpha for 32-bit icons.
	// GDI+ produces straight-alpha RGB for anti-aliased edges. If we hand straight-alpha
	// pixels to CreateIconIndirect, Windows can treat them as premultiplied and you get
	// gray halos around glyph edges.
	// Fix: premultiply RGB by A for all pixels with 0 < A < 255.
	for(size_t i = 0; i < n; i++)
	{
		auto v = px[i];
		auto a = (unsigned int)((v >> 24) & 0xFFu);
		if(a > 0 && a < 255)
		{
			auto r = (unsigned int)((v >> 16) & 0xFFu);
			auto g = (unsigned int)((v >> 8) & 0xFFu);
			auto b = (unsigned int)(v & 0xFFu);

			r = (r * a + 127u) / 255u;
			g = (g * a + 127u) / 255u;
			b = (b * a + 127u) / 255u;

			px[i] = (a << 24) | (r << 16) | (g << 8) | b;
		}
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
