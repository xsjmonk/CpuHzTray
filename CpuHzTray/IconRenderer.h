#pragma once
#include <windows.h>

#include "HistoryBuffer.h"

struct IconSpec
{
	double ghz = 0;
	bool overBase = false;
	const RingBufferD<60>* historyMHz = nullptr; // optional, oldest->newest
};

class IconRenderer
{
public:
	IconRenderer();
	~IconRenderer();

	HICON Render(const IconSpec& spec) const; // caller owns, must DestroyIcon

private:
	void EnsureInit() const;
	bool LoadFontFromFile() const;

	mutable bool initialized_ = false;
	mutable HANDLE fontMemHandle_ = nullptr; // RemoveFontMemResourceEx in dtor
	mutable HFONT hFont_ = nullptr;
};
