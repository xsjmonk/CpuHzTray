#pragma once

#include <windows.h>

// No GDI+ types in this header (avoids build issues in translation units that
// include this before <gdiplus.h>). Implementation uses GDI+ internally.

struct SparklineStyle
{
	int padding = 1;

	// Alpha is applied uniformly to the entire plot (fill + outline + dot).
	BYTE alpha = 250;

	COLORREF topRgb = RGB(0xD8, 0xE8, 0xFF);
	COLORREF bottomRgb = RGB(0x14, 0x75, 0xFF);

	// Outline/dot use bottom color (with same alpha)
	int outlineWidth = 1;

	// Smoothing & normalization
	float curveTension = 0.35f;

	// Stronger gain settings
	// When the observed window range is tight, expand visual contrast around the mid.
	double tightEnhance = 1.8;
	double minRangeMhzFixed = 300.0;     // fixed minimum range
	double minRangeMedianPct = 0.07;     // 5% of median
};

// Draw a Proxifier-like filled area sparkline into plotRc on the given HDC.
// samples must be ordered oldest->newest.
void DrawAreaSparklineGdiPlus(
	HDC hdc,
	const RECT& plotRc,
	const double* samples,
	int sampleCount,
	const SparklineStyle& style
);
