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
	g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
	g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

	Gdiplus::Rect plotRc(plotRcWin.left, plotRcWin.top, w, h);
	g.SetClip(plotRc);


	// Copy values and compute robust bounds
	std::vector<double> vals;
	vals.reserve((size_t)sampleCount);
	for(int i = 0; i < sampleCount; ++i) vals.push_back(samples[i]);

	std::vector<double> sorted = vals;
	double p10 = Percentile(sorted, 0.10);
	double p90 = Percentile(sorted, 0.90);

	// Enforce strong gain: minRange = max(fixed, medianPct)
	double median = Percentile(sorted, 0.50);
	double minRange = std::max(style.minRangeMhzFixed, std::abs(median) * style.minRangeMedianPct);

	bool expanded = false;
	if((p90 - p10) < minRange)
	{
		double mid = (p90 + p10) * 0.5;
		p10 = mid - minRange * 0.5;
		p90 = mid + minRange * 0.5;
		expanded = true;
	}
	if(p90 <= p10 + 1e-6)
	{
		p10 = p10 - 1.0;
		p90 = p90 + 1.0;
	}

	// If we had to expand (tight natural range), apply extra visual contrast around mid.
	const bool tight = expanded;

const int left = plotRc.X;
	const int top = plotRc.Y;
	const int right = plotRc.GetRight() - 1;
	const int bottom = plotRc.GetBottom() - 1;

	const float dx = (float)(right - left) / (float)(sampleCount - 1);

	// Build smoothed curve points (oldest->newest)
	std::vector<Gdiplus::PointF> pts;
	pts.reserve((size_t)sampleCount);
	for(int i = 0; i < sampleCount; ++i)
	{
		double v = ClampD(vals[(size_t)i], p10, p90);
		double t01 = (v - p10) / (p90 - p10);
		if(tight)
		{
			double tc = (t01 - 0.5) * style.tightEnhance + 0.5;
			t01 = ClampD(tc, 0.0, 1.0);
		}
		float x = (float)left + dx * (float)i;
		float y = (float)bottom - (float)t01 * (float)(bottom - top);
		y = (float)ClampD(y, (double)top, (double)bottom);
		pts.push_back(Gdiplus::PointF(x, y));
	}

	// Build filled area path using a curve for the top edge
	Gdiplus::GraphicsPath path;
	path.StartFigure();
	path.AddLine((float)left, (float)bottom, pts.front().X, (float)bottom);

	path.AddLines(pts.data(), sampleCount);

	path.AddLine(pts.back().X, (float)bottom, (float)right, (float)bottom);
	path.CloseFigure();

	// Uniform half transparency applied to ALL plot elements
	Gdiplus::Color cTop = MakeArgb(style.alpha, style.topRgb);
	Gdiplus::Color cBot = MakeArgb(style.alpha, style.bottomRgb);

	// Fill with vertical gradient with a "plateau":
	// top 2/3 stays at topRgb, bottom 1/3 transitions to bottomRgb (Proxifier-like)
	/*Gdiplus::LinearGradientBrush fillBrush(
		Gdiplus::Point(plotRc.X, plotRc.Y),
		Gdiplus::Point(plotRc.X, plotRc.GetBottom()),
		cTop,
		cBot
	);*/

	Gdiplus::LinearGradientBrush fillBrush(
		Gdiplus::Point(plotRc.X, plotRc.Y),
		Gdiplus::Point(plotRc.X, plotRc.GetBottom()),
		cTop,
		cBot
	);


// NO SetInterpolationColors call


	/*Gdiplus::Color colors[3] = { cTop, cTop, cBot };
	Gdiplus::REAL positions[3] = { 0.0f, 0.6667f, 1.0f };
	fillBrush.SetInterpolationColors(colors, positions, 3);*/

	g.FillPath(&fillBrush, &path);
	g.ResetClip();


}
