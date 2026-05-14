#include "CpuFrequency.h"

#include <wbemidl.h>
#include <comdef.h>

#include <algorithm>
#include <optional>
#include <vector>

#include <pdhmsg.h>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "pdh.lib")

static std::optional<double> TryReadMaxClockSpeedMHz(IWbemServices* svc)
{
	IEnumWbemClassObject* enumerator = nullptr;
	auto hr = svc->ExecQuery(
		bstr_t(L"WQL"),
		bstr_t(L"SELECT MaxClockSpeed FROM Win32_Processor"),
		WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
		nullptr,
		&enumerator
	);
	if(FAILED(hr) || !enumerator) return std::nullopt;

	IWbemClassObject* obj = nullptr;
	ULONG returned = 0;
	hr = enumerator->Next(WBEM_INFINITE, 1, &obj, &returned);
	if(FAILED(hr) || returned == 0 || !obj) { enumerator->Release(); return std::nullopt; }

	VARIANT vt;
	VariantInit(&vt);
	hr = obj->Get(L"MaxClockSpeed", 0, &vt, nullptr, nullptr);

	std::optional<double> out;
	if(SUCCEEDED(hr) && (vt.vt == VT_I4 || vt.vt == VT_UI4))
		out = static_cast<double>(vt.ulVal);

	VariantClear(&vt);
	obj->Release();
	enumerator->Release();
	return out;
}

namespace {

// Returns false for nullptr, empty, exact _Total, or names ending with ,_Total.
bool IsRealLogicalProcessorInstance(const wchar_t* name)
{
	if(!name || !*name)
		return false;

	if(_wcsicmp(name, L"_Total") == 0)
		return false;

	const wchar_t* lastComma = wcsrchr(name, L',');
	if(lastComma && _wcsicmp(lastComma + 1, L"_Total") == 0)
		return false;

	return true;
}

// Pure helper: compute average and max from a list of per-core samples.
struct CoreStats
{
	double avg = 0.0;
	double max = 0.0;
	int count = 0;
};

CoreStats ComputeCoreStats(const double* values, int count)
{
	CoreStats result;
	double sum = 0.0;
	for(int i = 0; i < count; ++i)
	{
		if(values[i] <= 0.0)
			continue;
		sum += values[i];
		if(values[i] > result.max)
			result.max = values[i];
		++result.count;
	}
	if(result.count > 0)
		result.avg = sum / result.count;
	return result;
}

struct NamedSample
{
	const wchar_t* name;
	double value;
};

struct NamedStats
{
	double avg = 0.0;
	double max = 0.0;
	int count = 0;
};

NamedStats ComputeNamedCoreStats(const NamedSample* samples, int count)
{
	NamedStats result;
	double sum = 0.0;
	for(int i = 0; i < count; ++i)
	{
		if(!IsRealLogicalProcessorInstance(samples[i].name))
			continue;
		if(samples[i].value <= 0.0)
			continue;
		sum += samples[i].value;
		if(samples[i].value > result.max)
			result.max = samples[i].value;
		++result.count;
	}
	if(result.count > 0)
		result.avg = sum / result.count;
	return result;
}

bool RunSelfTests()
{
	// IsRealLogicalProcessorInstance tests
	{
		auto check = [](const wchar_t* n, bool expected) -> bool {
			return IsRealLogicalProcessorInstance(n) == expected;
		};
		if(!check(L"_Total", false)) return false;
		if(!check(L"0,_Total", false)) return false;
		if(!check(L"1,_Total", false)) return false;
		if(!check(L"0,0", true)) return false;
		if(!check(L"0,1", true)) return false;
		if(!check(L"1,0", true)) return false;
		if(!check(L"3", true)) return false;
		if(!check(L"", false)) return false;
		if(!check(nullptr, false)) return false;
	}

	// ComputeCoreStats tests
	{
		double vals[] = {1000.0, 2000.0, 3000.0};
		auto s = ComputeCoreStats(vals, 3);
		if(s.avg != 2000.0 || s.max != 3000.0 || s.count != 3) return false;
	}
	{
		double vals[] = {1000.0, 0.0, -5.0, 2000.0};
		auto s = ComputeCoreStats(vals, 4);
		if(s.avg != 1500.0 || s.max != 2000.0 || s.count != 2) return false;
	}
	{
		double vals[] = {0.0, -1.0};
		auto s = ComputeCoreStats(vals, 2);
		if(s.count != 0) return false;
	}
	{
		auto s = ComputeCoreStats(nullptr, 0);
		if(s.count != 0) return false;
	}

	// ComputeNamedCoreStats with aggregate filtering.
	{
		NamedSample samples[] = {
			{L"0,0", 1000.0},
			{L"0,1", 2000.0},
			{L"0,_Total", 5000.0},
			{L"_Total", 6000.0},
		};
		auto s = ComputeNamedCoreStats(samples, 4);
		if(s.avg != 1500.0 || s.max != 2000.0 || s.count != 2) return false;
	}
	return true;
}

}

