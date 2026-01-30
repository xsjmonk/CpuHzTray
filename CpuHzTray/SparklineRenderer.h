#pragma once

#include <windows.h>

// No GDI+ types in this header (avoids build issues in translation units that
// include this before <gdiplus.h>). Implementation uses GDI+ internally.

struct SparklineStyle
{
	// With a 16px-high plot region, padding must be minimal to keep vertical resolution.
	int padding = 0;

	// Alpha is applied uniformly to the entire plot fill.
	BYTE alpha = 250;

	// Stroke width for the curve (in pixels)
	// Tune for readability in the upper half of a 32x32 icon.
	// Default tuned for a 16px-high plot region.
	float lineWidth = 1.6f;

	// Baseline-centered plot colors.
	// Above-baseline area uses the red gradient (top -> baseline).
	// Below-baseline area uses the blue gradient (baseline -> bottom).
	COLORREF aboveTopRgb    = RGB(0xAF, 0x1E, 0x2D); // AF1E2D (strong red)
	COLORREF aboveBaseRgb   = RGB(0xFF, 0xC8, 0xC4); // softer near baseline
	COLORREF belowBaseRgb   = RGB(0xB5, 0xFF, 0xD6); // light green near baseline
	COLORREF belowBottomRgb = RGB(0x03, 0xDF, 0x6D); // 03DF6D

	// Smoothing & normalization
	// Lower tension => smoother curve. (0.0..1.0)
	float curveTension = 0.45f;

	// Stronger gain settings
	// When the observed window range is tight, expand visual contrast around the base.
	double minRangeMhzFixed = 200.0;     // fixed minimum full-range
	double minRangeMedianPct = 0.05;     // percent of median (as MHz)

	// Extra visual gain multiplier for tiny tray plots (16px height).
	// >1.0 amplifies deviations around the baseline.
	double visualGain = 1.75;
};

// Draw a baseline-centered smooth line curve sparkline into plotRc on the given HDC.
// samples must be ordered oldest->newest.
// baseMHz is the baseline value; if baseMHz <= 0, median(samples) is used.
void DrawAreaSparklineGdiPlus(
	HDC hdc,
	const RECT& plotRc,
	const double* samples,
	int sampleCount,
	double baseMHz,
	const SparklineStyle& style
);
