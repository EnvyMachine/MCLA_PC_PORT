# Runs the built MCLA.exe for a limited time and shows the end of its log.
# Usage: powershell -ExecutionPolicy Bypass -File tools\run_mcla.ps1 [-Seconds 45] [-Trace] [-Tail 60]
param(
    [int]$Seconds = 45,
    [switch]$Trace,
    [int]$Tail = 60,
    [string]$Config = 'relwithdebinfo',
    # Dump all guest thread stacks after this many seconds (0 = off).
    [int]$DumpAfter = 0
)
$root = Split-Path -Parent $PSScriptRoot
$exeDir = Join-Path $root "build\mcla-$Config\MCLA"
$exe = Join-Path $exeDir 'MCLA.exe'
$log = Join-Path $exeDir 'mcla.log'

if ($Trace) { $env:MCLA_TRACE = '1' } else { Remove-Item Env:MCLA_TRACE -ErrorAction SilentlyContinue }
if ($DumpAfter -gt 0) { $env:MCLA_DUMP_AFTER = "$DumpAfter" } else { Remove-Item Env:MCLA_DUMP_AFTER -ErrorAction SilentlyContinue }
$p = Start-Process -FilePath $exe -ArgumentList (Join-Path $root 'game') -WorkingDirectory $exeDir -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $root 'notes\run_stdout.txt') -RedirectStandardError (Join-Path $root 'notes\run_stderr.txt')
if (-not $p.WaitForExit($Seconds * 1000)) {
    "still running after $Seconds s - stopping"
    Stop-Process -Id $p.Id -Force
} else {
    "exited with code 0x{0:X8}" -f $p.ExitCode
}
Start-Sleep -Milliseconds 300
"log lines: " + (Get-Content $log | Measure-Object -Line).Lines
Get-Content $log -Tail $Tail
