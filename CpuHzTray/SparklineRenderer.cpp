#include "SparklineRenderer.h"

#include <algorithm>
#include <vector>
#include <cmath>
#include <limits>

#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

static inline double ClampD(double v, double lo, double hi)
{
	if(v < lo) return lo;
	if(v > hi) return hi;
	return v;
}

static double Percentile(std::vector<double>& v, double p01)
{
	if(v.empty()) return 0.0;
	std::sort(v.begin(), v.end());
	if(p01 <= 0.0) return v.front();
	if(p01 >= 1.0) return v.back();

	double idx = (double)(v.size() - 1) * p01;
	size_t i0 = (size_t)std::floor(idx);
	size_t i1 = (size_t)std::ceil(idx);
	if(i0 == i1) return v[i0];
	double t = idx - (double)i0;
	return v[i0] + (v[i1] - v[i0]) * t;
}

static Gdiplus::Color MakeArgb(BYTE a, COLORREF rgb)
{
	return Gdiplus::Color(
		a,
		GetRValue(rgb),
		GetGValue(rgb),
		GetBValue(rgb)
	);
}

void DrawAreaSparklineGdiPlus(
	HDC hdc,
	const RECT& plotRcWin,
	const double* samples,
	int sampleCount,
	double baseMHz,
	const SparklineStyle& style
)
{
	if(hdc == nullptr) return;
	if(samples == nullptr) return;
	if(sampleCount < 2) return;

	int w = plotRcWin.right - plotRcWin.left;
	int h = plotRcWin.bottom - plotRcWin.top;
	if(w <= 2 || h <= 2) return;

	Gdiplus::Graphics g(hdc);
	g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
	g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);

	Gdiplus::Rect plotRc(plotRcWin.left, plotRcWin.top, w, h);
	g.SetClip(plotRc);

	const int left = plotRc.X;
	const int top = plotRc.Y + style.padding;
	const int right = plotRc.GetRight() - 1;
	const int bottom = plotRc.GetBottom() - 1 - style.padding;
	if(right - left <= 1 || bottom - top <= 1) { g.ResetClip(); return; }

	const float centerY = (float)(top + (bottom - top) / 2);
	const float halfH = (float)(bottom - top) / 2.0f;
	const float dx = (float)(right - left) / (float)(sampleCount - 1);

	// Copy values and compute robust stats.
	std::vector<double> vals;
	vals.reserve((size_t)sampleCount);
	for(int i = 0; i < sampleCount; ++i) vals.push_back(samples[i]);

	std::vector<double> sorted = vals;
	double median = Percentile(sorted, SparklineDefaults::kPercentileMedian);

	double base = (baseMHz > 0.0) ? baseMHz : median;

	double aboveMin = std::numeric_limits<double>::max();
	double aboveMax = std::numeric_limits<double>::lowest();
	double belowMin = std::numeric_limits<double>::max();
	double belowMax = std::numeric_limits<double>::lowest();
	for(const double v : vals)
	{
		if(v > base)
		{
			aboveMin = std::min(aboveMin, v);
			aboveMax = std::max(aboveMax, v);
		}
		else if(v < base)
		{
			belowMin = std::min(belowMin, v);
			belowMax = std::max(belowMax, v);
		}
	}

	const bool hasAboveSamples = aboveMin != std::numeric_limits<double>::max();
	const bool hasBelowSamples = belowMin != std::numeric_limits<double>::max();
	const double aboveRange = hasAboveSamples
		? std::max(aboveMax - aboveMin, SparklineDefaults::kMinimumDynamicRangeMhz)
		: SparklineDefaults::kMinimumDynamicRangeMhz;
	const double belowRange = hasBelowSamples
		? std::max(belowMax - belowMin, SparklineDefaults::kMinimumDynamicRangeMhz)
		: SparklineDefaults::kMinimumDynamicRangeMhz;

	// Keep a side's first point just inside its color half. This prevents an
	// above-base point from landing on the integer baseline clip row and being
	// painted with the below-base pen.
	const double sideInset = std::min(0.5, 0.5 / std::max(halfH, 1.0f));

	// The base remains the red/blue classification boundary. Once a sample is
	// classified, normalize against that side's recent min/max rather than
	// against the distance from the base; otherwise a nearby 10 MHz fluctuation
	// is still compressed by a base that is hundreds of MHz away.
	// Build smoothed curve points (oldest->newest), mapped around centerY.
	std::vector<Gdiplus::PointF> pts;
	pts.reserve((size_t)sampleCount);

	for(int i = 0; i < sampleCount; ++i)
	{
		double v = vals[(size_t)i];
		double d = 0.0;
		if(v > base && hasAboveSamples)
		{
			const double normalized = (v - aboveMin) / aboveRange;
			d = sideInset + normalized * (1.0 - sideInset);
		}
		else if(v < base && hasBelowSamples)
		{
			const double normalized = (belowMax - v) / belowRange;
			d = -(sideInset + normalized * (1.0 - sideInset));
		}
		else if(v > base)
		{
			d = 0.5;
		}
		else if(v < base)
		{
			d = -0.5;
		}
		d = ClampD(d, -1.0, 1.0);

		float x = (float)left + dx * (float)i;
		float y = centerY - (float)d * halfH;
		y = (float)ClampD(y, (double)top, (double)bottom);

		pts.push_back(Gdiplus::PointF(x, y));
	}

	// Reduce long runs of baseline (centerY) points before spline fitting.
	// Many consecutive baseline points can cause visible "bar" artifacts with AddCurve.
	auto compressBaselineRuns = [&](std::vector<Gdiplus::PointF>& p)
	{
		if(p.size() < 3) return;
		std::vector<Gdiplus::PointF> out;
		out.reserve(p.size());
		out.push_back(p.front());
		for(size_t i = 1; i + 1 < p.size(); ++i)
		{
			const auto& prev = out.back();
			const auto& cur = p[i];
			if(prev.Y == centerY && cur.Y == centerY)
				continue;
			out.push_back(cur);
		}
		out.push_back(p.back());
		p.swap(out);
	};

	compressBaselineRuns(pts);

	auto buildCurvePath = [&](Gdiplus::GraphicsPath& path, const std::vector<Gdiplus::PointF>& p)
	{
		path.Reset();
		if(p.size() < 2) return;
		path.StartFigure();
		if((int)p.size() >= 3)
			path.AddCurve(p.data(), (INT)p.size(), style.curveTension);
		else
			path.AddLines(p.data(), (INT)p.size());
	};

	Gdiplus::GraphicsPath curvePath;
	buildCurvePath(curvePath, pts);

	// Create vertical gradients: red (top -> baseline), blue (baseline -> bottom).
	Gdiplus::Color aboveTop = MakeArgb(SparklineDefaults::kGradientAlpha, style.aboveTopRgb);
	Gdiplus::Color aboveBase = MakeArgb(SparklineDefaults::kGradientAlpha, style.aboveBaseRgb);
	Gdiplus::Color belowBase = MakeArgb(SparklineDefaults::kGradientAlpha, style.belowBaseRgb);
	Gdiplus::Color belowBottom = MakeArgb(SparklineDefaults::kGradientAlpha, style.belowBottomRgb);

	// Vertical gradients with a hard turning point at the baseline (centerY).
	// Above: baseline -> top becomes more red.
	// Below: baseline -> bottom becomes more green.
	Gdiplus::LinearGradientBrush aboveBrush(
		Gdiplus::Point(left, (int)centerY),
		Gdiplus::Point(left, top),
		aboveBase,
		aboveTop
	);

	Gdiplus::LinearGradientBrush belowBrush(
		Gdiplus::Point(left, (int)centerY),
		Gdiplus::Point(left, bottom),
		belowBase,
		belowBottom
	);

	Gdiplus::Pen abovePen(&aboveBrush, style.lineWidth);
	Gdiplus::Pen belowPen(&belowBrush, style.lineWidth);
	abovePen.SetLineJoin(Gdiplus::LineJoinRound);
	belowPen.SetLineJoin(Gdiplus::LineJoinRound);
	abovePen.SetStartCap(Gdiplus::LineCapRound);
	abovePen.SetEndCap(Gdiplus::LineCapRound);
	belowPen.SetStartCap(Gdiplus::LineCapRound);
	belowPen.SetEndCap(Gdiplus::LineCapRound);

	// Line-only rendering: draw the same smooth curve twice with different clips/gradient pens.
	// IMPORTANT: avoid drawing the baseline scanline twice (prevents fuzzier/thicker-looking stroke).
	const int baseY = (int)centerY;

	// Clip strictly above baseline.
	Gdiplus::Rect aboveRc(left, top, right - left + 1, (baseY - 1) - top + 1);
	if(aboveRc.Height > 0)
	{
		g.SetClip(aboveRc);
		g.DrawPath(&abovePen, &curvePath);
	}

	// Clip at/below baseline.
	Gdiplus::Rect belowRc(left, baseY, right - left + 1, bottom - baseY + 1);
	if(belowRc.Height > 0)
	{
		g.SetClip(belowRc);
		g.DrawPath(&belowPen, &curvePath);
	}

	g.ResetClip();
}
