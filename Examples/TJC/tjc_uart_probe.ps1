[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Port,

    [ValidateRange(1200, 921600)]
    [int]$Baud = 115200,

    [string]$Command = "sendme",

    [ValidateRange(50, 5000)]
    [int]$WaitMs = 500
)

function ConvertTo-HexText {
    param([byte[]]$Bytes)

    if (($null -eq $Bytes) -or ($Bytes.Length -eq 0)) {
        return "<empty>"
    }

    return (($Bytes | ForEach-Object { $_.ToString("X2") }) -join " ")
}

function Split-TjcFrame {
    param([byte[]]$Bytes)

    $frames = [System.Collections.Generic.List[object]]::new()
    $current = [System.Collections.Generic.List[byte]]::new()
    $ffCount = 0

    foreach ($value in $Bytes) {
        if ($value -eq 0xFF) {
            $ffCount++
            if ($ffCount -eq 3) {
                $frames.Add([pscustomobject]@{
                    Bytes = $current.ToArray()
                })
                $current.Clear()
                $ffCount = 0
            }
            continue
        }

        while ($ffCount -gt 0) {
            $current.Add(0xFF)
            $ffCount--
        }
        $current.Add($value)
    }

    while ($ffCount -gt 0) {
        $current.Add(0xFF)
        $ffCount--
    }
    if ($current.Count -gt 0) {
        $frames.Add([pscustomobject]@{
            Bytes = $current.ToArray()
        })
    }

    return $frames
}

function Get-TjcFrameDescription {
    param([byte[]]$Frame)

    if (($null -eq $Frame) -or ($Frame.Length -eq 0)) {
        return "Empty frame"
    }

    switch ($Frame[0]) {
        0x00 { return "Invalid command" }
        0x01 { return "Command succeeded" }
        0x02 { return "Invalid component ID" }
        0x03 { return "Invalid page ID" }
        0x1A { return "Invalid variable name" }
        0x1B { return "Invalid variable operation" }
        0x1C { return "Assignment failed" }
        0x24 { return "Serial buffer overflow" }
        0x65 {
            if ($Frame.Length -ge 4) {
                $state = if ($Frame[3] -eq 0) { "release" } else { "press" }
                return "Touch: page=$($Frame[1]) component=$($Frame[2]) state=$state"
            }
            return "Invalid touch frame length"
        }
        0x66 {
            if ($Frame.Length -ge 2) {
                return "Current page: $($Frame[1])"
            }
            return "Invalid page frame length"
        }
        0x70 {
            if ($Frame.Length -gt 1) {
                return "String: $([System.Text.Encoding]::ASCII.GetString($Frame, 1, $Frame.Length - 1))"
            }
            return "Empty string"
        }
        0x71 {
            if ($Frame.Length -ge 5) {
                return "Number: $([System.BitConverter]::ToUInt32($Frame, 1))"
            }
            return "Invalid number frame length"
        }
        0x88 { return "Display startup completed" }
        0xFD { return "Transparent transfer completed" }
        0xFE { return "Transparent transfer ready" }
        default { return "Unknown frame type 0x$($Frame[0].ToString('X2'))" }
    }
}

$serial = [System.IO.Ports.SerialPort]::new(
    $Port,
    $Baud,
    [System.IO.Ports.Parity]::None,
    8,
    [System.IO.Ports.StopBits]::One)
$serial.Handshake = [System.IO.Ports.Handshake]::None
$serial.ReadTimeout = 50
$serial.WriteTimeout = 500
$received = [System.Collections.Generic.List[byte]]::new()

try {
    $serial.Open()
    $serial.DiscardInBuffer()
    $serial.DiscardOutBuffer()

    $commandBytes = [System.Text.Encoding]::ASCII.GetBytes($Command)
    $txBytes = [byte[]]::new($commandBytes.Length + 3)
    [System.Array]::Copy($commandBytes, $txBytes, $commandBytes.Length)
    $txBytes[$txBytes.Length - 3] = 0xFF
    $txBytes[$txBytes.Length - 2] = 0xFF
    $txBytes[$txBytes.Length - 1] = 0xFF

    Write-Host "Port: $Port, Baud: $Baud"
    Write-Host "TX ASCII: $Command"
    Write-Host "TX HEX:   $(ConvertTo-HexText $txBytes)"
    $serial.Write($txBytes, 0, $txBytes.Length)

    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt $WaitMs) {
        $available = $serial.BytesToRead
        if ($available -gt 0) {
            $buffer = [byte[]]::new($available)
            $read = $serial.Read($buffer, 0, $buffer.Length)
            for ($index = 0; $index -lt $read; $index++) {
                $received.Add($buffer[$index])
            }
        }
        Start-Sleep -Milliseconds 10
    }

    if ($received.Count -eq 0) {
        Write-Warning "No response. Check baud rate, crossed TX/RX wiring, and common ground."
        exit 2
    }

    $rxBytes = $received.ToArray()
    Write-Host "RX HEX:   $(ConvertTo-HexText $rxBytes)"
    $frameIndex = 0
    foreach ($frameItem in (Split-TjcFrame $rxBytes)) {
        $frameIndex++
        $frame = $frameItem.Bytes
        Write-Host "Frame ${frameIndex}: $(Get-TjcFrameDescription $frame)"
    }
}
finally {
    if ($serial.IsOpen) {
        $serial.Close()
    }
    $serial.Dispose()
}
