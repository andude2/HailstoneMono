param([Parameter(Mandatory = $true)][string]$Assembly)
$ErrorActionPreference = "Stop"
[Reflection.Assembly]::Load([IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Assembly).Path)) | Out-Null
function Invoke-Turn([string]$action, [int]$target, [int]$endurance = 100) {
    $query = @(
        $action,
        ('${Me.Class.ShortName}' + "`tWAR"),
        ('${Me.Dead}' + "`tFALSE"),
        ('${Me.Hovering}' + "`tFALSE"),
        ('${Me.Stunned}' + "`tFALSE"),
        ('${Me.Combat}' + "`tFALSE"),
        ('${Me.PctEndurance}' + "`t" + $endurance),
        ('${Target.ID}' + "`t" + $target),
        ('${Target.Distance3D}' + "`t8"),
        (('${Spawn[id ' + $target + '].ID}') + "`t" + $target),
        (('${Spawn[id ' + $target + '].Type}') + "`tNPC"),
        (('${Spawn[id ' + $target + '].Dead}') + "`tFALSE"),
        (('${Spawn[id ' + $target + '].Distance3D}') + "`t8")
    )
    $submitted = [Sentinel.HailstoneWarriorTrial]::OnPulse(($query -join "`n"))
    if ($submitted -notmatch 'submitted') { throw $submitted }
    $timeout = [Diagnostics.Stopwatch]::StartNew()
    do {
        $result = [Sentinel.HailstoneWarriorTrial]::OnPoll()
        if ($null -ne $result) { return $result }
        [Threading.Thread]::Yield() | Out-Null
    } while ($timeout.ElapsedMilliseconds -lt 5000)
    throw "Timed out waiting for the managed decision"
}

[Sentinel.HailstoneWarriorTrial]::OnInit()
try {
    $engage = Invoke-Turn engage 42
    if ($engage -notmatch 'queued .* /attack on ') { throw "Expected attack proposal: $engage" }
    $retarget = Invoke-Turn engage 43 1
    if ($retarget -notmatch 'queued .* /target id 43' -or $retarget -match 'queued .* /attack on ') {
        throw "Expected proposed target switch with no attack at low endurance: $retarget"
    }
    $disengage = Invoke-Turn disengage 43
    if ($disengage -notmatch 'queued .* /attack off ') { throw "Expected release proposal: $disengage" }
    Write-Output $engage, $retarget, $disengage
    Write-Output "Managed warrior dry run passed"
} finally {
    [Sentinel.HailstoneWarriorTrial]::OnStop()
}
