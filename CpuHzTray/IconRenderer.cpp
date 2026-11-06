#include "IconRenderer.h"
#include "SparklineRenderer.h"

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
  4. Character spacing is part of the measured layout. The same gap must be
     included when fitting the text and when placing the final glyph cells.
  5. BuildPixelSnappedTextPath returns a path already positioned in icon
     coordinates. It applies scale and translation exactly once. The caller
     must fill that path directly and must not transform it again.
*/

struct TrayTextLayout
{
	float em = kFontSizeSearchMinEm;
	int characterCount = 0;
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

static float GetCharacterGapWidth(int textLen)
{
	return kTextCharacterGapPx * (float)std::max(0, textLen - 1);
}

static bool MeasurePerCharacterText(
	Gdiplus::FontFamily& ff,
	const wchar_t* text,
	int textLen,
	float em,
	int style,
	TrayGlyphMeasurement* glyphs,
	int glyphCapacity,
	float& outGlyphWidth,
	Gdiplus::RectF& outBounds)
{
	if(textLen <= 0 || textLen > glyphCapacity)
		return false;

	Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
	fmt.SetFormatFlags(fmt.GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap);
	outGlyphWidth = 0.0f;
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

		outGlyphWidth += glyphBounds.Width;
		top = std::min(top, glyphBounds.Y);
		bottom = std::max(bottom, glyphBounds.Y + glyphBounds.Height);
		hasVisibleGlyph = true;
	}

	if(!hasVisibleGlyph)
		return false;

	outBounds = Gdiplus::RectF(
		0.0f,
		top,
		outGlyphWidth
			+ GetCharacterGapWidth(textLen),
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
	float measuredGlyphWidth = 0.0f;
	Gdiplus::RectF measuredBounds{};
	if(!MeasurePerCharacterText(
		ff,
		text,
		textLen,
		em,
		style,
		glyphs,
		kTrayTextMaxChars,
		measuredGlyphWidth,
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

		float cellLeft = layout.originX;
		for(int previous = 0; previous < i; ++previous)
		{
			cellLeft += glyphs[previous].bounds.Width * layout.horizontalScale;
			cellLeft += kTextCharacterGapPx;
		}
		const float cellRight =
			cellLeft + glyphs[i].bounds.Width * layout.horizontalScale;
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
	int inkBleedPx,
	TrayTextLayout& out)
{
	TrayGlyphMeasurement glyphs[kTrayTextMaxChars]{};
	float glyphWidth = 0.0f;
	Gdiplus::RectF bounds{};
	if(!MeasurePerCharacterText(
		ff,
		text,
		textLen,
		em,
		style,
		glyphs,
		kTrayTextMaxChars,
		glyphWidth,
		bounds))
		return false;

	const float regionWidth = regionRight - regionLeft;
	const float regionHeight = regionBottom - regionTop;
	const float bleed = (float)inkBleedPx;
	const float usableWidth = regionWidth - 2.0f * bleed;
	const float usableHeight = regionHeight - 2.0f * bleed;
	if(usableWidth <= 0.0f || usableHeight <= 0.0f)
		return false;
	if(bounds.Height > usableHeight)
		return false;

	// Preserve the font's normal width when possible. Keep the requested gap
	// as a final-pixel gap, and compress only glyph cells if the complete
	// layout is too wide.
	const float naturalGlyphWidth = glyphWidth;
	const float characterGapWidth = GetCharacterGapWidth(textLen);
	const float availableGlyphWidth = usableWidth - characterGapWidth;
	if(availableGlyphWidth <= 0.0f)
		return false;
	const float horizontalScale =
		std::min(1.0f, availableGlyphWidth / naturalGlyphWidth);
	const float scaledWidth =
		naturalGlyphWidth * horizontalScale + characterGapWidth;
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
	float regionBottom,
	int inkBleedPx)
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
			inkBleedPx,
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
			inkBleedPx,
			best);
	}

