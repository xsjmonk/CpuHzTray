# CpuHzTray

A lightweight Windows tray application that displays **actual CPU frequency** in real time.

## Features
- Adaptive PDH counter sampling (see strategy below)
- Custom embedded font for readability
- Transparent tray icon with sparkline history
- No drivers, admin rights, MSR access, or external dependencies required
- Refresh every 1 second

## How frequency is measured

The app uses a priority-ordered adaptive strategy. It tries each source in
order and uses the first one that returns valid data:

1. **Per-core `Processor Frequency`** (direct effective MHz per logical
   processor). This is the primary source when available. Average and max
   across valid cores are reported. Aggregate instances like `_Total` or
   `0,_Total` are excluded. No base MHz is needed.

2. **Per-core `% Processor Performance` × WMI `MaxClockSpeed`**. Used when
   direct frequency counters are unavailable. Value can exceed 100% on
   turbo-capable systems. Aggregate instances are excluded. Requires
   `Win32_Processor.MaxClockSpeed`.

3. **`_Total % Processor Performance` × WMI `MaxClockSpeed`**. Used when
   per-core counters are unavailable. Single value, no per-core detail.
   Requires `Win32_Processor.MaxClockSpeed`.

4. **Last-good cached reading**. Preserved across transient PDH collection
   failures to avoid showing `0.00 GHz`.

If no source returns valid data and a cached reading exists, the cached
value is shown with a `/Cached` suffix in the tooltip. When the PDH
collection itself fails, the source suffix becomes `/Cached/CollectFail`.

## Limitations
- This app uses only Windows PDH performance counters and WMI for base
  frequency. It does **not** use kernel drivers, MSR registers, or
  APERF/MPERF effective-clock sampling.
- Values may differ from tools that access hardware registers directly.
- `Win32_Processor.MaxClockSpeed` is required only for the percentage-based
  fallback modes. The direct `Processor Frequency` counter works without it.
- On some systems the per-core `Processor Frequency` counter is not
  available and only the percentage-based fallback works.
- `Win32_Processor.MaxClockSpeed` is read once at startup. Frequency
  changes due to overclocking or power plan changes after startup may not
  be reflected in the base MHz.

## Build
- Visual Studio 2022
- Win32 / x64
- C++
