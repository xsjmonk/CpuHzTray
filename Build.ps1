param(
    [string]$Configuration = "Release",
    [string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$solutionPath = Join-Path $repoRoot "CpuHzTrayCpp.sln"
$buildDir = Join-Path $repoRoot "Build"
$tempRoot = Join-Path $repoRoot ".build"
$intermediateDir = Join-Path $tempRoot "obj"

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

function Clear-DirectoryContents {
    param(
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
        return
    }

    Get-ChildItem -LiteralPath $Path -Force -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
}

function Get-MSBuildPath {
    $msbuildCommand = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($msbuildCommand) {
        return $msbuildCommand.Source
    }

    $vsWherePath = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vsWherePath) {
        $installationPath = & $vsWherePath -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
        if ($installationPath) {
            return $installationPath
        }
    }

    throw "MSBuild.exe was not found. Install Visual Studio Build Tools or Visual Studio with C++ build support."
}

function Remove-GeneratedArtifacts {
    param(
        [string]$RootPath
    )

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

if (-not (Test-Path $solutionPath)) {
    throw "Solution not found: $solutionPath"
}

$msbuildPath = Get-MSBuildPath

Clear-DirectoryContents -Path $buildDir

Remove-DirectoryTree -Path $tempRoot

Remove-GeneratedArtifacts -RootPath $repoRoot
New-Item -ItemType Directory -Path $intermediateDir -Force | Out-Null

& $msbuildPath $solutionPath `
    /t:Build `
    /m `
    /p:Configuration=$Configuration `
    /p:Platform=$Platform `
    /p:OutDir="$buildDir\" `
    /p:IntDir="$intermediateDir\" `
    /p:BaseIntermediateOutputPath="$tempRoot\" `
    /verbosity:minimal

if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}

Remove-DirectoryTree -Path $tempRoot

Remove-GeneratedArtifacts -RootPath $repoRoot

Write-Host "Build completed successfully."
Write-Host "Artifacts: $buildDir"
