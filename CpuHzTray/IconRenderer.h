#pragma once
#include <windows.h>

#include "HistoryBuffer.h"

struct IconSpec
{
	double ghz = 0;
	bool overBase = false;
	const RingBufferD<60>* historyMHz = nullptr; // optional, oldest->newest
	// Text colors (explicit variables)
	COLORREF textRgbNormal = RGB(5, 70, 67);
	COLORREF textRgbOver   = RGB(144, 28, 40);
};

class IconRenderer
{
public:
	IconRenderer();
	~IconRenderer();

	HICON Render(const IconSpec& spec) const; // caller owns, must DestroyIcon

private:
	void EnsureInit() const;
	bool LoadFontFromResource() const;

	mutable bool initialized_ = false;
	mutable HANDLE fontMemHandle_ = nullptr; // RemoveFontMemResourceEx in dtor
	mutable HFONT hFont_ = nullptr;
};
