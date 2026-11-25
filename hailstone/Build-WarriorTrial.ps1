param(
    [Parameter(Mandatory = $true)][string]$SentinelRoot,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$MSBuild = "MSBuild.exe"
)
$ErrorActionPreference = "Stop"
$SentinelRoot = (Resolve-Path -LiteralPath $SentinelRoot).Path
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$stage = Join-Path ([IO.Path]::GetTempPath()) ("hailstone-warrior-" + [Guid]::NewGuid())
try {
    New-Item -ItemType Directory -Path (Join-Path $stage "Sentinel"), (Join-Path $stage "Shared") | Out-Null
    $source = Join-Path $SentinelRoot "Sentinel"
    $project = Join-Path $source "Sentinel.csproj"
    [xml]$xml = Get-Content -Raw -LiteralPath $project
    $ns = $xml.DocumentElement.NamespaceURI
    foreach ($item in $xml.GetElementsByTagName("Compile", $ns)) {
        $relative = $item.GetAttribute("Include")
        $destination = [IO.Path]::GetFullPath((Join-Path (Join-Path $stage "Sentinel") $relative))
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
        Copy-Item -LiteralPath (Join-Path $source $relative) -Destination $destination
    }
    foreach ($file in @("WarriorTrial.cs", "SentinelHost.cs")) {
        $item = $xml.CreateElement("Compile", $ns)
        $item.SetAttribute("Include", $file)
        $xml.GetElementsByTagName("ItemGroup", $ns).Item(1).AppendChild($item) | Out-Null
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination (Join-Path $stage "Sentinel")
    }
    function Replace-Source([string]$file, [string]$before, [string]$after) {
        $path = Join-Path (Join-Path $stage "Sentinel") $file
        $text = [IO.File]::ReadAllText($path)
        if (!$text.Contains($before)) { throw "Sentinel host integration point missing in $file ($before)" }
        [IO.File]::WriteAllText($path, $text.Replace($before, $after))
    }
    Replace-Source "RuntimeExecution.cs" "internal bool WaitYield(int ms)" "internal WaitHandle YieldSignal { get { return _yielded.WaitHandle; } } internal bool WaitYield(int ms)"
    Replace-Source "Core.cs" "_boundary.WaitYield(" "Sentinel.HailstoneHost.WaitForWorker(_boundary, "
    Replace-Source "Core.cs" 'return Sentinel.QueryReuse.Read(query, ParseTloLive);' 'var adapter = Host as Sentinel.HailstoneHost; if (adapter != null && adapter.TryReadWorkerState(query, out value)) return value; return Sentinel.QueryReuse.Read(query, ParseTloLive);'
    Replace-Source "Core.cs" 'GuardUi("native UI bridge", delegate { Sentinel.NativeUiBridge.Tick(); });' 'if (!(Host is Sentinel.HailstoneHost)) GuardUi("native UI bridge", delegate { Sentinel.NativeUiBridge.Tick(); });'
    Replace-Source "Config\CharConfig.cs" 'string mqRoot = FindMqRoot();' 'string mqRoot = Core.Host is Sentinel.HailstoneHost ? "" : FindMqRoot();'
    Replace-Source "Config\CharConfig.cs" 'ConfigDir = ResolveCharacterConfigDir(mqRoot, server, characterName);' 'ConfigDir = Core.Host is Sentinel.HailstoneHost ? Sentinel.HailstoneHost.ResolveConfigurationDirectory(server, characterName) : ResolveCharacterConfigDir(mqRoot, server, characterName); _suppressMigration = Core.Host is Sentinel.HailstoneHost && Sentinel.HailstoneHost.SharesCharacterConfig;'
    Replace-Source "Comms\CommsChannel.cs" '_transport = new TcpTransport();' '_transport = Core.Host is Sentinel.HailstoneHost ? new TcpTransport(Sentinel.HailstoneHost.BrokerPort, true) : new TcpTransport();'
    Replace-Source "Comms\CommsChannel.cs" 'PollShm(nowMs);' 'if (!(Core.Host is Sentinel.HailstoneHost)) PollShm(nowMs);'
    Replace-Source "Comms\CommsChannel.cs" 'ShmStateLane.Publish(new ShmVitals' 'if (!(Core.Host is Sentinel.HailstoneHost)) ShmStateLane.Publish(new ShmVitals'
    Replace-Source "Comms\TcpTransport.cs" '_token = CommsToken.Resolve(DeployDirectory());' '_token = Core.Host is Sentinel.HailstoneHost ? CommsToken.Read(Sentinel.HailstoneHost.TokenFile) : CommsToken.Resolve(DeployDirectory());'
    Replace-Source "..\..\CrewBus\Shared\CommsToken.cs" 'private static string Read(string path)' 'internal static string Read(string path)'
    $drawing = $xml.CreateElement("Reference", $ns)
    $drawing.SetAttribute("Include", "System.Drawing")
    $xml.GetElementsByTagName("Reference", $ns).Item(0).ParentNode.AppendChild($drawing) | Out-Null
    $stagedProject = Join-Path $stage "Sentinel\Sentinel.csproj"
    $xml.Save($stagedProject)
    & $MSBuild $stagedProject /p:Configuration=Release /v:minimal
    if ($LASTEXITCODE -ne 0) { throw "Sentinel build failed ($LASTEXITCODE)" }
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    Copy-Item -LiteralPath (Join-Path $stage "Sentinel\bin\Release\sentinel.dll") -Destination $OutputDirectory
} finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}