CpuFrequency::~CpuFrequency()
{
	if(query_)
	{
		PdhCloseQuery(query_);
		query_ = nullptr;
	}
	perCoreFreqMHzCounter_ = nullptr;
	perCorePerfPctCounter_ = nullptr;
	totalPerfPctCounter_ = nullptr;
	totalFreqMHzCounter_ = nullptr;
}

bool CpuFrequency::Initialize()
{
	InitBaseWmi();
	if(!InitPdh())
		return false;

	if(!RunSelfTests())
		return false;

	return true;
}

bool CpuFrequency::InitBaseWmi()
{
	HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if(FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;

	hr = CoInitializeSecurity(
		nullptr,
		-1,
		nullptr,
		nullptr,
		RPC_C_AUTHN_LEVEL_DEFAULT,
		RPC_C_IMP_LEVEL_IMPERSONATE,
		nullptr,
		EOAC_NONE,
		nullptr
	);
	if(FAILED(hr) && hr != RPC_E_TOO_LATE) return false;

	IWbemLocator* locator = nullptr;
	hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID*)&locator);
	if(FAILED(hr) || !locator) return false;

	IWbemServices* services = nullptr;
	hr = locator->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), nullptr, nullptr, 0, 0, nullptr, nullptr, &services);
	locator->Release();
	if(FAILED(hr) || !services) return false;

	hr = CoSetProxyBlanket(
		services,
		RPC_C_AUTHN_WINNT,
		RPC_C_AUTHZ_NONE,
		nullptr,
		RPC_C_AUTHN_LEVEL_CALL,
		RPC_C_IMP_LEVEL_IMPERSONATE,
		nullptr,
		EOAC_NONE
	);
	if(FAILED(hr)) { services->Release(); return false; }

	auto maxMHz = TryReadMaxClockSpeedMHz(services);
	services->Release();

	if(maxMHz.has_value() && maxMHz.value() > 0)
		baseMHz_ = maxMHz.value();

	return baseMHz_ > 0;
}

bool CpuFrequency::InitPdh()
{
	auto s = PdhOpenQueryW(nullptr, 0, &query_);
	if(s != ERROR_SUCCESS || !query_) return false;

	// All counters are best-effort. At least one must provide useful data for Read() to succeed.

	s = PdhAddEnglishCounterW(query_, L"\\Processor Information(*)\\Processor Frequency", 0, &perCoreFreqMHzCounter_);
	if(s != ERROR_SUCCESS || !perCoreFreqMHzCounter_)
		perCoreFreqMHzCounter_ = nullptr;

	s = PdhAddEnglishCounterW(query_, L"\\Processor Information(*)\\% Processor Performance", 0, &perCorePerfPctCounter_);
	if(s != ERROR_SUCCESS || !perCorePerfPctCounter_)
		perCorePerfPctCounter_ = nullptr;

	s = PdhAddEnglishCounterW(query_, L"\\Processor Information(_Total)\\% Processor Performance", 0, &totalPerfPctCounter_);
	if(s != ERROR_SUCCESS || !totalPerfPctCounter_)
		totalPerfPctCounter_ = nullptr;

	// Diagnostic only: total Processor Frequency is kept but never used for baseMHz_.
	s = PdhAddEnglishCounterW(query_, L"\\Processor Information(_Total)\\Processor Frequency", 0, &totalFreqMHzCounter_);
	if(s != ERROR_SUCCESS || !totalFreqMHzCounter_)
		totalFreqMHzCounter_ = nullptr;

	// Prime: PDH may need multiple collections before returning valid data.
	PdhCollectQueryData(query_);
	Sleep(50);
	PdhCollectQueryData(query_);

	// At least one usable source is required.
	bool hasDirectFreq = perCoreFreqMHzCounter_ != nullptr;
	bool hasPerCorePerfBase = perCorePerfPctCounter_ != nullptr && baseMHz_ > 0.0;
	bool hasTotalPerfBase = totalPerfPctCounter_ != nullptr && baseMHz_ > 0.0;
	if(!hasDirectFreq && !hasPerCorePerfBase && !hasTotalPerfBase)
	{
		PdhCloseQuery(query_);
		query_ = nullptr;
		return false;
	}

	return true;
}

