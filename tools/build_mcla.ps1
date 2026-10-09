# Builds the MCLA recompilation project with clang-cl from VS 2022 Build Tools.
# Usage: powershell -ExecutionPolicy Bypass -File tools\build_mcla.ps1 [-Config Release] [-Jobs 6]
param(
    # RelWithDebInfo keeps symbols so crash reports name the sub_XXXXXXXX functions.
    [string]$Config = 'RelWithDebInfo',
    # Each translation unit is ~1 MB of generated C++; keep parallelism modest on 16 GB RAM.
    [int]$Jobs = 6
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath
if (-not $vs) { throw 'VS Build Tools with the Clang component not found' }
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

$bld = Join-Path $root "build\mcla-$($Config.ToLower())"
cmake -S $root -B $bld -G Ninja "-DCMAKE_BUILD_TYPE=$Config" -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link
if ($LASTEXITCODE) { throw "cmake configure failed ($LASTEXITCODE)" }
cmake --build $bld --parallel $Jobs
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
