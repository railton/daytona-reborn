param(
    [Parameter(Mandatory)][string]$Database,
    [Parameter(Mandatory)][string]$Backends,
    [Parameter(Mandatory)][string]$Artifacts,
    [string]$TaskName = 'Daytona-Native-Project',
    [ValidateRange(10,120)][int]$IntervalMinutes = 20,
    [ValidateRange(1,4)][int]$MaxTasks = 4,
    [ValidateRange(60,1800)][int]$Seconds = 1800,
    [switch]$StartNow
)
$ErrorActionPreference='Stop'
$python=(Get-Command python -ErrorAction Stop).Source
$root=Split-Path $PSScriptRoot -Parent
$db=(Resolve-Path -LiteralPath $Database).Path
$cfg=(Resolve-Path -LiteralPath $Backends).Path
$output=[IO.Path]::GetFullPath($Artifacts)
$runner=Join-Path $PSScriptRoot 'team_worker.py'
foreach($path in @($db,$cfg,$output,$runner)) {
    if($path -match '["\r\n]') { throw 'Unsupported path character' }
}
if(Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
    throw 'Task already exists; inspect before updating it'
}
$arguments='-X utf8 "{0}" "{1}" "{2}" "{3}" --max-tasks {4} --seconds {5} --origin scheduled' -f $runner,$db,$cfg,$output,$MaxTasks,$Seconds
$action=New-ScheduledTaskAction -Execute $python -Argument $arguments -WorkingDirectory $root
# No project end date. Each invocation and the daily submission count remain bounded.
$trigger=New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(1) -RepetitionInterval (New-TimeSpan -Minutes $IntervalMinutes)
$settings=New-ScheduledTaskSettingsSet -MultipleInstances IgnoreNew -StartWhenAvailable -ExecutionTimeLimit (New-TimeSpan -Seconds ($Seconds+600))
$principal=New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Description 'Persistent Daytona backlog; bounded workers, daily budget, STOP and uncertain-dispatch quarantine.' | Out-Null
if($StartNow) { Start-ScheduledTask -TaskName $TaskName }
Get-ScheduledTask -TaskName $TaskName | Select-Object TaskName,State,Triggers