static bool TryReadDoubleCounter(PDH_HCOUNTER c, double& outValue, unsigned long& outCStatus, long& outStatus)
{
	if(!c) return false;

	PDH_FMT_COUNTERVALUE v{};
	outStatus = (long)PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE, nullptr, &v);
	outCStatus = v.CStatus;

	if(outStatus == ERROR_SUCCESS && (v.CStatus == ERROR_SUCCESS || v.CStatus == PDH_CSTATUS_VALID_DATA || v.CStatus == PDH_CSTATUS_NEW_DATA))
	{
		outValue = v.doubleValue;
		return true;
	}
	return false;
}

static bool TryReadCounterArrayStats(
	PDH_HCOUNTER counter,
	double& outAvg,
	double& outMax,
	int& outCount,
	unsigned long& outCStatus,
	long& outStatus)
{
	outAvg = 0.0;
	outMax = 0.0;
	outCount = 0;
	outCStatus = 0;
	outStatus = 0;

	if(!counter)
		return false;

	DWORD bufferSize = 0;
	DWORD itemCount = 0;
	auto s = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, nullptr);
	if(s != PDH_MORE_DATA || bufferSize == 0 || itemCount == 0)
	{
		outStatus = (long)s;
		return false;
	}

	std::vector<BYTE> buffer(bufferSize);
	auto items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
	s = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, items);
	outStatus = (long)s;
	if(s != ERROR_SUCCESS)
		return false;

	double sum = 0.0;
	double mx = 0.0;
	int count = 0;
	unsigned long lastStatus = 0;

	for(DWORD i = 0; i < itemCount; ++i)
	{
		const auto& item = items[i];
		if(!IsRealLogicalProcessorInstance(item.szName))
			continue;

		lastStatus = item.FmtValue.CStatus;
		if(item.FmtValue.CStatus != ERROR_SUCCESS &&
			item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
			item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA)
			continue;

		const auto value = item.FmtValue.doubleValue;
		if(value <= 0.0)
			continue;

		sum += value;
		if(value > mx)
			mx = value;
		++count;
	}

	outCStatus = lastStatus;
	if(count <= 0)
		return false;

	outAvg = sum / count;
	outMax = mx;
	outCount = count;
	return true;
}

