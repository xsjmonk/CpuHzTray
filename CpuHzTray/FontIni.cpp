#include "FontIni.h"

#include <windows.h>

#include <cwchar>
#include <iterator>

namespace
{
std::wstring Trim(const wchar_t* value)
{
	if(value == nullptr)
		return {};

	std::wstring result(value);
	const size_t first = result.find_first_not_of(L" \t\r\n");
	if(first == std::wstring::npos)
		return {};

	const size_t last = result.find_last_not_of(L" \t\r\n");
	return result.substr(first, last - first + 1);
}
}

bool FontIni::LoadIfPresent(const std::wstring& path)
{
	present_ = false;
	error_.clear();
	dpi100Font_ = L"embedded";
	highDpiFont_ = L"embedded_highdpi";
	dpi100Style_ = ConfigTextStyle::Normal;
	highDpiStyle_ = ConfigTextStyle::Normal;

	const DWORD attributes = GetFileAttributesW(path.c_str());
	if(attributes == INVALID_FILE_ATTRIBUTES)
		return true;
	if((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
	{
		SetError(L"Font INI path is a directory: " + path);
		return false;
	}

	present_ = true;
	if(!ReadFontStem(path, L"Fonts", L"At100Dpi", dpi100Font_)
		|| !ReadFontStem(path, L"Fonts", L"Above100Dpi", highDpiFont_)
		|| !ReadTextStyle(path, L"TextStyle", L"At100Dpi", dpi100Style_)
		|| !ReadTextStyle(path, L"TextStyle", L"Above100Dpi", highDpiStyle_))
	{
		return false;
	}
	return true;
}

bool FontIni::IsPresent() const
{
	return present_;
}

const wchar_t* FontIni::GetError() const
{
	return error_.empty() ? nullptr : error_.c_str();
}

const std::wstring& FontIni::Get100DpiFont() const
{
	return dpi100Font_;
}

const std::wstring& FontIni::GetHighDpiFont() const
{
	return highDpiFont_;
}

ConfigTextStyle FontIni::Get100DpiStyle() const
{
	return dpi100Style_;
}

ConfigTextStyle FontIni::GetHighDpiStyle() const
{
	return highDpiStyle_;
}

bool FontIni::ReadFontStem(
	const std::wstring& path,
	const wchar_t* section,
	const wchar_t* key,
	std::wstring& outValue)
{
	wchar_t buffer[260]{};
	GetPrivateProfileStringW(
		section,
		key,
		L"",
		buffer,
		static_cast<DWORD>(std::size(buffer)),
		path.c_str());
	outValue = Trim(buffer);
	if(IsValidFontStem(outValue))
		return true;

	std::wstring message = L"INI font value must be an extensionless font stem: [";
	message += section;
	message += L"] ";
	message += key;
	SetError(message);
	return false;
}

bool FontIni::ReadTextStyle(
	const std::wstring& path,
	const wchar_t* section,
	const wchar_t* key,
	ConfigTextStyle& outValue)
{
	wchar_t buffer[64]{};
	GetPrivateProfileStringW(
		section,
		key,
		L"",
		buffer,
		static_cast<DWORD>(std::size(buffer)),
		path.c_str());
	const std::wstring value = Trim(buffer);
	if(_wcsicmp(value.c_str(), L"normal") == 0)
	{
		outValue = ConfigTextStyle::Normal;
		return true;
	}
	if(_wcsicmp(value.c_str(), L"bold") == 0)
	{
		outValue = ConfigTextStyle::Bold;
		return true;
	}

	std::wstring message = L"INI text style must be normal or bold: [";
	message += section;
	message += L"] ";
	message += key;
	SetError(message);
	return false;
}

bool FontIni::IsValidFontStem(const std::wstring& value) const
{
	return !value.empty()
		&& value.find_first_of(L"\\/.:") == std::wstring::npos;
}

void FontIni::SetError(const std::wstring& message)
{
	if(error_.empty())
		error_ = message;
}
