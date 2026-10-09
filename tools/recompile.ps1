# Regenerates ppc/ (the recompiled game code) from scratch.
#   1. XenonAnalyse        -> config/MCLA_switch_tables.toml
#   2. find_jump_tables.py -> appends tables XenonAnalyse missed
#   3. XenonRecomp into a fresh ppc/ (it never deletes stale output files)
#   4. fix_switch_functions.py + XenonRecomp again if any switch boundary errors
#   5. gen_import_stubs.py
# Usage: powershell -ExecutionPolicy Bypass -File tools\recompile.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$analyse = 'build\XenonRecomp\XenonAnalyse\XenonAnalyse.exe'
$recomp = 'build\XenonRecomp\XenonRecomp\XenonRecomp.exe'
$ctxHeader = 'thirdparty\XenonRecomp\XenonUtils\ppc_context.h'

& $analyse extracted\default.xex config\MCLA_switch_tables.toml | Out-Null
python tools\find_jump_tables.py

function Invoke-Recomp {
    if (Test-Path ppc) {
        if (Test-Path build\ppc_previous) { Remove-Item -Recurse -Force build\ppc_previous }
        Move-Item ppc build\ppc_previous
    }
    New-Item -ItemType Directory ppc | Out-Null
    & $recomp config\MCLA.toml $ctxHeader *> notes\xenonrecomp.log
    if ($LASTEXITCODE) { throw "XenonRecomp failed ($LASTEXITCODE)" }
}

Invoke-Recomp
for ($i = 0; $i -lt 4 -and (Select-String -Path notes\xenonrecomp.log -Pattern 'Switch case' -Quiet); $i++) {
    python tools\fix_switch_functions.py
    Invoke-Recomp
}
$messages = Get-Content notes\xenonrecomp.log | Where-Object { $_ -notmatch '^Recompiling functions' }
"XenonRecomp: $((Get-ChildItem ppc -File).Count) files, $($messages.Count) warnings"
$messages | Select-Object -First 10
python tools\gen_import_stubs.py
