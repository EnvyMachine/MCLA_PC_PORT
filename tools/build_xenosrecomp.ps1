# Builds the standalone XenosRecomp tool (shader container -> HLSL -> DXIL/SPIR-V)
# with clang-cl from VS 2022 Build Tools.
# Usage: powershell -ExecutionPolicy Bypass -File tools\build_xenosrecomp.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath
if (-not $vs) { throw 'VS Build Tools with the Clang component not found' }
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

$src = Join-Path $root 'thirdparty\XenosRecomp'
$bld = Join-Path $root 'build\XenosRecomp'
cmake -S $src -B $bld -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
if ($LASTEXITCODE) { throw "cmake configure failed ($LASTEXITCODE)" }
cmake --build $bld --target XenosRecomp
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }

Get-ChildItem -Recurse $bld -Include XenosRecomp.exe | Select-Object FullName
