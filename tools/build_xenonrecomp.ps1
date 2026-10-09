# Builds XenonRecomp + XenonAnalyse with clang-cl from VS 2022 Build Tools.
# Usage: powershell -ExecutionPolicy Bypass -File tools\build_xenonrecomp.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath
if (-not $vs) { throw 'VS Build Tools with the Clang component not found' }

# Enter the x64 developer environment (puts cl/clang-cl/cmake/ninja/SDK on PATH).
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

clang-cl --version | Select-Object -First 1
cmake --version | Select-Object -First 1

$src = Join-Path $root 'thirdparty\XenonRecomp'
$bld = Join-Path $root 'build\XenonRecomp'
cmake -S $src -B $bld -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
if ($LASTEXITCODE) { throw "cmake configure failed ($LASTEXITCODE)" }
cmake --build $bld --target XenonRecomp XenonAnalyse
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }

Get-ChildItem -Recurse $bld -Include XenonRecomp.exe, XenonAnalyse.exe | Select-Object FullName