CpuReading CpuFrequency::Read()
{
	CpuReading r{};
	r.baseMHz = baseMHz_;

	if(!query_)
	{
		r.source = L"PDH-NotReady";
		return r;
	}

	auto s = PdhCollectQueryData(query_);
	lastPdhStatus_ = (long)s;
	if(s != ERROR_SUCCESS)
	{
		r.lastPdhStatus = lastPdhStatus_;
		// Use cached reading on transient collect failure to avoid 0.00 GHz.
		// This mirrors the cached fallback in step 4 below.
		if(lastGoodAvgMHz_ > 0.0)
		{
			r.avgMHz = lastGoodAvgMHz_;
			r.maxMHz = lastGoodMaxMHz_;
			r.currentMHz = lastGoodAvgMHz_;
			r.validCoreCount = lastGoodValidCoreCount_;
			r.ok = true;
			r.source = lastGoodSource_ + L"/Cached/CollectFail";
			r.baseMHz = baseMHz_;
			r.lastPdhCStatus = lastPdhCStatus_;
			return r;
		}
		r.source = L"PDH-CollectFail";
		return r;
	}

	// Helper to publish a successful reading and update last-good cache.
	auto publish = [&](double avg, double max, int count, const std::wstring& src)
	{
		r.avgMHz = avg;
		r.maxMHz = max;
		r.currentMHz = avg;
		r.validCoreCount = count;
		r.ok = true;
		r.source = src;
		r.lastPdhStatus = lastPdhStatus_;
		r.lastPdhCStatus = lastPdhCStatus_;

		lastGoodAvgMHz_ = avg;
		lastGoodMaxMHz_ = max;
		lastGoodValidCoreCount_ = count;
		lastGoodSource_ = src;
	};

	// 1. Per-core direct frequency counter (primary source, no base needed).
	//    Processor Frequency is current effective MHz on each core.
	//    It must never be used as baseMHz_.
	{
		double avg = 0.0, mx = 0.0;
		int count = 0;
		unsigned long cst = 0;
		long st = 0;
		if(perCoreFreqMHzCounter_ &&
			TryReadCounterArrayStats(perCoreFreqMHzCounter_, avg, mx, count, cst, st))
		{
			lastPdhCStatus_ = cst;
			lastPdhStatus_ = st;
			publish(avg, mx, count, L"PDH-PerCore-Frequency");
			return r;
		}
		lastPdhCStatus_ = cst;
		lastPdhStatus_ = st;
	}

	// 2. Per-core performance percentage × WMI base MHz.
	{
		double avgPct = 0.0, maxPct = 0.0;
		int count = 0;
		unsigned long cst = 0;
		long st = 0;
		if(baseMHz_ > 0.0 && perCorePerfPctCounter_ &&
			TryReadCounterArrayStats(perCorePerfPctCounter_, avgPct, maxPct, count, cst, st))
		{
			lastPdhCStatus_ = cst;
			lastPdhStatus_ = st;
			publish(baseMHz_ * avgPct / 100.0, baseMHz_ * maxPct / 100.0, count, L"PDH-PerCore-PerfBase");
			return r;
		}
		lastPdhCStatus_ = cst;
		lastPdhStatus_ = st;
	}

	// 3. _Total performance percentage × WMI base MHz.
	{
		double perfPct = 0.0;
		unsigned long cst = 0;
		long st = 0;
		if(baseMHz_ > 0.0 && totalPerfPctCounter_ &&
			TryReadDoubleCounter(totalPerfPctCounter_, perfPct, cst, st))
		{
			lastPdhCStatus_ = cst;
			lastPdhStatus_ = st;
			publish(baseMHz_ * perfPct / 100.0, baseMHz_ * perfPct / 100.0, 1, L"PDH-Total-PerfBase");
			return r;
		}
	}

	// 4. Cached last-good reading for transient PDH failures.
	if(lastGoodAvgMHz_ > 0.0)
	{
		r.avgMHz = lastGoodAvgMHz_;
		r.maxMHz = lastGoodMaxMHz_;
		r.currentMHz = lastGoodAvgMHz_;
		r.validCoreCount = lastGoodValidCoreCount_;
		r.ok = true;
		r.source = lastGoodSource_ + L"/Cached";
		r.lastPdhStatus = lastPdhStatus_;
		r.lastPdhCStatus = lastPdhCStatus_;
		r.baseMHz = baseMHz_;
		return r;
	}

	// 5. No usable frequency source produced a value.
	//    If WMI base is known, indicate base-only (not a valid current Hz reading).
	r.ok = false;
	r.lastPdhStatus = lastPdhStatus_;
	r.lastPdhCStatus = lastPdhCStatus_;
	r.baseMHz = baseMHz_;

	bool hasAnyCounter = perCoreFreqMHzCounter_ || perCorePerfPctCounter_ || totalPerfPctCounter_;
	if(!hasAnyCounter)
		r.source = L"NoUsableCounter";
	else if(baseMHz_ > 0.0)
		r.source = L"BaseOnly-NoCurrentHz";
	else
		r.source = L"BaseUnknown";
	return r;
}
