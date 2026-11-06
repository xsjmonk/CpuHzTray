#include "DpiFont.h"

#include "resource.h"

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <memory>
#include <vector>

namespace
{
constexpr UINT kDefaultDpi = 96;

struct FontResourceDefinition
{
	int resourceId;
	const wchar_t* fileStem;
};

constexpr FontResourceDefinition kFontResources[] = {
	{IDR_FONT_EMBEDDED, L"embedded"},
	{IDR_FONT_EMBEDDED_HIGH_DPI, L"embedded_highdpi"},
	{IDR_FONT_KOLLECTIONBITMAP, L"kollectionbitmap"},
	{IDR_FONT_PIXELZONE_E4MV4, L"Pixelzone-E4MV4"},
	{IDR_FONT_16X8PXL_SANS, L"16x8pxl-sans"},
	{IDR_FONT_FLEXI_IBM_VGA_TRUE, L"Flexi_IBM_VGA_True"},
	{IDR_FONT_READABLE9X4, L"Readable9x4"},
	{IDR_FONT_PIXTALL, L"PixTall"},
	{IDR_FONT_PIXEL_MILLENNIUM, L"Pixel_Millennium"}};

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

std::wstring GetIniPath()
{
	wchar_t modulePath[MAX_PATH]{};
	const DWORD length = GetModuleFileNameW(
		nullptr,
		modulePath,
		static_cast<DWORD>(std::size(modulePath)));
	if(length == 0 || length >= std::size(modulePath))
		return {};

	std::wstring path(modulePath, length);
	const size_t separator = path.find_last_of(L"\\/");
	if(separator == std::wstring::npos)
		return L"CpuHzTray.ini";
	return path.substr(0, separator + 1) + L"CpuHzTray.ini";
}
}

struct DpiFont::Impl
{
	struct EmbeddedFont
	{
		std::wstring fileStem;
		std::wstring familyName;
		std::vector<BYTE> fontBytes;
		std::unique_ptr<Gdiplus::PrivateFontCollection> collection;
	};

	bool initialized = false;
	bool ok = false;
	std::wstring error;
	FontIni ini;
	std::vector<std::unique_ptr<EmbeddedFont>> fonts;
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

	const std::wstring iniPath = GetIniPath();
	if(iniPath.empty() || !impl_->ini.LoadIfPresent(iniPath))
	{
		impl_->error = impl_->ini.GetError()
			? impl_->ini.GetError()
			: L"Failed to load CpuHzTray.ini.";
		return false;
	}

	for(const auto& definition : kFontResources)
	{
		auto font = std::make_unique<Impl::EmbeddedFont>();
		font->fileStem = definition.fileStem;
		font->collection =
			std::make_unique<Gdiplus::PrivateFontCollection>();
		if(!LoadFontIntoPrivateCollection(
			hMod,
			definition.resourceId,
			*font->collection,
			font->fontBytes))
		{
			impl_->error = L"Failed to load embedded font: ";
			impl_->error += definition.fileStem;
			impl_->error += L".ttf.";
			return false;
		}
		if(!ExtractFamilyName(*font->collection, font->familyName))
		{
			impl_->error = L"Failed to extract the family name from embedded font: ";
			impl_->error += definition.fileStem;
			impl_->error += L".ttf.";
			return false;
		}
		impl_->fonts.push_back(std::move(font));
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
	const std::wstring& requestedFont = highDpi
		? impl_->ini.GetHighDpiFont()
		: impl_->ini.Get100DpiFont();
	const auto requestedStyle = highDpi
		? impl_->ini.GetHighDpiStyle()
		: impl_->ini.Get100DpiStyle();

	const Impl::EmbeddedFont* selectedFont = nullptr;
	for(const auto& font : impl_->fonts)
	{
		if(_wcsicmp(font->fileStem.c_str(), requestedFont.c_str()) == 0)
		{
			selectedFont = font.get();
			break;
		}
	}

	if(selectedFont == nullptr)
	{
		impl_->error = L"Configured font is not embedded: ";
		impl_->error += requestedFont;
		impl_->error += L".ttf.";
		return false;
	}

	outSelection.privateFontCollection = selectedFont->collection.get();
	outSelection.familyName = selectedFont->familyName.c_str();
	outSelection.style = ResolveTextStyle(requestedStyle);
	outSelection.rendering.antiAlias = false;
	outSelection.rendering.renderToArgbBitmap = highDpi;
	outSelection.rendering.textInkBleedPx = highDpi ? 1 : 0;
	return true;
}

const wchar_t* DpiFont::GetError() const
{
	return impl_->error.empty() ? nullptr : impl_->error.c_str();
}

int DpiFont::ResolveTextStyle(ConfigTextStyle style)
{
	return style == ConfigTextStyle::Bold
		? Gdiplus::FontStyleBold
		: Gdiplus::FontStyleRegular;
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
