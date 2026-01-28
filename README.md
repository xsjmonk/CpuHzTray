# CpuHzTray

A lightweight Windows tray application that displays **actual CPU frequency** in real time.

## Features
- Uses PDH counters (`% Processor Performance`)
- Correctly reflects turbo boost (>100%)
- Custom embedded font for readability
- Transparent tray icon
- No drivers required
- Refresh every 1 second

## Build
- Visual Studio 2022
- Win32 / x64
- C++

## Notes
Some systems report base frequency only via WMI.
This app computes effective frequency as:

baseMHz × (% Processor Performance / 100)
