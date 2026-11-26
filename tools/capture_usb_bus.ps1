[CmdletBinding()]
param(
    [string]$Port = 'COM3',
    [string]$LogPath = ("waveshare-usb-{0}.txt" -f (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
)

$ErrorActionPreference = 'Stop'
$serial = [IO.Ports.SerialPort]::new($Port, 115200, 'None', 8, 'One')
$serial.NewLine = "`r`n"
$serial.WriteTimeout = 1000

function Read-Diagnostic([string]$Command, [string]$Header, [string]$Terminator) {
    $serial.DiscardInBuffer()
    $serial.WriteLine($Command)
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $text = ''
    $lastByte = 0
    while ($timer.ElapsedMilliseconds -lt 2500) {
        $chunk = $serial.ReadExisting()
        if ($chunk.Length) {
            $text += $chunk
            $lastByte = $timer.ElapsedMilliseconds
        }
        $start = $text.IndexOf($Header, [StringComparison]::Ordinal)
        if ($start -ge 0) {
            $response = $text.Substring($start)
            if ($Terminator) {
                $end = $response.IndexOf($Terminator, [StringComparison]::Ordinal)
                if ($end -ge 0) { return $response.Substring(0, $end + $Terminator.Length).TrimEnd() }
            } elseif ($response.Contains("`n") -and ($timer.ElapsedMilliseconds - $lastByte -ge 150)) {
                return $response.TrimEnd()
            }
        }
        Start-Sleep -Milliseconds 15
    }
    throw "Incomplete response to $Command on $Port. Check the port and firmware version."
}

# CreateNew preserves previous bench captures, including partial captures.
$stream = [IO.File]::Open($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($LogPath),
                         [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
$log = [IO.StreamWriter]::new($stream)
$log.AutoFlush = $true
function Record([string]$Text) {
    $log.WriteLine($Text)
    Write-Output $Text
}
try {
    $serial.Open()
    Record ("{0:o} port={1}" -f [DateTimeOffset]::Now, $Port)
    $trace = Read-Diagnostic 'KMBOX_TRACE' 'KMBOX_TRACE:' ''
    Record $trace
    foreach ($pass in 1, 2) {
        Record ("sample={0} utc={1:o}" -f $pass, [DateTimeOffset]::UtcNow)
        $info = Read-Diagnostic 'KMBOX_INFO' 'KMBOX_VID:' ''
        if ($info -notmatch '(?m)^KMBOX_RX:') { throw 'Incomplete KMBOX_INFO counter block.' }
        # The counters suffice. Omit raw HID payload bytes from the bench log.
        Record ($info -replace '(?m)(KMBOX_RX:[^\r\n]*),d=[0-9A-Fa-f]+', '$1')
        $index = Read-Diagnostic 'KMBOX_BUS' 'KMBOX_BUS:v1 ' ''
        Record $index
        if ($index -notmatch '(?m)^KMBOX_BUS:v1 [^\r\n]* pages=([ 0-9]*)\r?$') {
            throw 'Unexpected USB diagnostic index.'
        }
        $pages = $Matches[1].Trim()
        if ($pages) {
            foreach ($page in ($pages -split '\s+')) {
                Record (Read-Diagnostic "KMBOX_BUS $page" 'KMBOX_BUS:' "KMBOX_BUS_END`r`n")
            }
        }
        if ($pass -eq 1) { Start-Sleep -Milliseconds 1200 }
    }
} catch {
    $log.WriteLine("ERROR: " + $_.Exception.Message)
    throw
} finally {
    $serial.Dispose()
    $log.Dispose()
    Write-Host "Capture: $LogPath"
}
