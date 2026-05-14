#pragma once
#include <windows.h>
#include <string>
#include <pdh.h>

struct CpuReading
{
	double currentMHz = 0.0;
	double avgMHz = 0.0;
	double maxMHz = 0.0;
	double baseMHz = 0.0;
	int validCoreCount = 0;
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

	double lastGoodAvgMHz_ = 0.0;
	double lastGoodMaxMHz_ = 0.0;
	int lastGoodValidCoreCount_ = 0;
	std::wstring lastGoodSource_;

	PDH_HQUERY query_ = nullptr;
	PDH_HCOUNTER perCoreFreqMHzCounter_ = nullptr;
	PDH_HCOUNTER perCorePerfPctCounter_ = nullptr;
	PDH_HCOUNTER totalPerfPctCounter_ = nullptr;
	PDH_HCOUNTER totalFreqMHzCounter_ = nullptr;

	long lastPdhStatus_ = 0;
	unsigned long lastPdhCStatus_ = 0;
};
