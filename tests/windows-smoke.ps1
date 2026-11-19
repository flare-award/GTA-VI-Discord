#
# windows-smoke.ps1 - runs the real daemon on Windows and checks that it
# actually works: config parsing, logging, process detection (Toolhelp32),
# the single instance lock, the control pipe (--status/--stop), the mode
# switch and a clean shutdown.
#
# Usage: pwsh -File tests/windows-smoke.ps1 -Exe build\Release\GTAVI-Discord.exe
#
param(
    [Parameter(Mandatory = $true)][string]$Exe
)

$ErrorActionPreference = 'Stop'

function Fail([string]$message) {
    Write-Host "SMOKE FAIL: $message"
    if (Test-Path $logPath) {
        Write-Host "--- presence.log ---"
        Get-Content $logPath -Encoding utf8 | ForEach-Object { Write-Host $_ }
    }
    throw $message
}

$exePath = (Resolve-Path $Exe).Path
$work = Join-Path $env:TEMP ("gtavi-smoke-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $work -Force | Out-Null
$configPath = Join-Path $work "config.ini"
$logPath = Join-Path $work "presence.log"

# powershell.exe (Windows PowerShell 5.1) is used as the stand-in game: it is
# not running by default while the step itself runs under pwsh 7.
@"
[discord]
client_id = 123456789012345678
mode = pc

[activity]
large_image = gta6
large_text = Grand Theft Auto VI

[detection]
poll_ms = 500
processes = powershell.exe

[log]
level = debug
"@ | Set-Content -Path $configPath -Encoding utf8

$daemon = $null
$game = $null
try {
    $daemon = Start-Process -FilePath $exePath -ArgumentList "--config", "`"$configPath`"" -PassThru
    Start-Sleep -Seconds 3
    if ($daemon.HasExited) { Fail "daemon exited early with code $($daemon.ExitCode)" }
    if (-not (Test-Path $logPath)) { Fail "no presence.log was created" }

    $log = Get-Content $logPath -Raw -Encoding utf8
    if ($log -notmatch 'запуск v') { Fail "daemon did not log its startup" }

    # --status must talk to the running daemon through the control pipe
    $status = Start-Process -FilePath $exePath -ArgumentList "--status" -Wait -PassThru
    if ($status.ExitCode -ne 0) { Fail "--status exited with $($status.ExitCode)" }

    # start the "game"
    $game = Start-Process -FilePath "powershell.exe" -ArgumentList "-NoProfile", "-Command", "Start-Sleep -Seconds 60" -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 4
    $log = Get-Content $logPath -Raw -Encoding utf8
    if ($log -notmatch 'игра запущена') { Fail "the game process was not detected" }
    if ($log -notmatch 'Discord не запущен') { Fail "the daemon did not report that Discord is absent" }

    # the mode switch is persisted to the config file (ASCII marker)
    $switch = Start-Process -FilePath $exePath -ArgumentList "--mode", "playstation" -Wait -PassThru
    if ($switch.ExitCode -ne 0) { Fail "--mode exited with $($switch.ExitCode)" }
    Start-Sleep -Seconds 1
    if ((Get-Content $configPath -Raw) -notmatch 'mode = playstation') { Fail "mode was not persisted" }

    # stop the game
    Stop-Process -Id $game.Id -Force
    $game = $null
    Start-Sleep -Seconds 4
    $log = Get-Content $logPath -Raw -Encoding utf8
    if ($log -notmatch 'игра закрыта') { Fail "game exit was not detected" }

    # stop the daemon through the control pipe and make sure it really exits
    $stop = Start-Process -FilePath $exePath -ArgumentList "--stop" -Wait -PassThru
    if ($stop.ExitCode -ne 0) { Fail "--stop exited with $($stop.ExitCode)" }
    if (-not $daemon.WaitForExit(10000)) { Fail "daemon is still running after --stop" }
    if ($daemon.ExitCode -ne 0) { Fail "daemon exited with $($daemon.ExitCode)" }

    $log = Get-Content $logPath -Raw -Encoding utf8
    if ($log -notmatch 'stopped') { Fail "daemon did not log its shutdown" }

    Write-Host "windows smoke: ok"
}
finally {
    if ($game -and -not $game.HasExited) { Stop-Process -Id $game.Id -Force -ErrorAction SilentlyContinue }
    if ($daemon -and -not $daemon.HasExited) { Stop-Process -Id $daemon.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
