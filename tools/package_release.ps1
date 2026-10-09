# Updates (or creates) the self-contained release folder from the current build.
# Program files are replaced; game\ (hard links to the game data, no extra space),
# userdata\ (saves), MCLA.ini and shader_cache\ are created only if missing.
# Usage: powershell -ExecutionPolicy Bypass -File tools\package_release.ps1 [-Dest <folder>]
param(
    [string]$Dest,
    [string]$Config = 'relwithdebinfo',
    [string]$GameData,
    # Saves to start a new release folder with (copied, never moved).
    [string]$Saves
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $Dest) { $Dest = Join-Path $root 'release\Midnight Club Los Angeles' }
if (-not $GameData) { $GameData = Join-Path $root 'game' }
if (-not $Saves) { $Saves = Join-Path $root 'dist\MCLA-test\userdata' }
$build = Join-Path $root "build\mcla-$Config\MCLA"

New-Item -ItemType Directory -Force $Dest | Out-Null
Copy-Item (Join-Path $build 'MCLA.exe') (Join-Path $Dest 'Midnight Club Los Angeles.exe') -Force
foreach ($f in 'MCLA.pdb', 'dxcompiler.dll', 'dxil.dll') { Copy-Item (Join-Path $build $f) $Dest -Force }
New-Item -ItemType Directory -Force (Join-Path $Dest 'ffmpeg') | Out-Null
Copy-Item (Join-Path $build 'ffmpeg\*') (Join-Path $Dest 'ffmpeg') -Force

if (-not (Test-Path (Join-Path $Dest 'shader_cache'))) {
    New-Item -ItemType Directory (Join-Path $Dest 'shader_cache') | Out-Null
    Copy-Item (Join-Path $build 'shader_cache\*.bin') (Join-Path $Dest 'shader_cache')
}
if (-not (Test-Path (Join-Path $Dest 'userdata')) -and (Test-Path $Saves)) {
    Copy-Item -Recurse $Saves (Join-Path $Dest 'userdata')
}
if (-not (Test-Path (Join-Path $Dest 'game\default.xex'))) {
    foreach ($f in Get-ChildItem $GameData -Recurse -File) {
        $target = Join-Path (Join-Path $Dest 'game') $f.FullName.Substring($GameData.Length + 1)
        New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
        New-Item -ItemType HardLink -Path $target -Target $f.FullName | Out-Null
    }
}
"release updated: $Dest"
