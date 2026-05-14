#pragma once
#include <windows.h>
#include <string>
#include <pdh.h>

struct SampleWindow
{
	static constexpr int kCapacity = 10;
	double samples_[kCapacity] = {};
	int count_ = 0;

	void Push(double value)
	{
		if(count_ < kCapacity) ++count_;
		for(int i = count_ - 1; i > 0; --i)
			samples_[i] = samples_[i - 1];
		samples_[0] = value;
	}

	int Count() const { return count_; }

	double Recent(int offset) const
	{
		if(offset < 0 || offset >= count_) return 0.0;
		return samples_[offset];
	}
};

struct CpuReading
{
	double currentMHz = 0.0;
	double avgMHz = 0.0;
	double maxMHz = 0.0;
	double minMHz = 0.0;
	double baseMHz = 0.0;
	int validCoreCount = 0;
	bool ok = false;
	std::wstring source;
	long lastPdhStatus = 0;
	unsigned long lastPdhCStatus = 0;
	bool nominalLike = false;
	std::wstring accuracy;
	std::wstring warning;
};

class CpuHzDiagnosticLogger;

class CpuFrequency
{
public:
	CpuFrequency() = default;
	~CpuFrequency();

	bool Initialize();
	CpuReading Read();
	void EnableDiagnosticLogger(const wchar_t* path);

private:
	bool InitBaseWmi();
	bool InitPdh();
	bool TryReadPowerInformation(CpuReading& r);

	double baseMHz_ = 0;

	double lastGoodAvgMHz_ = 0.0;
	double lastGoodMaxMHz_ = 0.0;
	double lastGoodMinMHz_ = 0.0;
	int lastGoodValidCoreCount_ = 0;
	std::wstring lastGoodSource_;

	PDH_HQUERY query_ = nullptr;
	PDH_HCOUNTER perCoreFreqMHzCounter_ = nullptr;
	PDH_HCOUNTER perCorePerfPctCounter_ = nullptr;
	PDH_HCOUNTER totalPerfPctCounter_ = nullptr;
	PDH_HCOUNTER totalFreqMHzCounter_ = nullptr;

	long lastPdhStatus_ = 0;
	unsigned long lastPdhCStatus_ = 0;

	SampleWindow powerInfoWindow_;
	SampleWindow perCorePerfWindow_;
	SampleWindow totalPerfWindow_;
	SampleWindow procFreqWindow_;

	CpuHzDiagnosticLogger* diagnosticLogger_ = nullptr;
};
