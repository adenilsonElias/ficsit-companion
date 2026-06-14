<#
.SYNOPSIS
    One-command desktop build for ficsit-companion on Windows.

.DESCRIPTION
    Checks that a C++ toolchain and CMake are available, then configures and
    builds the desktop app using the "windows" CMake preset. If a prerequisite
    is missing, it prints exactly how to install it and stops — it never installs
    anything itself and never requires administrator rights.

    Just run it from the repo root:
        ./build.ps1
#>

$ErrorActionPreference = "Stop"

function Write-Step($message) { Write-Host "==> $message" -ForegroundColor Cyan }
function Write-Problem($message) { Write-Host $message -ForegroundColor Yellow }

$repoRoot = $PSScriptRoot
$missing = $false

# --- Check for CMake ---------------------------------------------------------
Write-Step "Checking for CMake..."
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($null -eq $cmake) {
    Write-Problem "CMake was not found on PATH."
    Write-Problem "Install it with one of:"
    Write-Problem "    winget install Kitware.CMake"
    Write-Problem "  or install Visual Studio 2022 with the 'Desktop development with C++'"
    Write-Problem "  workload, which bundles CMake."
    $missing = $true
}
else {
    Write-Host "    found: $($cmake.Source)"
}

# --- Check for a C++ compiler (Visual Studio) --------------------------------
Write-Step "Checking for a C++ compiler..."
$hasCompiler = $false

# `cl` on PATH means we're already in a developer environment.
if (Get-Command cl -ErrorAction SilentlyContinue) {
    $hasCompiler = $true
    Write-Host "    found: cl on PATH"
}
else {
    # Otherwise look for an installed Visual Studio via vswhere.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath
        if ($vsPath) {
            $hasCompiler = $true
            Write-Host "    found: Visual Studio at $vsPath"
        }
    }
}

if (-not $hasCompiler) {
    Write-Problem "No C++ compiler (MSVC) was found."
    Write-Problem "Install Visual Studio 2022 with the 'Desktop development with C++' workload:"
    Write-Problem "    winget install Microsoft.VisualStudio.2022.BuildTools --override `"--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended`""
    Write-Problem "  (or the full Visual Studio 2022 Community edition with the same workload)."
    $missing = $true
}

if ($missing) {
    Write-Problem ""
    Write-Problem "Install the prerequisite(s) above, open a new terminal, and run ./build.ps1 again."
    exit 1
}

# --- Configure & build -------------------------------------------------------
Write-Step "Configuring (cmake --preset windows)..."
cmake --preset windows -S $repoRoot

Write-Step "Building (cmake --build --preset windows)..."
cmake --build --preset windows

# --- Report where the exe landed --------------------------------------------
$exe = Join-Path $repoRoot "build\ficsit-companion\Release\ficsit-companion.exe"
Write-Step "Build complete."
if (Test-Path $exe) {
    Write-Host "Run the app with:" -ForegroundColor Green
    Write-Host "    $exe" -ForegroundColor Green
}
else {
    Write-Host "Look for ficsit-companion.exe under build\ficsit-companion\Release\" -ForegroundColor Green
}
