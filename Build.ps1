param(
    [string]$Configuration = "Release",
    [string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Get-ScriptDirectory {
    return Split-Path -Parent $PSCommandPath
}

function Write-Step {
    param([string]$Message)
    Write-Host "[Build] $Message" -ForegroundColor Yellow
}

function Write-Success {
    param([string]$Message)
    Write-Host "[Build] $Message" -ForegroundColor Green
}

function Write-ErrorStop {
    param([string]$Message)
    Write-Host "[Build] ERROR: $Message" -ForegroundColor Red
    exit 1
}

function Remove-DirectoryTree {
    param([string]$Path)

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

function Clear-DirectoryContents {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
        return
    }

    Get-ChildItem -LiteralPath $Path -Force -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
}

function Find-MSBuild {
    Write-Host "Searching for MSBuild..." -ForegroundColor Yellow

    $vsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vsWhere) {
        $instances = & $vsWhere -all -format json 2>&1 | ConvertFrom-Json
        if ($instances) {
            $sorted = $instances | Sort-Object -Property installationVersion -Descending
            foreach ($instance in $sorted) {
                foreach ($sub in @("MSBuild\Current\Bin\MSBuild.exe", "MSBuild\15.0\Bin\MSBuild.exe")) {
                    $msbuild = Join-Path $instance.installationPath $sub
                    if (Test-Path $msbuild) {
                        Write-Host "  Found (vswhere): $msbuild" -ForegroundColor Green
                        return $msbuild
                    }
                }
            }
        }
    }

    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\MSBuild\Current\Bin\MSBuild.exe",
        "C:\Program Files (x86)\MSBuild\14.0\Bin\MSBuild.exe",
        "C:\WINDOWS\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            Write-Host "  Found (hardcoded): $candidate" -ForegroundColor Green
            return $candidate
        }
    }

    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($command) {
        Write-Host "  Found (PATH): $($command.Source)" -ForegroundColor Yellow
        return $command.Source
    }

    throw "MSBuild.exe was not found. Install Visual Studio Build Tools or Visual Studio."
}

function Invoke-MSBuild {
    param(
        [Parameter(Mandatory)]
        [string]$MsBuildPath,
        [Parameter(Mandatory)]
        [string]$ProjectPath,
        [string]$Configuration = "Release",
        [string]$Platform = "AnyCPU",
        [string[]]$ExtraArgs
    )

    $msbuildArgs = @(
        $ProjectPath
        "/t:Build"
        "/p:Configuration=$Configuration"
        "/p:Platform=$Platform"
        "/consoleloggerparameters:Summary"
        "/noLogo"
    )

    if ($ExtraArgs) {
        $msbuildArgs += $ExtraArgs
    }

    Write-Host "Building via $MsBuildPath" -ForegroundColor Cyan
    $output = & $MsBuildPath @msbuildArgs 2>&1
    $output | ForEach-Object { Write-Host $_ }

    if ($LASTEXITCODE -ne 0) {
        Write-Host "MSBuild exited with code $LASTEXITCODE. Full output above." -ForegroundColor Red
        throw "Build failed with exit code $LASTEXITCODE"
    }
}

function Remove-GeneratedArtifacts {
    param([string]$RootPath)

    $directoryNames = @("obj", "ipch")
    $directories = Get-ChildItem -Path $RootPath -Directory -Recurse -Force -ErrorAction SilentlyContinue |
        Where-Object { $directoryNames -contains $_.Name -and $_.FullName -notlike "$buildDir*" } |
        Sort-Object FullName -Descending

    foreach ($directory in $directories) {
        Remove-DirectoryTree -Path $directory.FullName
    }

    $filePatterns = @("*.tlog", "*.lastbuildstate", "*.unsuccessfulbuild", "*.obj", "*.idb", "*.ilk", "*.pch", "*.pdb")
    foreach ($pattern in $filePatterns) {
        Get-ChildItem -Path $RootPath -File -Recurse -Force -Filter $pattern -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -notlike "$buildDir*" } |
            Remove-Item -Force -ErrorAction SilentlyContinue
    }
}

$ScriptDir = Get-ScriptDirectory
$solutionPath = Join-Path $ScriptDir "CpuHzTrayCpp.sln"
$buildDir = Join-Path $ScriptDir "Build"
$tempRoot = Join-Path $ScriptDir ".build"
$intermediateDir = Join-Path $tempRoot "obj"

Write-Host "=== Building CpuHzTrayCpp ===" -ForegroundColor Cyan

if (-not (Test-Path $solutionPath)) {
    Write-ErrorStop "Solution not found: $solutionPath"
}

$msbuildPath = Find-MSBuild
Write-Step "Using MSBuild: $msbuildPath"

Write-Step "Preparing Build folder..."
Clear-DirectoryContents -Path $buildDir

Write-Step "Cleaning previous intermediate files..."
Remove-DirectoryTree -Path $tempRoot

Remove-GeneratedArtifacts -RootPath $ScriptDir
New-Item -ItemType Directory -Path $intermediateDir -Force | Out-Null

try {
    Write-Step "Building ($Configuration|$Platform)..."
    & $msbuildPath $solutionPath `
        /t:Build `
        /m `
        "/p:Configuration=$Configuration" `
        "/p:Platform=$Platform" `
        "/p:OutDir=$buildDir\" `
        "/p:IntDir=$intermediateDir\" `
        "/p:BaseIntermediateOutputPath=$tempRoot\" `
        /verbosity:minimal

    if ($LASTEXITCODE -ne 0) {
        Write-ErrorStop "Build failed with exit code $LASTEXITCODE."
    }

    Write-Step "Cleaning up..."
    Remove-DirectoryTree -Path $tempRoot
    Remove-GeneratedArtifacts -RootPath $ScriptDir

    Write-Step "Removing PDB files from Build folder..."
    Get-ChildItem -Path $buildDir -Filter "*.pdb" -Recurse -File | Remove-Item -Force -ErrorAction SilentlyContinue

    Write-Step "Removing x64 intermediate folder..."
    Remove-DirectoryTree -Path (Join-Path $ScriptDir "x64")

    Write-Success "=== Build completed successfully ==="
    Write-Host "[Build] Artifacts: $buildDir" -ForegroundColor Cyan
}
catch {
    Write-ErrorStop $_.Exception.Message
}
