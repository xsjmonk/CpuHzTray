#pragma once

#include <string>

enum class ConfigTextStyle
{
	Normal,
	Bold
};

class FontIni
{
public:
	bool LoadIfPresent(const std::wstring& path);
	bool IsPresent() const;
	const wchar_t* GetError() const;

	const std::wstring& Get100DpiFont() const;
	const std::wstring& GetHighDpiFont() const;
	ConfigTextStyle Get100DpiStyle() const;
	ConfigTextStyle GetHighDpiStyle() const;

private:
	bool ReadFontStem(
		const std::wstring& path,
		const wchar_t* section,
		const wchar_t* key,
		std::wstring& outValue);
	bool ReadTextStyle(
		const std::wstring& path,
		const wchar_t* section,
		const wchar_t* key,
		ConfigTextStyle& outValue);
	bool IsValidFontStem(const std::wstring& value) const;
	void SetError(const std::wstring& message);

	bool present_ = false;
	std::wstring error_;
	std::wstring dpi100Font_ = L"embedded";
	std::wstring highDpiFont_ = L"embedded_highdpi";
	ConfigTextStyle dpi100Style_ = ConfigTextStyle::Normal;
	ConfigTextStyle highDpiStyle_ = ConfigTextStyle::Normal;
};
