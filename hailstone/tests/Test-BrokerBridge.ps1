param(
    [Parameter(Mandatory = $true)][string]$Harness,
    [Parameter(Mandatory = $true)][string]$Broker,
    [Parameter(Mandatory = $true)][string]$RuntimeFixture
)
$ErrorActionPreference = "Stop"
$fixture = (Resolve-Path $RuntimeFixture).Path
$settings = Join-Path $fixture "sentinel.json"
if (Test-Path -LiteralPath $settings) { throw "Use a disposable runtime fixture without sentinel.json" }
$dir = Join-Path ([IO.Path]::GetTempPath()) ("hailstone-comms-" + [Guid]::NewGuid())
$brokerProcess = $null
$clientProcess = $null
$peer = $null
function Send-Packet($stream, $packet) {
    $bytes = [Text.Encoding]::UTF8.GetBytes(($packet | ConvertTo-Json -Compress -Depth 8))
    $header = [BitConverter]::GetBytes([Net.IPAddress]::HostToNetworkOrder([int]$bytes.Length))
    $stream.Write($header, 0, 4)
    $stream.Write($bytes, 0, $bytes.Length)
}
function Read-Bytes($stream, [int]$size) {
    $buffer = New-Object byte[] $size
    for ($offset = 0; $offset -lt $size;) {
        $n = $stream.Read($buffer, $offset, $size - $offset)
        if ($n -le 0) { throw "Unexpected end of broker stream" }
        $offset += $n
    }
    return ,$buffer
}
function Receive-Packet($stream) {
    $header = Read-Bytes $stream 4
    $size = [Net.IPAddress]::NetworkToHostOrder([BitConverter]::ToInt32($header, 0))
    if ($size -lt 1 -or $size -gt 8388608) { throw "Invalid frame size" }
    $bytes = Read-Bytes $stream $size
    return [Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json
}
try {
    New-Item -ItemType Directory $dir | Out-Null
    Copy-Item -LiteralPath $Broker -Destination (Join-Path $dir "sentinel-broker.exe")
    $token = [Guid]::NewGuid().ToString("N") + [Guid]::NewGuid().ToString("N")
    $tokenPath = Join-Path $dir "sentinel-comms.token"
    [IO.File]::WriteAllText($tokenPath, $token)
    $listener = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    @{ port = $port; token_file = $tokenPath; server = "Fixture" } |
        ConvertTo-Json | Set-Content -Encoding ASCII -LiteralPath $settings
    $brokerProcess = Start-Process (Join-Path $dir "sentinel-broker.exe") -ArgumentList "--port $port" -PassThru
    $deadline = [Diagnostics.Stopwatch]::StartNew()
    do {
        $peer = New-Object Net.Sockets.TcpClient
        try { $peer.Connect([Net.IPAddress]::Loopback, $port); break }
        catch { $peer.Dispose(); $peer = $null; Start-Sleep -Milliseconds 25 }
    } while ($deadline.ElapsedMilliseconds -lt 5000)
    if ($null -eq $peer) { throw "Broker did not listen" }
    $peer.ReceiveTimeout = 1000
    $stream = $peer.GetStream()
    Send-Packet $stream @{protocol="sentinel-tcp-1"; kind="hello"; session=[Guid]::NewGuid().ToString("N"); name="FixturePeer"; token=$token}
    if ((Receive-Packet $stream).kind -ne "welcome") { throw "Peer could not authenticate" }
    $output = Join-Path $dir "harness.log"
    $clientProcess = Start-Process $Harness -ArgumentList ('"' + $fixture + '" --broker') -PassThru -RedirectStandardOutput $output
    $deadline.Restart()
    $engaged = $false
    do {
        if (!$stream.DataAvailable) { Start-Sleep -Milliseconds 10; continue }
        $packet = Receive-Packet $stream
        if ($packet.kind -ne "deliver") { continue }
        $message = $packet.data | ConvertFrom-Json
        if ($message.type -eq "engage" -and $message.server -eq "Fixture" -and
            $message.from -eq "Warrior" -and $message.body.targetId -eq 42) { $engaged = $true; break }
    } while ($deadline.ElapsedMilliseconds -lt 8000)
    if (!$engaged) { Get-Content -LiteralPath $output; throw "No matching broadcast engage arrived" }
    Send-Packet $stream @{protocol="sentinel-tcp-1"; kind="ack"; id=$packet.id}
    Start-Sleep -Milliseconds 400
    $release = @{v=1; type="disengage"; server="Fixture"; zone="training"; from="FixturePeer"; ms=0;
        body=@{reason="command"}} | ConvertTo-Json -Compress -Depth 4
    $expires = [long]([DateTime]::UtcNow.Ticks / [TimeSpan]::TicksPerMillisecond) + 4000
    Send-Packet $stream @{protocol="sentinel-tcp-1"; kind="publish"; id=[Guid]::NewGuid().ToString("N");
        expires=$expires; data=$release}
    if (!$clientProcess.WaitForExit(15000)) {
        Get-Content -LiteralPath $output
        throw "Embedded client did not stop after broadcasting engage"
    }
    $log = Get-Content -Raw -LiteralPath $output
    if ($log -notmatch "Broker echo fixture passed") {
        Write-Output $log
        throw "Embedded client failed to apply the peer disengage"
    }
    Write-Output "Embedded Sentinel broadcast engage and applied a peer disengage through the authenticated broker"
} finally {
    if ($peer) { $peer.Dispose() }
    if ($clientProcess -and !$clientProcess.HasExited) { Stop-Process -Id $clientProcess.Id -Force }
    if ($brokerProcess -and !$brokerProcess.HasExited) { Stop-Process -Id $brokerProcess.Id -Force }
    if (Test-Path -LiteralPath $settings) { Remove-Item -LiteralPath $settings }
    if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
}
