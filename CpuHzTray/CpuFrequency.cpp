#include "CpuFrequency.h"

#include <wbemidl.h>
#include <comdef.h>

#include <algorithm>
#include <optional>
#include <vector>

#include <pdhmsg.h>
#include <powrprof.h>

// PROCESSOR_POWER_INFORMATION is defined in kernel-mode header ntpoapi.h;
// define it here for user-mode use with CallNtPowerInformation.
typedef struct _PROCESSOR_POWER_INFORMATION {
	ULONG Number;
	ULONG MaxMhz;
	ULONG CurrentMhz;
	ULONG MhzLimit;
	ULONG MaxIdleState;
	ULONG CurrentIdleState;
} PROCESSOR_POWER_INFORMATION;

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "PowrProf.lib")

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

// Pure helper: compute average, max, and min from a list of per-core samples.
struct CoreStats
{
	double avg = 0.0;
	double max = 0.0;
	double min = 0.0;
	int count = 0;
};

CoreStats ComputeCoreStats(const double* values, int count)
{
	CoreStats result;
	double sum = 0.0;
	bool first = true;
	for(int i = 0; i < count; ++i)
	{
		if(values[i] <= 0.0)
			continue;
		if(first)
		{
			result.min = values[i];
			first = false;
		}
		sum += values[i];
		if(values[i] > result.max)
			result.max = values[i];
		if(values[i] < result.min)
			result.min = values[i];
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
	double min = 0.0;
	int count = 0;
};

NamedStats ComputeNamedCoreStats(const NamedSample* samples, int count)
{
	NamedStats result;
	double sum = 0.0;
	bool first = true;
	for(int i = 0; i < count; ++i)
	{
		if(!IsRealLogicalProcessorInstance(samples[i].name))
			continue;
		if(samples[i].value <= 0.0)
			continue;
		if(first)
		{
			result.min = samples[i].value;
			first = false;
		}
		sum += samples[i].value;
		if(samples[i].value > result.max)
			result.max = samples[i].value;
		if(samples[i].value < result.min)
			result.min = samples[i].value;
		++result.count;
	}
	if(result.count > 0)
		result.avg = sum / result.count;
	return result;
}

bool IsNominalLike(const SampleWindow& w, double baseMHz)
{
	if(baseMHz <= 0.0) return false;
	int n = w.Count();
	if(n < 6) return false;
	double threshold = std::max(75.0, baseMHz * 0.035);
	for(int i = 0; i < 6; ++i)
	{
		double val = w.Recent(i);
		if(val <= 0.0) return false;
		if(std::abs(val - baseMHz) > threshold) return false;
	}
	return true;
}

static double MaterialDeltaMHz(double baseMHz)
{
	return baseMHz > 0.0 ? std::max(75.0, baseMHz * 0.035) : 75.0;
}

static bool Near(double a, double b, double eps = 0.000001)
{
	return std::abs(a - b) <= eps;
}

struct CandidateSample
{
	std::wstring source;
	double avgMHz = 0.0;
	double maxMHz = 0.0;
	double minMHz = 0.0;
	int validCoreCount = 0;
	bool ok = false;
	long pdhStatus = 0;
	unsigned long pdhCStatus = 0;
};

static bool IsBelowBase(double valueMHz, double baseMHz)
{
	return baseMHz > 0.0 && valueMHz > 0.0 && valueMHz < baseMHz;
}

static bool IsBaseLikeValue(double valueMHz, double baseMHz)
{
	if(baseMHz <= 0.0 || valueMHz <= 0.0)
		return false;
	return std::abs(valueMHz - baseMHz) <= MaterialDeltaMHz(baseMHz);
}

static int ChooseBestCandidate(
	const CandidateSample* candidates, int nCandidates,
	const SampleWindow& powerInfoWin,
	const SampleWindow& perCorePerfWin,
	const SampleWindow& totalPerfWin,
	const SampleWindow& procFreqWin,
	double baseMHz,
	bool& outAllNominalLike)
{
	outAllNominalLike = true;
	if(nCandidates <= 0) return -1;

	int piIdx = -1, perCoreIdx = -1, totalIdx = -1, procFreqIdx = -1;
	for(int i = 0; i < nCandidates; ++i)
	{
		if(candidates[i].source == L"PowerInformation-CurrentMhz") piIdx = i;
		else if(candidates[i].source == L"PDH-PerCore-PerfBase") perCoreIdx = i;
		else if(candidates[i].source == L"PDH-Total-PerfBase") totalIdx = i;
		else if(candidates[i].source == L"PDH-ProcessorFrequency-Diagnostic") procFreqIdx = i;
	}

	// Rule 0 — If the highest-priority candidate by normal order is base-like,
	// prefer the first available below-base live candidate in priority order.
	// This works even when a non-base diagnostic is present alongside a below-base source.
	if(baseMHz > 0.0)
	{
		int normalPriority[] = { piIdx, perCoreIdx, totalIdx, procFreqIdx };
		int normalFirstIdx = -1;
		for(int i = 0; i < 4; ++i)
		{
			if(normalPriority[i] >= 0)
			{
				normalFirstIdx = normalPriority[i];
				break;
			}
		}

		if(normalFirstIdx >= 0 && IsBaseLikeValue(candidates[normalFirstIdx].avgMHz, baseMHz))
		{
			int belowBasePriority[] = { perCoreIdx, totalIdx, piIdx, procFreqIdx };
			for(int i = 0; i < 4; ++i)
			{
				int idx = belowBasePriority[i];
				if(idx >= 0 && IsBelowBase(candidates[idx].avgMHz, baseMHz))
				{
					outAllNominalLike = false;
					return idx;
				}
			}
		}
	}

	// Rule 1 — If PowerInformation is base-like, prefer the first available live current source.
	if(piIdx >= 0 && IsBaseLikeValue(candidates[piIdx].avgMHz, baseMHz))
	{
		int livePriority[] = { perCoreIdx, totalIdx, procFreqIdx };
		for(int i = 0; i < 3; ++i)
		{
			int idx = livePriority[i];
			if(idx >= 0 && candidates[idx].avgMHz > 0.0)
			{
				outAllNominalLike = false;
				return idx;
			}
		}
	}

	// Rule 2 — Existing NominalLike fallback priority.
	const std::wstring kPriority[] = {
		L"PowerInformation-CurrentMhz",
		L"PDH-PerCore-PerfBase",
		L"PDH-Total-PerfBase",
		L"PDH-ProcessorFrequency-Diagnostic"
	};

	// First pass: find first non-NominalLike candidate in priority order.
	for(const auto& prio : kPriority)
	{
		int idx = -1;
		const SampleWindow* w = nullptr;
		if(prio == L"PowerInformation-CurrentMhz") { idx = piIdx; w = &powerInfoWin; }
		else if(prio == L"PDH-PerCore-PerfBase") { idx = perCoreIdx; w = &perCorePerfWin; }
		else if(prio == L"PDH-Total-PerfBase") { idx = totalIdx; w = &totalPerfWin; }
		else if(prio == L"PDH-ProcessorFrequency-Diagnostic") { idx = procFreqIdx; w = &procFreqWin; }

		if(idx < 0) continue;
		bool nominal = w ? IsNominalLike(*w, baseMHz) : false;
		if(!nominal)
		{
			outAllNominalLike = false;
			return idx;
		}
	}

	// Second pass: all are NominalLike, pick highest priority available.
	for(const auto& prio : kPriority)
	{
		int idx = -1;
		if(prio == L"PowerInformation-CurrentMhz") idx = piIdx;
		else if(prio == L"PDH-PerCore-PerfBase") idx = perCoreIdx;
		else if(prio == L"PDH-Total-PerfBase") idx = totalIdx;
		else if(prio == L"PDH-ProcessorFrequency-Diagnostic") idx = procFreqIdx;

		if(idx >= 0) return idx;
	}

	return -1;
}

std::wstring GetUtcTimestamp()
{
	SYSTEMTIME st;
	GetSystemTime(&st);
	wchar_t buf[64];
	swprintf_s(buf, L"%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	return buf;
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
		if(s.avg != 2000.0 || s.max != 3000.0 || s.min != 1000.0 || s.count != 3) return false;
	}
	{
		double vals[] = {1000.0, 0.0, -5.0, 2000.0};
		auto s = ComputeCoreStats(vals, 4);
		if(s.avg != 1500.0 || s.max != 2000.0 || s.min != 1000.0 || s.count != 2) return false;
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
	{
		double vals[] = {2200.0, 2880.0, 2500.0};
		auto s = ComputeCoreStats(vals, 3);
		if(!Near(s.avg, 7580.0 / 3.0) || s.max != 2880.0 || s.min != 2200.0 || s.count != 3) return false;
	}
	{
		double vals[] = {0.0, 0.0, 0.0};
		auto s = ComputeCoreStats(vals, 3);
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
		if(s.avg != 1500.0 || s.max != 2000.0 || s.min != 1000.0 || s.count != 2) return false;
	}

	// IsNominalLike tests
	{
		SampleWindow w;
		if(IsNominalLike(w, 2500.0)) return false;
		if(IsNominalLike(w, 0.0)) return false;
		for(int i = 0; i < 6; ++i)
			w.Push(2501.0);
		if(!IsNominalLike(w, 2500.0)) return false;
		w.Push(3000.0);
		if(IsNominalLike(w, 2500.0)) return false;
	}
	{
		SampleWindow w;
		double baseMhz = 2500.0;
		double threshold = std::max(75.0, baseMhz * 0.035);
		for(int i = 0; i < 6; ++i)
			w.Push(baseMhz + threshold - 1.0);
		if(!IsNominalLike(w, baseMhz)) return false;
		w.Push(baseMhz + threshold + 1.0);
		if(IsNominalLike(w, baseMhz)) return false;
	}

	// ChooseBestCandidate tests
	{
		// Test 1: Below base, no warm-up wait.
		CandidateSample t1[] = {
			{L"PowerInformation-CurrentMhz", 2500.0, 2500.0, 2500.0, 16, true},
			{L"PDH-PerCore-PerfBase", 1200.0, 1300.0, 1100.0, 16, true},
			{L"PDH-Total-PerfBase", 1250.0, 1250.0, 1250.0, 1, true},
		};
		SampleWindow eWins[4];
		bool allNom = false;
		int idx = ChooseBestCandidate(t1, 3, eWins[0], eWins[1], eWins[2], eWins[3], 2500.0, allNom);
		if(idx != 1 || t1[idx].avgMHz != 1200.0 || allNom) return false;
	}
	{
		// Test 2: Below base, total-only fallback.
		CandidateSample t2[] = {
			{L"PowerInformation-CurrentMhz", 2500.0, 2500.0, 2500.0, 16, true},
			{L"PDH-Total-PerfBase", 1400.0, 1400.0, 1400.0, 1, true},
		};
		SampleWindow eWins[4];
		bool allNom = false;
		int idx = ChooseBestCandidate(t2, 2, eWins[0], eWins[1], eWins[2], eWins[3], 2500.0, allNom);
		if(idx != 1 || t2[idx].avgMHz != 1400.0 || allNom) return false;
	}
	{
		// Test 3: Above base/turbo — PowerInfo is not base-like, should win.
		CandidateSample t3[] = {
			{L"PowerInformation-CurrentMhz", 3900.0, 3900.0, 3900.0, 16, true},
			{L"PDH-PerCore-PerfBase", 3800.0, 3800.0, 3800.0, 16, true},
		};
		SampleWindow eWins[4];
		bool allNom = false;
		int idx = ChooseBestCandidate(t3, 2, eWins[0], eWins[1], eWins[2], eWins[3], 2500.0, allNom);
		if(idx != 0 || allNom) return false;
	}
	{
		// Test 4: PowerInfo base-like, percentage materially different.
		CandidateSample t4[] = {
			{L"PowerInformation-CurrentMhz", 2500.0, 2500.0, 2500.0, 16, true},
			{L"PDH-PerCore-PerfBase", 3100.0, 3100.0, 3100.0, 16, true},
		};
		SampleWindow eWins[4];
		bool allNom = false;
		int idx = ChooseBestCandidate(t4, 2, eWins[0], eWins[1], eWins[2], eWins[3], 2500.0, allNom);
		if(idx != 1 || t4[idx].avgMHz != 3100.0 || allNom) return false;
	}
	{
		// Test 5: No PDH percentage candidate, only PowerInfo.
		CandidateSample t5[] = {
			{L"PowerInformation-CurrentMhz", 2500.0, 2500.0, 2500.0, 16, true},
		};
		SampleWindow eWins[4];
		bool allNom = true;
		int idx = ChooseBestCandidate(t5, 1, eWins[0], eWins[1], eWins[2], eWins[3], 2500.0, allNom);
		if(idx != 0 || allNom) return false;
	}
	{
		// Test 6: All nominal-like.
		CandidateSample t6[] = {
			{L"PowerInformation-CurrentMhz", 2500.0, 2500.0, 2500.0, 16, true},
			{L"PDH-PerCore-PerfBase", 2510.0, 2510.0, 2510.0, 16, true},
			{L"PDH-Total-PerfBase", 2490.0, 2490.0, 2490.0, 1, true},
		};
		SampleWindow nomWins[4];
		for(int i = 0; i < 6; ++i)
		{
			nomWins[0].Push(2502.0);
			nomWins[1].Push(2508.0);
			nomWins[2].Push(2492.0);
		}
		bool allNom = false;
		int idx = ChooseBestCandidate(t6, 3, nomWins[0], nomWins[1], nomWins[2], nomWins[3], 2500.0, allNom);
		if(idx != 2 || allNom) return false;
	}
	// Test A — first sample below base from per-core percentage wins over base-like PowerInfo
	{
		CandidateSample t[] = {
			{L"PowerInformation-CurrentMhz", 2500.0, 2500.0, 2500.0, 16, true},
			{L"PDH-PerCore-PerfBase", 2460.0, 2480.0, 2440.0, 16, true},
		};
		SampleWindow w[4];
		bool allNom = true;
		int idx = ChooseBestCandidate(t, 2, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 1 || t[idx].avgMHz != 2460.0 || allNom) return false;
	}
	// Test B — first sample below base from total percentage wins when per-core is unavailable
	{
		CandidateSample t[] = {
			{L"PowerInformation-CurrentMhz", 2500.0, 2500.0, 2500.0, 16, true},
			{L"PDH-Total-PerfBase", 2460.0, 2460.0, 2460.0, 1, true},
		};
		SampleWindow w[4];
		bool allNom = true;
		int idx = ChooseBestCandidate(t, 2, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 1 || t[idx].avgMHz != 2460.0 || allNom) return false;
	}
	// Test C — first sample below base from diagnostic Processor Frequency wins when percentage unavailable
	{
		CandidateSample t[] = {
			{L"PowerInformation-CurrentMhz", 2500.0, 2500.0, 2500.0, 16, true},
			{L"PDH-ProcessorFrequency-Diagnostic", 2460.0, 2480.0, 2440.0, 16, true},
		};
		SampleWindow w[4];
		bool allNom = true;
		int idx = ChooseBestCandidate(t, 2, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 1 || t[idx].avgMHz != 2460.0 || allNom) return false;
	}
	// Test D — source priority among below-base candidates
	{
		CandidateSample t[] = {
			{L"PowerInformation-CurrentMhz", 2500.0, 2500.0, 2500.0, 16, true},
			{L"PDH-PerCore-PerfBase", 2470.0, 2480.0, 2460.0, 16, true},
			{L"PDH-Total-PerfBase", 2300.0, 2300.0, 2300.0, 1, true},
			{L"PDH-ProcessorFrequency-Diagnostic", 2200.0, 2210.0, 2190.0, 16, true},
		};
		SampleWindow w[4];
		bool allNom = true;
		int idx = ChooseBestCandidate(t, 4, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 1 || t[idx].avgMHz != 2470.0 || allNom) return false;
	}
	// Test E — above-base diagnostic wins if PowerInfo base-like and percentage unavailable
	{
		CandidateSample t[] = {
			{L"PowerInformation-CurrentMhz", 2500.0, 2500.0, 2500.0, 16, true},
			{L"PDH-ProcessorFrequency-Diagnostic", 3100.0, 3150.0, 3000.0, 16, true},
		};
		SampleWindow w[4];
		bool allNom = true;
		int idx = ChooseBestCandidate(t, 2, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 1 || t[idx].avgMHz != 3100.0 || allNom) return false;
	}
	// Test F — PowerInfo still wins when it is not base-like
	{
		CandidateSample t[] = {
			{L"PowerInformation-CurrentMhz", 3900.0, 3900.0, 3900.0, 16, true},
			{L"PDH-PerCore-PerfBase", 2460.0, 2480.0, 2440.0, 16, true},
		};
		SampleWindow w[4];
		bool allNom = false;
		int idx = ChooseBestCandidate(t, 2, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 0 || allNom) return false;
	}
	// Test G — slightly below-base live candidate must win; never snap to base.
	{
		CandidateSample t[] = {
			{L"PowerInformation-CurrentMhz", 2500.0, 2500.0, 2500.0, 16, true},
			{L"PDH-PerCore-PerfBase", 2493.0, 2495.0, 2490.0, 16, true},
		};
		SampleWindow w[4];
		bool allNom = true;
		int idx = ChooseBestCandidate(t, 2, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 1 || t[idx].avgMHz != 2493.0 || allNom) return false;
	}
	// Test H — exact-base PowerInformation with exact-base live source chooses the live source when available.
	{
		CandidateSample t[] = {
			{L"PowerInformation-CurrentMhz", 2500.0, 2500.0, 2500.0, 16, true},
			{L"PDH-PerCore-PerfBase", 2500.0, 2500.0, 2500.0, 16, true},
		};
		SampleWindow w[4];
		bool allNom = true;
		int idx = ChooseBestCandidate(t, 2, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 1 || t[idx].avgMHz != 2500.0 || allNom) return false;
	}
	// Test I — when PowerInformation is not base-like, it still wins over a lower live candidate.
	{
		CandidateSample t[] = {
			{L"PowerInformation-CurrentMhz", 3900.0, 3900.0, 3900.0, 16, true},
			{L"PDH-PerCore-PerfBase", 2493.0, 2495.0, 2490.0, 16, true},
		};
		SampleWindow w[4];
		bool allNom = false;
		int idx = ChooseBestCandidate(t, 2, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 0 || allNom) return false;
	}
	// Test J — PowerInformation below base beats exact-base PDH per-core
	{
		CandidateSample t[] = {
			{L"PowerInformation-CurrentMhz", 2493.0, 2495.0, 2490.0, 16, true},
			{L"PDH-PerCore-PerfBase", 2500.0, 2500.0, 2500.0, 16, true},
		};
		SampleWindow w[4];
		bool allNom = true;
		int idx = ChooseBestCandidate(t, 2, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 0 || t[idx].avgMHz != 2493.0 || allNom) return false;
	}
	// Test K — PowerInformation below base beats exact-base PDH total when per-core is unavailable
	{
		CandidateSample t[] = {
			{L"PowerInformation-CurrentMhz", 2493.0, 2495.0, 2490.0, 16, true},
			{L"PDH-Total-PerfBase", 2500.0, 2500.0, 2500.0, 1, true},
		};
		SampleWindow w[4];
		bool allNom = true;
		int idx = ChooseBestCandidate(t, 2, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 0 || t[idx].avgMHz != 2493.0 || allNom) return false;
	}
	// Test L — PDH per-core below base still beats PowerInformation exact base
	{
		CandidateSample t[] = {
			{L"PowerInformation-CurrentMhz", 2500.0, 2500.0, 2500.0, 16, true},
			{L"PDH-PerCore-PerfBase", 2493.0, 2495.0, 2490.0, 16, true},
		};
		SampleWindow w[4];
		bool allNom = true;
		int idx = ChooseBestCandidate(t, 2, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 1 || t[idx].avgMHz != 2493.0 || allNom) return false;
	}
	// Test M — source priority among multiple below-base candidates
	{
		CandidateSample t[] = {
			{L"PowerInformation-CurrentMhz", 2493.0, 2495.0, 2490.0, 16, true},
			{L"PDH-PerCore-PerfBase", 2480.0, 2485.0, 2475.0, 16, true},
			{L"PDH-Total-PerfBase", 2470.0, 2470.0, 2470.0, 1, true},
			{L"PDH-ProcessorFrequency-Diagnostic", 2460.0, 2465.0, 2455.0, 16, true},
		};
		SampleWindow w[4];
		bool allNom = true;
		int idx = ChooseBestCandidate(t, 4, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 1 || t[idx].avgMHz != 2480.0 || allNom) return false;
	}
	// Test N — above-base PowerInformation unchanged
	{
		CandidateSample t[] = {
			{L"PowerInformation-CurrentMhz", 3900.0, 3900.0, 3900.0, 16, true},
			{L"PDH-PerCore-PerfBase", 2493.0, 2495.0, 2490.0, 16, true},
		};
		SampleWindow w[4];
		bool allNom = false;
		int idx = ChooseBestCandidate(t, 2, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 0 || allNom) return false;
	}
	// Test O — PowerInformation unavailable; exact-base per-core must not beat below-base total
	{
		CandidateSample t[] = {
			{L"PDH-PerCore-PerfBase", 2500.0, 2500.0, 2500.0, 16, true},
			{L"PDH-Total-PerfBase", 2493.0, 2493.0, 2493.0, 1, true},
		};
		SampleWindow w[4];
		bool allNom = true;
		int idx = ChooseBestCandidate(t, 2, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 1 || t[idx].avgMHz != 2493.0 || allNom) return false;
	}
	// Test P — PowerInformation unavailable; exact-base per-core must not beat below-base diagnostic
	{
		CandidateSample t[] = {
			{L"PDH-PerCore-PerfBase", 2500.0, 2500.0, 2500.0, 16, true},
			{L"PDH-ProcessorFrequency-Diagnostic", 2493.0, 2495.0, 2490.0, 16, true},
		};
		SampleWindow w[4];
		bool allNom = true;
		int idx = ChooseBestCandidate(t, 2, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 1 || t[idx].avgMHz != 2493.0 || allNom) return false;
	}
	// Test Q — PowerInformation unavailable; source priority among below-base candidates remains unchanged
	{
		CandidateSample t[] = {
			{L"PDH-PerCore-PerfBase", 2497.0, 2498.0, 2496.0, 16, true},
			{L"PDH-Total-PerfBase", 2300.0, 2300.0, 2300.0, 1, true},
			{L"PDH-ProcessorFrequency-Diagnostic", 2200.0, 2210.0, 2190.0, 16, true},
		};
		SampleWindow w[4];
		bool allNom = true;
		int idx = ChooseBestCandidate(t, 3, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 0 || t[idx].avgMHz != 2497.0 || allNom) return false;
	}
	// Test R — no base-like candidate; preserve existing priority
	{
		CandidateSample t[] = {
			{L"PDH-PerCore-PerfBase", 3900.0, 3900.0, 3900.0, 16, true},
			{L"PDH-Total-PerfBase", 2493.0, 2493.0, 2493.0, 1, true},
		};
		SampleWindow w[4];
		bool allNom = false;
		int idx = ChooseBestCandidate(t, 2, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 0 || allNom) return false;
	}
	// Test S — PowerInformation unavailable; exact-base per-core must not beat below-base total even when diagnostic is non-base
	{
		CandidateSample t[] = {
			{L"PDH-PerCore-PerfBase", 2500.0, 2500.0, 2500.0, 16, true},
			{L"PDH-Total-PerfBase", 2493.0, 2493.0, 2493.0, 1, true},
			{L"PDH-ProcessorFrequency-Diagnostic", 3600.0, 3600.0, 3600.0, 16, true},
		};
		SampleWindow w[4];
		bool allNom = true;
		int idx = ChooseBestCandidate(t, 3, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 1 || t[idx].avgMHz != 2493.0 || allNom) return false;
	}
	// Test T — PowerInformation unavailable; exact-base per-core must not beat below-base diagnostic when total unavailable
	{
		CandidateSample t[] = {
			{L"PDH-PerCore-PerfBase", 2500.0, 2500.0, 2500.0, 16, true},
			{L"PDH-ProcessorFrequency-Diagnostic", 2493.0, 2495.0, 2490.0, 16, true},
		};
		SampleWindow w[4];
		bool allNom = true;
		int idx = ChooseBestCandidate(t, 2, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 1 || t[idx].avgMHz != 2493.0 || allNom) return false;
	}
	// Test U — PowerInformation exact base must not beat below-base total even when diagnostic is non-base
	{
		CandidateSample t[] = {
			{L"PowerInformation-CurrentMhz", 2500.0, 2500.0, 2500.0, 16, true},
			{L"PDH-Total-PerfBase", 2493.0, 2493.0, 2493.0, 1, true},
			{L"PDH-ProcessorFrequency-Diagnostic", 3600.0, 3600.0, 3600.0, 16, true},
		};
		SampleWindow w[4];
		bool allNom = true;
		int idx = ChooseBestCandidate(t, 3, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 1 || t[idx].avgMHz != 2493.0 || allNom) return false;
	}
	// Test V — clearly non-base highest-priority candidate remains unchanged
	{
		CandidateSample t[] = {
			{L"PDH-PerCore-PerfBase", 3900.0, 3900.0, 3900.0, 16, true},
			{L"PDH-Total-PerfBase", 2493.0, 2493.0, 2493.0, 1, true},
			{L"PDH-ProcessorFrequency-Diagnostic", 3600.0, 3600.0, 3600.0, 16, true},
		};
		SampleWindow w[4];
		bool allNom = false;
		int idx = ChooseBestCandidate(t, 3, w[0], w[1], w[2], w[3], 2500.0, allNom);
		if(idx != 0 || allNom) return false;
	}
	return true;
}

}

class CpuHzDiagnosticLogger
{
	FILE* file_ = nullptr;
public:
	~CpuHzDiagnosticLogger() { Close(); }

	bool Open(const wchar_t* path)
	{
		if(_wfopen_s(&file_, path, L"w") != 0 || !file_)
			return false;
		fwprintf(file_, L"timeUtc,source,avgMHz,maxMHz,minMHz,baseMHz,validCoreCount,isNominalLike,pdhStatus,pdhCStatus\n");
		return true;
	}

	void Write(const std::wstring& timeUtc, const std::wstring& source,
			   double avgMHz, double maxMHz, double minMHz, double baseMHz,
			   int validCoreCount, bool isNominalLike,
			   long pdhStatus, unsigned long pdhCStatus)
	{
		if(!file_) return;
		fwprintf(file_, L"%s,%s,%.3f,%.3f,%.3f,%.3f,%d,%s,%ld,%lu\n",
			timeUtc.c_str(), source.c_str(),
			avgMHz, maxMHz, minMHz, baseMHz,
			validCoreCount,
			isNominalLike ? L"true" : L"false",
			pdhStatus, pdhCStatus);
		fflush(file_);
	}

	void Close()
	{
		if(file_)
		{
			fclose(file_);
			file_ = nullptr;
		}
	}
};

CpuFrequency::~CpuFrequency()
{
	delete diagnosticLogger_;
	diagnosticLogger_ = nullptr;

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
	InitPdh(); // best-effort; query_ stays null if PDH unavailable

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
	double& outMin,
	int& outCount,
	unsigned long& outCStatus,
	long& outStatus)
{
	outAvg = 0.0;
	outMax = 0.0;
	outMin = 0.0;
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
	double mn = 0.0;
	int count = 0;
	unsigned long lastStatus = 0;
	bool first = true;

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

		if(first)
		{
			mx = value;
			mn = value;
			first = false;
		}
		else
		{
			if(value > mx) mx = value;
			if(value < mn) mn = value;
		}
		sum += value;
		++count;
	}

	outCStatus = lastStatus;
	if(count <= 0)
		return false;

	outAvg = sum / count;
	outMax = mx;
	outMin = mn;
	outCount = count;
	return true;
}

