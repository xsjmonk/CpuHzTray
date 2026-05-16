# CpuHzTray

A lightweight Windows tray application that displays **actual CPU frequency** in real time.

## Features
- Adaptive multi-source sampling with NominalLike detection (see strategy below)
- Custom embedded font for readability
- Transparent tray icon with sparkline history
- No drivers, admin rights, MSR access, or external dependencies required
- Refresh every 1 second

## How frequency is measured

The app collects ALL available sources every second, then applies
NominalLike detection and priority-based selection:

### Candidate sources (in priority order)

1. **`CallNtPowerInformation(ProcessorInformation)` / `CurrentMhz`**.
   Primary source. Reads per-logical-processor `CurrentMhz` from Windows
   power management. No base MHz needed. On some PCs this returns the
   nominal/base frequency instead of the live effective clock.

2. **Per-core `% Processor Performance` × WMI `MaxClockSpeed`**.
   Percentage-based counter that can exceed 100% on turbo-capable systems.
   Aggregate instances (`_Total`, `0,_Total`) are excluded. Requires
   `Win32_Processor.MaxClockSpeed`.

3. **`_Total % Processor Performance` × WMI `MaxClockSpeed`**.
   Single-value aggregate when per-core counters are unavailable. Requires
   `Win32_Processor.MaxClockSpeed`.

4. **PDH `Processor Frequency` (diagnostic fallback)**.
   Raw PDH frequency counter. On some PCs this returns nominal/base values
   and does **not** track live frequency changes. Used only when all higher-
   priority sources are NominalLike or unavailable.

### Source selection rules

The app collects all available sources every sample, then applies these
rules in order:

**Rule 1 — Immediate below-base actual wins over base-like direct source.**
If `PowerInformation-CurrentMhz` is within `max(75 MHz, baseMHz × 0.035)`
of base (i.e. base-like) and another valid non-PowerInformation candidate
reports `avgMHz < baseMHz − max(10 MHz, baseMHz × 0.005)` (below base),
the below-base source is selected immediately, without waiting for 6
NominalLike samples. Priority: (a) per-core `% Processor Performance`,
(b) `_Total % Processor Performance`, (c) diagnostic `Processor Frequency`.

**Rule 2 — Immediate materially-different live candidate wins over base-like direct source.**
If `PowerInformation-CurrentMhz` is base-like and another source differs
from base by more than `max(75 MHz, baseMHz × 0.035)`, select it
immediately (same priority order). This catches both turbo and below-base
cases where a live source varies while PowerInformation is stuck at base.

**Rule 3 — NominalLike fallback priority.**
If neither rule 1 nor 2 selects a candidate, fall back to the original
priority order:
1. `PowerInformation-CurrentMhz`
2. `PDH-PerCore-PerfBase`
3. `PDH-Total-PerfBase`
4. `PDH-ProcessorFrequency-Diagnostic`

Within this list, a per-source rolling window (10 samples) tracks recent
readings. A source is **NominalLike** when `baseMHz > 0`, the window has
at least 6 samples, and all 6 most recent samples fall within
`max(75 MHz, baseMHz × 0.035)` of base. The first non-NominalLike source
in priority order is selected. If all are NominalLike, the highest-priority
available source is selected and `/NominalLike` is appended to the source
name. A **Warning** line appears in the tooltip when the displayed value
is NominalLike.

### Cached fallback

If no source returns valid data and a previous reading exists, the cached
value is shown with a `/Cached` suffix. When PDH collection itself fails,
the suffix becomes `/CollectFail/Cached`.

## Diagnostic mode

Run with `--diagnose-hz` to write a CSV log (`candidate_readings.csv`) of
every sample to the working directory. Each row includes the selected
source, its values, and whether it was classified as NominalLike.

```
timeUtc,source,avgMHz,maxMHz,minMHz,baseMHz,validCoreCount,isNominalLike,pdhStatus,pdhCStatus
```

This is useful for comparing which sources vary under load on your system.

## Limitations
- This app uses only Windows API (`CallNtPowerInformation`), PDH
  performance counters, and WMI. It does **not** use kernel drivers, MSR
  registers, or APERF/MPERF effective-clock sampling.
- Values are **Windows-estimated**, not hardware-measured. Tools that
  access hardware registers directly (e.g. TopCpu, MSR-based readers) may
  show different values.
- On some systems ALL Windows APIs may report nominal/base frequency
  instead of the live effective clock. In this case the tray will show
  `/NominalLike` in the source name and a warning in the tooltip.
- `Win32_Processor.MaxClockSpeed` is read once at startup. Frequency
  changes due to overclocking or power plan changes after startup may not
  be reflected in the base MHz.

## Build
- Visual Studio 2022
- Win32 / x64
- C++
