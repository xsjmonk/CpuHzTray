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

};

namespace SparklineDefaults
{
	// Avoid division by zero when the recent samples are identical.
	inline constexpr double kMinimumDynamicRangeMhz = 1e-6;
	inline constexpr BYTE kGradientAlpha = 255;
}

// Draw a smooth line curve sparkline into plotRc on the given HDC.
// samples must be ordered oldest->newest (first sample is typically base MHz).
// Vertical scale uses only sample min/max. baseMHz is used for the color
// boundary only, not for vertical normalization.
void DrawAreaSparklineGdiPlus(
	HDC hdc,
	const RECT& plotRc,
	const double* samples,
	int sampleCount,
	double baseMHz,
	const SparklineStyle& style
);
