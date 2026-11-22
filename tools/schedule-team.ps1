param(
    [Parameter(Mandatory)][string]$Queue,
    [Parameter(Mandatory)][string]$Backends,
    [Parameter(Mandatory)][string]$TaskName,
    [ValidateRange(5,120)][int]$DurationMinutes = 60,
    [ValidateRange(1,12)][int]$MaxJobs = 3,
    [ValidateRange(30,1800)][int]$WindowSeconds = 600,
    [switch]$StartNow
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$python = (Get-Command python -ErrorAction Stop).Source
$queuePath = (Resolve-Path -LiteralPath $Queue).Path
$backendPath = (Resolve-Path -LiteralPath $Backends).Path
$runner = Join-Path $PSScriptRoot 'team_queue.py'
$stopFile = Join-Path (Split-Path $queuePath -Parent) 'STOP'
foreach ($value in @($queuePath, $backendPath, $runner, $stopFile)) {
    if ($value -match '["\r\n]') { throw 'Unsupported character in argument path' }
}
if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
    throw 'A task with this name already exists; no task was replaced'
}
$arguments = '-X utf8 "{0}" run "{1}" "{2}" --max-jobs {3} --seconds {4} --stop-file "{5}"' -f `
    $runner, $queuePath, $backendPath, $MaxJobs, $WindowSeconds, $stopFile
$action = New-ScheduledTaskAction -Execute $python -Argument $arguments -WorkingDirectory $root
$start = (Get-Date).AddMinutes(1)
$trigger = New-ScheduledTaskTrigger -Once -At $start `
    -RepetitionInterval (New-TimeSpan -Minutes 5) -RepetitionDuration (New-TimeSpan -Minutes $DurationMinutes)
$trigger.EndBoundary = $start.AddMinutes($DurationMinutes).ToString('s')
$trigger.Repetition.StopAtDurationEnd = $true
$settings = New-ScheduledTaskSettingsSet -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit (New-TimeSpan -Seconds ($WindowSeconds + 60)) -StartWhenAvailable
$principal = New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) `
    -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger `
    -Settings $settings -Principal $principal `
    -Description 'Finite Daytona review queue; explicit tasks only; stop on error, no retries or purchases.' | Out-Null
if ($StartNow) { Start-ScheduledTask -TaskName $TaskName }
Get-ScheduledTask -TaskName $TaskName | Select-Object TaskName, State, Actions, Triggers