	return best;
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

HICON IconRenderer::Render(
	const IconSpec& spec,
	int size,
	const TextFontSelection& font) const
{
	fontError_.clear();
	if(size < kIconMinSizePx)
		size = kIconMinSizePx;
	if(!font.privateFontCollection || !font.familyName)
	{
		SetFontError(L"Selected embedded font is not available.");
		return nullptr;
	}

	auto* pfc = static_cast<Gdiplus::PrivateFontCollection*>(
		const_cast<void*>(font.privateFontCollection));

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

	auto* px = (unsigned int*)bits;
	const auto pixelCount = (size_t)size * (size_t)size;

	// Normalize the sparkline pixels before an intermediate text bitmap is
	// composited. Direct rendering performs this after drawing text.
	if(font.rendering.renderToArgbBitmap)
	{
		for(size_t i = 0; i < pixelCount; i++)
		{
			auto v = px[i];
			if(((v & 0x00FFFFFFu) != 0) && ((v & 0xFF000000u) == 0))
				px[i] = v | 0xFF000000u;
		}
		for(size_t i = 0; i < pixelCount; i++)
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
	}

	const float regionLeft = (float)textRc.left + (float)kTextFitInsetPx;
	const float regionTop = (float)textRc.top + (float)kTextFitInsetPx;
	const float regionRight = (float)textRc.right - (float)kTextFitInsetPx;
	const float regionBottom =
		(float)textRc.bottom
		- (float)kTextFitInsetPx
		- kTextBottomMarginPx
		- kTextBottomSafetyPx;
	// Font sizing for the icon uses GDI+ tight glyph bounds below; do not mix
	// with GDI metrics here (different fonts/rendering modes can disagree).

	// Draw text LAST (top-most). Use GDI+ path so we can measure tight glyph bounds
	// and place the glyph box at the icon bottom with a 1px margin.
	{
		// Use the selected embedded font only. No fallback.
		Gdiplus::FontFamily ff(font.familyName, pfc);
		if(!ff.IsAvailable())
		{
			std::wstring msg = L"Selected embedded font family name not found at render time. Cached family name='";
			msg += font.familyName;
			msg += L"'.";
			SetFontError(msg);
			SelectObject(hdc, oldBmp);
			DeleteDC(hdc);
			DeleteObject(hbm);
			return nullptr;
		}

		const int textLen = (int)wcslen(text);
		const int fontStyle = font.style;

		const TrayTextLayout layout = FindMaxTrayTextLayout(
			ff,
			text,
			textLen,
			fontStyle,
			regionLeft,
			regionTop,
			regionRight,
			regionBottom,
			font.rendering.textInkBleedPx);

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
		Gdiplus::SolidBrush brush(Gdiplus::Color(
			255,
			GetRValue(rgb),
			GetGValue(rgb),
			GetBValue(rgb)));
		if(!font.rendering.renderToArgbBitmap)
		{
			Gdiplus::Graphics g(hdc);
			g.SetPageUnit(Gdiplus::UnitPixel);
			g.ResetClip();
			g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
			g.SetSmoothingMode(
				font.rendering.antiAlias
					? Gdiplus::SmoothingModeAntiAlias
					: Gdiplus::SmoothingModeNone);
			g.SetTextRenderingHint(
				font.rendering.antiAlias
					? Gdiplus::TextRenderingHintAntiAliasGridFit
					: Gdiplus::TextRenderingHintSingleBitPerPixelGridFit);
			g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
			g.FillPath(&brush, &path);
		}
		else
		{
			Gdiplus::Bitmap textBitmap(
				size,
				size,
				PixelFormat32bppARGB);
			if(textBitmap.GetLastStatus() != Gdiplus::Ok)
			{
				SelectObject(hdc, oldBmp);
				DeleteDC(hdc);
				DeleteObject(hbm);
				return nullptr;
			}

			Gdiplus::Graphics textGraphics(&textBitmap);
			textGraphics.SetPageUnit(Gdiplus::UnitPixel);
			textGraphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
			textGraphics.SetSmoothingMode(
				font.rendering.antiAlias
					? Gdiplus::SmoothingModeAntiAlias
					: Gdiplus::SmoothingModeNone);
			textGraphics.SetTextRenderingHint(
				font.rendering.antiAlias
					? Gdiplus::TextRenderingHintAntiAliasGridFit
					: Gdiplus::TextRenderingHintSingleBitPerPixelGridFit);
			textGraphics.Clear(Gdiplus::Color(0, 0, 0, 0));
			textGraphics.FillPath(&brush, &path);

			for(int y = 0; y < size; ++y)
			{
				for(int x = 0; x < size; ++x)
				{
					Gdiplus::Color source;
					if(textBitmap.GetPixel(x, y, &source) != Gdiplus::Ok)
						continue;

					const unsigned int sourceAlpha = source.GetAlpha();
					if(sourceAlpha == 0)
						continue;

					const unsigned int sourceR =
						(source.GetRed() * sourceAlpha + 127u) / 255u;
					const unsigned int sourceG =
						(source.GetGreen() * sourceAlpha + 127u) / 255u;
					const unsigned int sourceB =
						(source.GetBlue() * sourceAlpha + 127u) / 255u;
					px[(size_t)y * (size_t)size + (size_t)x] =
						(sourceAlpha << 24)
						| (sourceR << 16)
						| (sourceG << 8)
						| sourceB;
				}
			}
		}
	}
	SelectObject(hdc, oldBmp);
	DeleteDC(hdc);

	if(!font.rendering.renderToArgbBitmap)
	{
		for(size_t i = 0; i < pixelCount; i++)
		{
			auto v = px[i];
			if(((v & 0x00FFFFFFu) != 0) && ((v & 0xFF000000u) == 0))
				px[i] = v | 0xFF000000u;
		}
		for(size_t i = 0; i < pixelCount; i++)
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
