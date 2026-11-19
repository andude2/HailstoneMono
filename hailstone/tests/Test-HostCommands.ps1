param([Parameter(Mandatory = $true)][string]$Assembly)
$ErrorActionPreference = "Stop"
$dll = [Reflection.Assembly]::Load([IO.File]::ReadAllBytes((Resolve-Path $Assembly).Path))
$resolver = $dll.GetType("Sentinel.HailstoneHost").GetMethod("ResolveCommand", [Reflection.BindingFlags]"Static,NonPublic")
$lookup = [Func[string,string]] {
    param($query)
    switch -Exact ($query) {
        '${Me.Gem[Fixture Heal]}' { return "3" }
        '${FindItem[=Fixture Clicky].ItemSlot}' { return "24" }
        '${FindItem[=Fixture Clicky].ItemSlot2}' { return "2" }
        default { return "NULL" }
    }
}
$cases = @{
    '/cast "=Fixture Heal"' = '/cast 3'
    '/cast "=Not memorized"' = $null
    '/useitem "Fixture Clicky"' = '/useitem 24 2'
    '/useitem "Missing"' = $null
    '/itemnotify 11 rightmouseup' = '/useitem 11'
    '/cast 2' = '/cast 2'
    '/stick 15 front' = '/stick 15'
    '/stick 25 !front id 42' = '/stick 25 id 42'
    '/stick 15 behind' = '/stick 15 behind'
    '/stick 15 behindonce' = '/stick 15 behind'
    '/stick off' = '/stick off'
}
foreach ($case in $cases.GetEnumerator()) {
    $actual = $resolver.Invoke($null, @($case.Key, $lookup))
    if ($actual -cne $case.Value) { throw "Wrong command for $($case.Key): $actual" }
}
Write-Output "Hailstone spell, clicky, and stick command translation passed"
