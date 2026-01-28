#pragma once
#include <windows.h>
#include <string>
#include <pdh.h>

struct CpuReading
{
	double currentMHz = 0;
	double baseMHz = 0;
	bool ok = false;
	std::wstring source;
	long lastPdhStatus = 0;
	unsigned long lastPdhCStatus = 0;
};

class CpuFrequency
{
public:
	CpuFrequency() = default;
	~CpuFrequency();

	bool Initialize();
	CpuReading Read();

private:
	bool InitBaseWmi();
	bool InitPdh();

	double baseMHz_ = 0;
	double lastGoodPerfPct_ = 0.0;
	double lastGoodFreqMHz_ = 0.0;

	PDH_HQUERY query_ = nullptr;
	PDH_HCOUNTER perfPctCounter_ = nullptr;
	PDH_HCOUNTER freqMHzCounter_ = nullptr;

	long lastPdhStatus_ = 0;
	unsigned long lastPdhCStatus_ = 0;
};
