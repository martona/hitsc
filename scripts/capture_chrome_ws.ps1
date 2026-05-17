#
# this script will log ws frames for protocol reversing
#
#
# start chrome with:
#
# "C:\Program Files\Google\Chrome\Application\chrome.exe" \
#   --remote-debugging-port=9222 
#   --user-data-dir="c:\users\..\chromealtdatadir" \
#   "https://ipmi-s...s.lan/"  
#

[CmdletBinding()]
param(
    [string]$DebuggingUrl = "http://127.0.0.1:9222",

    [string]$WebSocketUrlRegex = "/kvm",

    [string]$OutputDir = (Join-Path $env:TEMP ("hitsc-ws-capture-" + (Get-Date -Format "yyyyMMdd-HHmmss"))),

    [switch]$AllWebSockets,

    [switch]$ListTargets
)

$ErrorActionPreference = "Stop"

function Join-Url {
    param(
        [string]$Base,
        [string]$Path
    )

    return $Base.TrimEnd("/") + "/" + $Path.TrimStart("/")
}

function Get-CdpJson {
    param([string]$Path)

    $uri = Join-Url $DebuggingUrl $Path
    try {
        Invoke-RestMethod -Method Get -Uri $uri
    } catch {
        throw "Could not connect to Chrome DevTools at $uri. Launch Chrome with --remote-debugging-port, preferably with a fresh --user-data-dir so an existing Chrome instance cannot ignore the flag. Original error: $($_.Exception.Message)"
    }
}

function Format-HexBytes {
    param(
        [byte[]]$Bytes,
        [int]$Max = 16
    )

    if (-not $Bytes -or $Bytes.Length -eq 0) {
        return ""
    }

    $take = [Math]::Min($Bytes.Length, $Max)
    $parts = for ($i = 0; $i -lt $take; $i++) {
        "{0:x2}" -f $Bytes[$i]
    }

    return ($parts -join " ")
}

function Read-HitscPacketHeader {
    param([byte[]]$Bytes)

    if (-not $Bytes -or $Bytes.Length -lt 8) {
        return $null
    }

    [pscustomobject]@{
        Type        = [BitConverter]::ToUInt16($Bytes, 0)
        PayloadSize = [BitConverter]::ToUInt32($Bytes, 2)
        Status      = [BitConverter]::ToUInt16($Bytes, 6)
        FirstBytes  = Format-HexBytes -Bytes $Bytes -Max 16
    }
}

function Get-FrameBytes {
    param($Frame)

    $payload = [string]$Frame.payloadData

    if ($Frame.opcode -eq 2) {
        try {
            return [Convert]::FromBase64String($payload)
        } catch {
            return [Text.Encoding]::UTF8.GetBytes($payload)
        }
    }

    return [Text.Encoding]::UTF8.GetBytes($payload)
}

function Receive-CdpMessage {
    param([Net.WebSockets.ClientWebSocket]$Socket)

    $buffer = New-Object byte[] 65536
    $stream = [IO.MemoryStream]::new()

    try {
        do {
            $segment = [ArraySegment[byte]]::new($buffer)
            $result = $Socket.ReceiveAsync($segment, [Threading.CancellationToken]::None).GetAwaiter().GetResult()

            if ($result.MessageType -eq [Net.WebSockets.WebSocketMessageType]::Close) {
                return $null
            }

            if ($result.Count -gt 0) {
                $stream.Write($buffer, 0, $result.Count)
            }
        } while (-not $result.EndOfMessage)

        return [Text.Encoding]::UTF8.GetString($stream.ToArray())
    } finally {
        $stream.Dispose()
    }
}

function Add-JsonLine {
    param(
        [string]$Path,
        [object]$Object
    )

    $line = $Object | ConvertTo-Json -Compress -Depth 64
    Add-Content -Path $Path -Value $line -Encoding UTF8
}

if ($ListTargets) {
    Get-CdpJson "json" |
        Select-Object id, type, title, url, webSocketDebuggerUrl |
        Format-Table -AutoSize
    exit 0
}