bool CpuFrequency::TryReadPowerInformation(CpuReading& r)
{
	DWORD procCount = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
	if(procCount == 0)
		return false;

	std::vector<PROCESSOR_POWER_INFORMATION> ppi(procCount);
	ULONG size = (ULONG)(procCount * sizeof(PROCESSOR_POWER_INFORMATION));
	if(CallNtPowerInformation(ProcessorInformation, nullptr, 0, ppi.data(), size) != 0)
		return false;

	double sum = 0.0;
	double mx = 0.0;
	double mn = 0.0;
	int validCount = 0;
	bool first = true;

	for(DWORD i = 0; i < procCount; ++i)
	{
		DWORD mhz = ppi[i].CurrentMhz;
		if(mhz == 0)
			continue;

		double val = static_cast<double>(mhz);
		if(first)
		{
			mn = val;
			first = false;
		}
		sum += val;
		if(val > mx) mx = val;
		if(val < mn) mn = val;
		++validCount;
	}

	if(validCount == 0)
		return false;

	r.avgMHz = sum / validCount;
	r.maxMHz = mx;
	r.minMHz = mn;
	r.currentMHz = r.avgMHz;
	r.validCoreCount = validCount;
	r.ok = true;
	r.source = L"PowerInformation-CurrentMhz";
	r.baseMHz = baseMHz_;

	return true;
}

