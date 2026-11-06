#include "DpiFont.h"

#include "resource.h"

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace
{
constexpr UINT kDefaultDpi = 96;

bool LoadFontIntoPrivateCollection(
	HMODULE hMod,
	int resourceId,
	Gdiplus::PrivateFontCollection& pfc,
	std::vector<BYTE>& fontBytes)
{
	HRSRC hrsrc = FindResourceW(hMod, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
	if(!hrsrc)
		return false;

	DWORD size = SizeofResource(hMod, hrsrc);
	if(size == 0)
		return false;

	HGLOBAL resource = LoadResource(hMod, hrsrc);
	if(!resource)
		return false;

	void* data = LockResource(resource);
	if(!data)
		return false;

	fontBytes.resize(size);
	memcpy(fontBytes.data(), data, size);
	return pfc.AddMemoryFont(
		fontBytes.data(),
		static_cast<INT>(fontBytes.size())) == Gdiplus::Ok;
}

bool ExtractFamilyName(
	const Gdiplus::PrivateFontCollection& pfc,
	std::wstring& outFamilyName)
{
	if(pfc.GetFamilyCount() <= 0)
		return false;

	Gdiplus::FontFamily family;
	INT familyCount = 0;
	if(pfc.GetFamilies(1, &family, &familyCount) != Gdiplus::Ok
		|| familyCount <= 0)
	{
		return false;
	}

	WCHAR familyName[LF_FACESIZE]{};
	family.GetFamilyName(familyName, LANG_NEUTRAL);
	if(familyName[0] == L'\0')
		return false;

	outFamilyName.assign(familyName);
	return true;
}
}

struct DpiFont::Impl
{
	bool initialized = false;
	bool ok = false;
	std::wstring error;
	std::wstring normalFamilyName;
	std::wstring highDpiFamilyName;
	std::vector<BYTE> normalFontBytes;
	std::vector<BYTE> highDpiFontBytes;
	std::unique_ptr<Gdiplus::PrivateFontCollection> normalCollection;
	std::unique_ptr<Gdiplus::PrivateFontCollection> highDpiCollection;
};

DpiFont::DpiFont()
	: impl_(new Impl())
{
}

DpiFont::~DpiFont()
{
	delete impl_;
	impl_ = nullptr;
}

bool DpiFont::EnsureInitialized()
{
	if(impl_->initialized)
		return impl_->ok;
	impl_->initialized = true;

	HMODULE hMod = GetModuleHandleW(nullptr);
	if(!hMod)
	{
		impl_->error = L"GetModuleHandleW failed.";
		return false;
	}

	impl_->normalCollection =
		std::make_unique<Gdiplus::PrivateFontCollection>();
	impl_->highDpiCollection =
		std::make_unique<Gdiplus::PrivateFontCollection>();

	if(!LoadFontIntoPrivateCollection(
		hMod,
		IDR_FONT_EMBEDDED,
		*impl_->normalCollection,
		impl_->normalFontBytes))
	{
		impl_->error = L"Failed to load embedded.ttf.";
		return false;
	}

	if(!LoadFontIntoPrivateCollection(
		hMod,
		IDR_FONT_EMBEDDED_HIGH_DPI,
		*impl_->highDpiCollection,
		impl_->highDpiFontBytes))
	{
		impl_->error = L"Failed to load embedded_highdpi.ttf.";
		return false;
	}

	if(!ExtractFamilyName(
		*impl_->normalCollection,
		impl_->normalFamilyName))
	{
		impl_->error = L"Failed to extract the family name from embedded.ttf.";
		return false;
	}

	if(!ExtractFamilyName(
		*impl_->highDpiCollection,
		impl_->highDpiFamilyName))
	{
		impl_->error = L"Failed to extract the family name from embedded_highdpi.ttf.";
		return false;
	}

	impl_->ok = true;
	return true;
}

bool DpiFont::Select(UINT dpi, TextFontSelection& outSelection)
{
	outSelection = {};
	if(!EnsureInitialized())
		return false;

	const UINT normalizedDpi = NormalizeDpi(dpi);
	const bool highDpi = normalizedDpi > kDefaultDpi;
	if(highDpi)
	{
		outSelection.privateFontCollection = impl_->highDpiCollection.get();
		outSelection.familyName = impl_->highDpiFamilyName.c_str();
		outSelection.rendering.antiAlias = false;
		outSelection.rendering.renderToArgbBitmap = true;
		outSelection.rendering.textInkBleedPx = 1;
	}
	else
	{
		outSelection.privateFontCollection = impl_->normalCollection.get();
		outSelection.familyName = impl_->normalFamilyName.c_str();
		outSelection.rendering.antiAlias = false;
		outSelection.rendering.renderToArgbBitmap = false;
		outSelection.rendering.textInkBleedPx = 0;
	}
	outSelection.style = ResolveTextStyle(normalizedDpi);
	return true;
}

const wchar_t* DpiFont::GetError() const
{
	return impl_->error.empty() ? nullptr : impl_->error.c_str();
}

int DpiFont::ResolveTextStyle(UINT dpi)
{
	return NormalizeDpi(dpi) > kDefaultDpi
		? Gdiplus::FontStyleRegular
		: Gdiplus::FontStyleBold;
}

UINT DpiFont::NormalizeDpi(UINT dpi)
{
	if(dpi == 0)
		dpi = kDefaultDpi;
	if(dpi <= 102)
		return kDefaultDpi;
	if(dpi <= 132)
		return 120;
	if(dpi <= 156)
		return 144;
	return dpi;
}

int DpiFont::GetTrayIconPixelSizeForDpi(UINT dpi)
{
	dpi = NormalizeDpi(dpi);
	HMODULE user32 = GetModuleHandleW(L"user32.dll");
	if(user32)
	{
		using GetSystemMetricsForDpiFn = int(WINAPI*)(int, UINT);
		auto fn = reinterpret_cast<GetSystemMetricsForDpiFn>(
			GetProcAddress(user32, "GetSystemMetricsForDpi"));
		if(fn)
		{
			const int cx = fn(SM_CXSMICON, dpi);
			const int cy = fn(SM_CYSMICON, dpi);
			return std::max({cx, cy, 16});
		}
	}
	return std::max(MulDiv(16, static_cast<int>(dpi), 96), 16);
}
