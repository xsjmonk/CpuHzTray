Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$sourceRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$destinationRoot = (Get-Location).Path
$stagingRoot = Join-Path $destinationRoot "code"
$zipPath = Join-Path $destinationRoot "code.zip"

$excludedDirectoryNames = @(
    ".git",
    ".vs",
    "Build",
    "bin",
    "obj",
    "x64",
    "x86",
    "Debug",
    "Release",
    "CMakeFiles"
)

$excludedFilePatterns = @(
    "*.exe",
    "*.dll",
    "*.pdb",
    "*.obj",
    "*.idb",
    "*.ilk",
    "*.ipdb",
    "*.iobj",
    "*.pch",
    "*.tlog",
    "*.lastbuildstate",
    "*.unsuccessfulbuild",
    "*.user",
    "*.suo",
    "*.VC.db",
    "*.VC.VC.opendb",
    "*.log",
    "*.dmp",
    "*.zip",
    "*.7z",
    "code.zip"
)

function Remove-DirectoryTree {
    param(
        [string]$Path
    )

    if (-not (Test-Path $Path)) {
        return
    }

    try {
        Remove-Item -Path $Path -Recurse -Force -ErrorAction Stop
    }
    catch {
        & cmd.exe /c "rmdir /s /q `"$Path`"" | Out-Null
    }
}

function Test-ExcludedPath {
    param(
        [string]$RelativePath,
        [bool]$IsDirectory
    )

    $normalizedPath = $RelativePath -replace "/", "\"
    $segments = $normalizedPath.Split("\", [System.StringSplitOptions]::RemoveEmptyEntries)

    foreach ($segment in $segments) {
        if ($excludedDirectoryNames -contains $segment) {
            return $true
        }
    }

    if (-not $IsDirectory) {
        foreach ($pattern in $excludedFilePatterns) {
            if ([System.Management.Automation.WildcardPattern]::Get($pattern, "IgnoreCase").IsMatch([System.IO.Path]::GetFileName($normalizedPath))) {
                return $true
            }
        }
    }

    return $false
}

function Copy-SourceTree {
    param(
        [string]$RootPath
    )

    $rootLength = $RootPath.TrimEnd("\").Length
    $items = Get-ChildItem -Path $RootPath -Recurse -Force

    foreach ($item in $items) {
        $relativePath = $item.FullName.Substring($rootLength).TrimStart("\")
        if ([string]::IsNullOrWhiteSpace($relativePath)) {
            continue
        }

        $isDirectory = $item.PSIsContainer
        if (Test-ExcludedPath -RelativePath $relativePath -IsDirectory $isDirectory) {
            continue
        }

        $destinationPath = Join-Path $stagingRoot $relativePath

        if ($isDirectory) {
            New-Item -ItemType Directory -Path $destinationPath -Force | Out-Null
            continue
        }

        $parentDirectory = Split-Path -Parent $destinationPath
        if (-not (Test-Path $parentDirectory)) {
            New-Item -ItemType Directory -Path $parentDirectory -Force | Out-Null
        }

        Copy-Item -Path $item.FullName -Destination $destinationPath -Force
    }
}

Remove-DirectoryTree -Path $stagingRoot
if (Test-Path $zipPath) {
    Remove-Item -Path $zipPath -Force
}

New-Item -ItemType Directory -Path $stagingRoot -Force | Out-Null
Copy-SourceTree -RootPath $sourceRoot

Compress-Archive -Path (Join-Path $stagingRoot "*") -DestinationPath $zipPath -CompressionLevel Optimal

Remove-DirectoryTree -Path $stagingRoot

Write-Host "Copied source tree to archive: $zipPath"