CpuReading CpuFrequency::Read()
{
	CpuReading r{};
	r.baseMHz = baseMHz_;

	CandidateSample candidates[4];
	int nCandidates = 0;

	// 1. PowerInformation (primary)
	{
		CpuReading pi;
		pi.baseMHz = baseMHz_;
		if(TryReadPowerInformation(pi))
		{
			candidates[nCandidates++] = {
				pi.source, pi.avgMHz, pi.maxMHz, pi.minMHz,
				pi.validCoreCount, true, 0, 0
			};
		}
	}

	// 2. PDH sources
	bool pdhCollectAttempted = false;
	bool pdhCollectFailed = false;
	if(query_)
	{
		auto s = PdhCollectQueryData(query_);
		lastPdhStatus_ = (long)s;
		pdhCollectAttempted = true;
		pdhCollectFailed = (s != ERROR_SUCCESS);

		if(!pdhCollectFailed)
		{
			// 2a. Per-core % Processor Performance * baseMHz
			{
				double avgPct = 0.0, maxPct = 0.0, minPct = 0.0;
				int count = 0;
				unsigned long cst = 0;
				long st = 0;
				if(baseMHz_ > 0.0 && perCorePerfPctCounter_ &&
					TryReadCounterArrayStats(perCorePerfPctCounter_, avgPct, maxPct, minPct, count, cst, st))
				{
					lastPdhCStatus_ = cst;
					lastPdhStatus_ = st;
					candidates[nCandidates++] = {
						L"PDH-PerCore-PerfBase",
						baseMHz_ * avgPct / 100.0,
						baseMHz_ * maxPct / 100.0,
						baseMHz_ * minPct / 100.0,
						count, true, st, cst
					};
				}
				else
				{
					lastPdhCStatus_ = cst;
					lastPdhStatus_ = st;
				}
			}

			// 2b. _Total % Processor Performance * baseMHz
			{
				double perfPct = 0.0;
				unsigned long cst = 0;
				long st = 0;
				if(baseMHz_ > 0.0 && totalPerfPctCounter_ &&
					TryReadDoubleCounter(totalPerfPctCounter_, perfPct, cst, st))
				{
					lastPdhCStatus_ = cst;
					lastPdhStatus_ = st;
					double mhz = baseMHz_ * perfPct / 100.0;
					candidates[nCandidates++] = {
						L"PDH-Total-PerfBase",
						mhz, mhz, mhz, 1, true, st, cst
					};
				}
				else
				{
					lastPdhCStatus_ = cst;
					lastPdhStatus_ = st;
				}
			}

			// 2c. PDH Processor Frequency (diagnostic)
			{
				double avg = 0.0, mx = 0.0, mn = 0.0;
				int count = 0;
				unsigned long cst = 0;
				long st = 0;
				if(perCoreFreqMHzCounter_ &&
					TryReadCounterArrayStats(perCoreFreqMHzCounter_, avg, mx, mn, count, cst, st))
				{
					lastPdhCStatus_ = cst;
					lastPdhStatus_ = st;
					candidates[nCandidates++] = {
						L"PDH-ProcessorFrequency-Diagnostic",
						avg, mx, mn, count, true, st, cst
					};
				}
				else
				{
					lastPdhCStatus_ = cst;
					lastPdhStatus_ = st;
				}
			}
		}
	}

	// 3. Push each candidate avgMHz to its per-source sample window
	for(int i = 0; i < nCandidates; ++i)
	{
		SampleWindow* w = nullptr;
		if(candidates[i].source == L"PowerInformation-CurrentMhz")
			w = &powerInfoWindow_;
		else if(candidates[i].source == L"PDH-PerCore-PerfBase")
			w = &perCorePerfWindow_;
		else if(candidates[i].source == L"PDH-Total-PerfBase")
			w = &totalPerfWindow_;
		else if(candidates[i].source == L"PDH-ProcessorFrequency-Diagnostic")
			w = &procFreqWindow_;

		if(w)
			w->Push(candidates[i].avgMHz);
	}

	// 4. Select best candidate (rules A, B, C)
	bool allNominalLike = true;
	int bestIdx = ChooseBestCandidate(candidates, nCandidates,
		powerInfoWindow_, perCorePerfWindow_, totalPerfWindow_, procFreqWindow_,
		baseMHz_, allNominalLike);

	// 5. Build result from selected candidate
	if(bestIdx >= 0)
	{
		auto& best = candidates[bestIdx];

		std::wstring srcName = best.source;
		if(allNominalLike)
			srcName += L"/NominalLike";

		lastGoodAvgMHz_ = best.avgMHz;
		lastGoodMaxMHz_ = best.maxMHz;
		lastGoodMinMHz_ = best.minMHz;
		lastGoodValidCoreCount_ = best.validCoreCount;
		lastGoodSource_ = srcName;

		r.avgMHz = best.avgMHz;
		r.maxMHz = best.maxMHz;
		r.minMHz = best.minMHz;
		r.currentMHz = best.avgMHz;
		r.validCoreCount = best.validCoreCount;
		r.ok = true;
		r.source = srcName;
		r.baseMHz = baseMHz_;
		r.lastPdhStatus = best.pdhStatus;
		r.lastPdhCStatus = best.pdhCStatus;
		r.nominalLike = allNominalLike;
		r.accuracy = L"WindowsEstimated";
		if(allNominalLike)
			r.warning = L"Values appear stuck near base frequency (NominalLike)";

		if(diagnosticLogger_)
		{
			auto ts = GetUtcTimestamp();
			diagnosticLogger_->Write(ts, r.source,
				r.avgMHz, r.maxMHz, r.minMHz, r.baseMHz,
				r.validCoreCount, r.nominalLike,
				r.lastPdhStatus, r.lastPdhCStatus);
		}

		return r;
	}

	// 6. No candidates: cached fallback
	if(lastGoodAvgMHz_ > 0.0)
	{
		r.avgMHz = lastGoodAvgMHz_;
		r.maxMHz = lastGoodMaxMHz_;
		r.minMHz = lastGoodMinMHz_;
		r.currentMHz = lastGoodAvgMHz_;
		r.validCoreCount = lastGoodValidCoreCount_;
		r.ok = true;
		r.source = lastGoodSource_;
		if(pdhCollectAttempted && pdhCollectFailed)
			r.source += L"/CollectFail";
		r.source += L"/Cached";
		r.baseMHz = baseMHz_;
		r.lastPdhStatus = lastPdhStatus_;
		r.lastPdhCStatus = lastPdhCStatus_;
		r.accuracy = L"WindowsEstimated";
		return r;
	}

	// 7. No data at all
	r.ok = false;
	r.lastPdhStatus = lastPdhStatus_;
	r.lastPdhCStatus = lastPdhCStatus_;
	r.baseMHz = baseMHz_;
	r.accuracy = L"WindowsEstimated";

	bool hasAnyCounter = perCoreFreqMHzCounter_ || perCorePerfPctCounter_ || totalPerfPctCounter_;
	if(!hasAnyCounter && !query_)
		r.source = L"NoUsableCounter";
	else if(baseMHz_ > 0.0)
		r.source = L"BaseOnly-NoCurrentHz";
	else
		r.source = L"BaseUnknown";

	return r;
}

void CpuFrequency::EnableDiagnosticLogger(const wchar_t* path)
{
	delete diagnosticLogger_;
	diagnosticLogger_ = nullptr;

	auto* logger = new CpuHzDiagnosticLogger();
	if(logger->Open(path))
		diagnosticLogger_ = logger;
	else
		delete logger;
}
