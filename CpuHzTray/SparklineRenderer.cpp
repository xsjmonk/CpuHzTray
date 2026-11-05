#include "SparklineRenderer.h"

#include <algorithm>
#include <vector>
#include <cmath>

#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

static inline double ClampD(double v, double lo, double hi)
{
	if(v < lo) return lo;
	if(v > hi) return hi;
	return v;
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

	const float dx = (float)(right - left) / (float)(sampleCount - 1);
	const float plotHeight = (float)(bottom - top);

	std::vector<double> vals;
	vals.reserve((size_t)sampleCount);
	for(int i = 0; i < sampleCount; ++i)
		vals.push_back(samples[i]);

	double sampleMin = vals.front();
	double sampleMax = vals.front();
	for(const double v : vals)
	{
		sampleMin = std::min(sampleMin, v);
		sampleMax = std::max(sampleMax, v);
	}

	const double span = std::max(
		sampleMax - sampleMin,
		SparklineDefaults::kMinimumDynamicRangeMhz);

	// Map only by sample min/max: highest sample -> top, lowest -> bottom.
	// Base MHz is not part of this scale; it appears only as the seeded first
	// history sample and as the color boundary when it falls inside the range.
	auto valueToY = [&](double valueMHz) -> float
	{
		const double normalized = (valueMHz - sampleMin) / span;
		return (float)bottom - (float)normalized * plotHeight;
	};

	float colorBoundaryY = valueToY((sampleMin + sampleMax) * 0.5);
	if(baseMHz > 0.0)
	{
		colorBoundaryY = valueToY(baseMHz);
		colorBoundaryY = (float)ClampD(
			(double)colorBoundaryY,
			(double)top,
			(double)bottom);
	}

	std::vector<Gdiplus::PointF> pts;
	pts.reserve((size_t)sampleCount);
	for(int i = 0; i < sampleCount; ++i)
	{
		const float x = (float)left + dx * (float)i;
		float y = valueToY(vals[(size_t)i]);
		y = (float)ClampD((double)y, (double)top, (double)bottom);
		pts.push_back(Gdiplus::PointF(x, y));
	}

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

	Gdiplus::Color aboveTop = MakeArgb(SparklineDefaults::kGradientAlpha, style.aboveTopRgb);
	Gdiplus::Color aboveBase = MakeArgb(SparklineDefaults::kGradientAlpha, style.aboveBaseRgb);
	Gdiplus::Color belowBase = MakeArgb(SparklineDefaults::kGradientAlpha, style.belowBaseRgb);
	Gdiplus::Color belowBottom = MakeArgb(SparklineDefaults::kGradientAlpha, style.belowBottomRgb);

	Gdiplus::LinearGradientBrush aboveBrush(
		Gdiplus::Point(left, (int)colorBoundaryY),
		Gdiplus::Point(left, top),
		aboveBase,
		aboveTop
	);

	Gdiplus::LinearGradientBrush belowBrush(
		Gdiplus::Point(left, (int)colorBoundaryY),
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

	const int baseY = (int)std::lround(colorBoundaryY);

	Gdiplus::Rect aboveRc(left, top, right - left + 1, (baseY - 1) - top + 1);
	if(aboveRc.Height > 0)
	{
		g.SetClip(aboveRc);
		g.DrawPath(&abovePen, &curvePath);
	}

	Gdiplus::Rect belowRc(left, baseY, right - left + 1, bottom - baseY + 1);
	if(belowRc.Height > 0)
	{
		g.SetClip(belowRc);
		g.DrawPath(&belowPen, &curvePath);
	}

	g.ResetClip();
}
