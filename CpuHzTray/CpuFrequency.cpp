#include "CpuFrequency.h"

#include <wbemidl.h>
#include <comdef.h>

#include <algorithm>
#include <optional>

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

CpuFrequency::~CpuFrequency()
{
	if(query_)
	{
		PdhCloseQuery(query_);
		query_ = nullptr;
	}
	perfPctCounter_ = nullptr;
	freqMHzCounter_ = nullptr;
}

bool CpuFrequency::Initialize()
{
	InitBaseWmi();  // best effort
	InitPdh();      // required for perf% (and also provides a fallback base)
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

	// Turbo-capable indicator (can exceed 100 on many systems).
	s = PdhAddEnglishCounterW(query_, L"\\Processor Information(_Total)\\% Processor Performance", 0, &perfPctCounter_);
	if(s != ERROR_SUCCESS || !perfPctCounter_)
	{
		PdhCloseQuery(query_);
		query_ = nullptr;
		return false;
	}

	// Often stuck at base on some systems, but still useful as a *fallback base MHz* if WMI fails.
	s = PdhAddEnglishCounterW(query_, L"\\Processor Information(_Total)\\Processor Frequency", 0, &freqMHzCounter_);
	if(s != ERROR_SUCCESS || !freqMHzCounter_)
	{
		// Keep perfPctCounter_ but proceed without freq counter.
		freqMHzCounter_ = nullptr;
	}

	// Prime: PDH may need multiple collections before returning valid data.
	PdhCollectQueryData(query_);
	Sleep(50);
	PdhCollectQueryData(query_);

	lastGoodPerfPct_ = 0.0;
	lastGoodFreqMHz_ = 0.0;
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

CpuReading CpuFrequency::Read()
{
	CpuReading r{};
	r.baseMHz = baseMHz_;

	if(!query_ || !perfPctCounter_)
	{
		r.ok = false;
		r.source = L"PDH-NotReady";
		return r;
	}

	auto s = PdhCollectQueryData(query_);
	lastPdhStatus_ = (long)s;
	if(s != ERROR_SUCCESS)
	{
		r.ok = false;
		r.source = L"PDH-CollectFail";
		r.lastPdhStatus = lastPdhStatus_;
		return r;
	}

	// 1) Update fallback base from "Processor Frequency" if WMI base is missing.
	double freqMHz = 0.0;
	unsigned long cst = 0;
	long st = 0;
	if(freqMHzCounter_ && TryReadDoubleCounter(freqMHzCounter_, freqMHz, cst, st))
	{
		if(freqMHz > 0.0) lastGoodFreqMHz_ = freqMHz;
	}

	if(baseMHz_ <= 0.0 && lastGoodFreqMHz_ > 0.0)
		baseMHz_ = lastGoodFreqMHz_;

	// 2) Read % Processor Performance (can exceed 100).
	double perfPct = 0.0;
	cst = 0;
	st = 0;
	if(TryReadDoubleCounter(perfPctCounter_, perfPct, cst, st))
	{
		if(perfPct > 0.0) lastGoodPerfPct_ = perfPct;
		lastPdhCStatus_ = cst;
		lastPdhStatus_ = st;
	}

	r.lastPdhStatus = lastPdhStatus_;
	r.lastPdhCStatus = lastPdhCStatus_;
	r.baseMHz = baseMHz_;

	// 3) Produce reading even if perf% is missing (never show 0.0 unless base is unknown).
	if(baseMHz_ <= 0.0)
	{
		r.ok = false;
		r.source = L"BaseUnknown";
		return r;
	}

	double usedPct = lastGoodPerfPct_;
	if(usedPct <= 0.0) usedPct = 100.0;

	r.currentMHz = baseMHz_ * (usedPct / 100.0);
	r.ok = true;
	r.source = (lastGoodPerfPct_ > 0.0) ? L"PDH-%Perf" : L"PDH-BaseOnly";
	return r;
}