$version = Get-CdpJson "json/version"
if (-not $version.webSocketDebuggerUrl) {
    throw "Chrome did not expose a browser WebSocket. Did you launch it with --remote-debugging-port=9222?"
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$rawPath = Join-Path $OutputDir "websocket-events.ndjson"
$summaryPath = Join-Path $OutputDir "websocket-summary.txt"
$metaPath = Join-Path $OutputDir "capture-meta.json"

$initialTargets = @(Get-CdpJson "json")
$meta = [ordered]@{
    started             = (Get-Date).ToString("o")
    debuggingUrl        = $DebuggingUrl
    browserWebSocketUrl = $version.webSocketDebuggerUrl
    webSocketUrlRegex   = $WebSocketUrlRegex
    allWebSockets       = [bool]$AllWebSockets
    initialTargets      = $initialTargets
}
$meta | ConvertTo-Json -Depth 64 | Set-Content -Path $metaPath -Encoding UTF8

Set-Content -Path $rawPath -Value "" -Encoding UTF8
Set-Content -Path $summaryPath -Value "" -Encoding UTF8

$socket = [Net.WebSockets.ClientWebSocket]::new()
$socket.Options.KeepAliveInterval = [TimeSpan]::FromSeconds(20)
$socket.ConnectAsync([Uri]$version.webSocketDebuggerUrl, [Threading.CancellationToken]::None).GetAwaiter().GetResult()

$nextId = 1
$sessionTargets = @{}
$socketUrls = @{}
$frameNumber = 0

function Send-Cdp {
    param(
        [Net.WebSockets.ClientWebSocket]$Socket,
        [string]$Method,
        [hashtable]$Params = @{},
        [string]$SessionId = $null
    )

    $script:nextId++
    $message = [ordered]@{
        id     = $script:nextId
        method = $Method
    }

    if ($Params -and $Params.Count -gt 0) {
        $message.params = $Params
    }

    if ($SessionId) {
        $message.sessionId = $SessionId
    }

    $json = $message | ConvertTo-Json -Compress -Depth 32
    $bytes = [Text.Encoding]::UTF8.GetBytes($json)
    $segment = [ArraySegment[byte]]::new($bytes)
    $Socket.SendAsync(
        $segment,
        [Net.WebSockets.WebSocketMessageType]::Text,
        $true,
        [Threading.CancellationToken]::None
    ).GetAwaiter().GetResult() | Out-Null
}

function Is-CapturableTarget {
    param($TargetInfo)

    if (-not $TargetInfo) {
        return $false
    }

    if ($TargetInfo.type -ne "page" -and $TargetInfo.type -ne "iframe") {
        return $false
    }

    $url = [string]$TargetInfo.url
    if ($url.StartsWith("devtools://")) {
        return $false
    }

    return $true
}

function Enable-NetworkForSession {
    param(
        [Net.WebSockets.ClientWebSocket]$Socket,
        [string]$SessionId,
        $TargetInfo
    )

    Send-Cdp -Socket $Socket -Method "Network.enable" -Params @{} -SessionId $SessionId
    Send-Cdp -Socket $Socket -Method "Runtime.runIfWaitingForDebugger" -Params @{} -SessionId $SessionId

    $label = "{0} {1}" -f $TargetInfo.type, $TargetInfo.url
    Write-Host "attached: $label"
}

try {
    Write-Host "Writing:"
    Write-Host "  raw:     $rawPath"
    Write-Host "  summary: $summaryPath"
    Write-Host ""
    Write-Host "Open the KVM viewer now. Press Ctrl+C here when done."
    Write-Host ""

    Send-Cdp -Socket $socket -Method "Target.setDiscoverTargets" -Params @{ discover = $true }
    Send-Cdp -Socket $socket -Method "Target.setAutoAttach" -Params @{
        autoAttach             = $true
        waitForDebuggerOnStart = $false
        flatten                = $true
    }

    foreach ($target in $initialTargets) {
        if (-not $target.id) {
            continue
        }
        if (-not (Is-CapturableTarget -TargetInfo $target)) {
            continue
        }

        Send-Cdp -Socket $socket -Method "Target.attachToTarget" -Params @{
            targetId = $target.id
            flatten  = $true
        }
    }

    while ($socket.State -eq [Net.WebSockets.WebSocketState]::Open) {
        $text = Receive-CdpMessage -Socket $socket
        if (-not $text) {
            break
        }

        $message = $text | ConvertFrom-Json
        $method = [string]$message.method
        $sessionId = [string]$message.sessionId

        if (-not $method) {
            continue
        }

        if ($method -eq "Target.attachedToTarget") {
            $attachedSessionId = [string]$message.params.sessionId
            $targetInfo = $message.params.targetInfo
            $sessionTargets[$attachedSessionId] = $targetInfo

            if (Is-CapturableTarget -TargetInfo $targetInfo) {
                Enable-NetworkForSession -Socket $socket -SessionId $attachedSessionId -TargetInfo $targetInfo
            } else {
                Send-Cdp -Socket $socket -Method "Runtime.runIfWaitingForDebugger" -Params @{} -SessionId $attachedSessionId
            }
            continue
        }

        if ($method -eq "Target.targetInfoChanged") {
            foreach ($key in @($sessionTargets.Keys)) {
                if ($sessionTargets[$key].targetId -eq $message.params.targetInfo.targetId) {
                    $sessionTargets[$key] = $message.params.targetInfo
                }
            }
            continue
        }

        if (-not $method.StartsWith("Network.webSocket")) {
            continue
        }

        $targetInfo = $sessionTargets[$sessionId]
        if (-not (Is-CapturableTarget -TargetInfo $targetInfo)) {
            continue
        }

        $requestId = [string]$message.params.requestId
        $requestKey = "$sessionId/$requestId"
        $targetInfo = $sessionTargets[$sessionId]

        if ($method -eq "Network.webSocketCreated") {
            $socketUrls[$requestKey] = [string]$message.params.url
        }

        $socketUrl = [string]$socketUrls[$requestKey]
        $matchesSocket = $AllWebSockets -or ($socketUrl -match $WebSocketUrlRegex)

        if (-not $matchesSocket) {
            continue
        }

        Add-JsonLine -Path $rawPath -Object ([ordered]@{
            time      = (Get-Date).ToString("o")
            sessionId = $sessionId
            target    = $targetInfo
            method    = $method
            params    = $message.params
        })

        if ($method -eq "Network.webSocketCreated") {
            $line = "{0} socket-created sid={1} req={2} url={3}" -f `
                (Get-Date).ToString("o"), $sessionId, $requestId, $socketUrl
            Add-Content -Path $summaryPath -Value $line -Encoding UTF8
            Write-Host $line
            continue
        }

        if ($method -eq "Network.webSocketClosed") {
            $line = "{0} socket-closed sid={1} req={2} url={3}" -f `
                (Get-Date).ToString("o"), $sessionId, $requestId, $socketUrl
            Add-Content -Path $summaryPath -Value $line -Encoding UTF8
            Write-Host $line
            continue
        }

        if ($method -eq "Network.webSocketFrameError") {
            $line = "{0} socket-error sid={1} req={2} error={3}" -f `
                (Get-Date).ToString("o"), $sessionId, $requestId, $message.params.errorMessage
            Add-Content -Path $summaryPath -Value $line -Encoding UTF8
            Write-Host $line
            continue
        }

        if ($method -ne "Network.webSocketFrameSent" -and $method -ne "Network.webSocketFrameReceived") {
            continue
        }

        $frameNumber++
        $direction = if ($method -eq "Network.webSocketFrameSent") { "send" } else { "recv" }
        $frame = $message.params.response
        $bytes = Get-FrameBytes -Frame $frame
        $header = Read-HitscPacketHeader -Bytes $bytes

        if ($header) {
            $line = "{0} {1} #{2} sid={3} req={4} opcode={5} type={6} status={7} payload={8} bytes={9} url={10}" -f `
                (Get-Date).ToString("o"),
                $direction,
                $frameNumber,
                $sessionId,
                $requestId,
                $frame.opcode,
                $header.Type,
                $header.Status,
                $header.PayloadSize,
                $header.FirstBytes,
                $socketUrl
        } else {
            $line = "{0} {1} #{2} sid={3} req={4} opcode={5} bytes={6} first={7} url={8}" -f `
                (Get-Date).ToString("o"),
                $direction,
                $frameNumber,
                $sessionId,
                $requestId,
                $frame.opcode,
                $bytes.Length,
                (Format-HexBytes -Bytes $bytes -Max 16),
                $socketUrl
        }

        Add-Content -Path $summaryPath -Value $line -Encoding UTF8
        Write-Host $line
    }
} finally {
    if ($socket.State -eq [Net.WebSockets.WebSocketState]::Open) {
        $socket.CloseAsync(
            [Net.WebSockets.WebSocketCloseStatus]::NormalClosure,
            "capture stopped",
            [Threading.CancellationToken]::None
        ).GetAwaiter().GetResult()
    }

    $socket.Dispose()

    Write-Host ""
    Write-Host "Capture written to:"
    Write-Host "  $OutputDir"
}
