# ============================================
# BadUSB Host Sync Script
#
# Usage:
#   .\sync_host.ps1                 # Wait for new device
#   .\sync_host.ps1 -ComPort COM7   # Use specific port
#
# This script can detect when a new device is plugged in.
# ============================================

param(
    [string]$ComPort = "",
    [int]$BaudRate = 115200,
    [int]$TimeoutSeconds = 60
)

$HANDSHAKE_REQ = "NOLOGO_SHAKE"
$HANDSHAKE_RESP = "NOLOGO_ACK"
$SYNC_HOST = "NOLOGO_SYNC"
$SYNC_DEVICE = "NOLOGO_DONE"

function Write-Status($Message, $Color = "White") {
    $ts = Get-Date -Format "HH:mm:ss"
    Write-Host "[$ts] " -NoNewline -ForegroundColor DarkGray
    Write-Host $Message -ForegroundColor $Color
}

function Wait-ForNewPort {
    $initialPorts = [IO.Ports.SerialPort]::GetPortNames()
    Write-Status "Current ports: $($initialPorts -join ', ')" -Color Gray
    Write-Status "Waiting for new device... (plug in BadUSB now)" -Color Yellow
    
    while ($true) {
        Start-Sleep -Milliseconds 500
        $currentPorts = [IO.Ports.SerialPort]::GetPortNames()
        
        # Find new ports
        $newPorts = $currentPorts | Where-Object { $_ -notin $initialPorts }
        
        if ($newPorts) {
            $newPort = $newPorts | Select-Object -First 1
            Write-Status "New device detected: $newPort" -Color Green
            return $newPort
        }
    }
}

function Connect-CDCPort($port) {
    Write-Status "Connecting to $port..." -Color Yellow
    
    $maxRetries = 10
    for ($i = 1; $i -le $maxRetries; $i++) {
        $s = $null
        try {
            $s = New-Object IO.Ports.SerialPort $port, $BaudRate
            $s.DtrEnable = $true
            $s.RtsEnable = $true
            $s.ReadTimeout = 5000
            $s.WriteTimeout = 5000
            # Disable hardware flow control to avoid semaphore timeouts
            $s.Handshake = [IO.Ports.Handshake]::None
            $s.Open()
            
            Write-Status "Port opened, setting DTR to signal device..." -Color Gray
            # Ensure DTR is set - this signals the device that host is ready
            $s.DtrEnable = $true
            
            # Give device time to detect DTR and prepare for communication
            Write-Status "Waiting for device to detect DTR..." -Color Gray
            Start-Sleep -Seconds 3
            
            # Clear any garbage in buffers
            $s.DiscardInBuffer()
            $s.DiscardOutBuffer()
            
            Write-Status "Sending handshake: $HANDSHAKE_REQ" -Color Yellow
            $s.Write($HANDSHAKE_REQ)
            
            # Wait for response with timeout
            $responseWait = 0
            $maxWait = 3000  # 3 seconds
            $resp = ""
            
            while ($responseWait -lt $maxWait) {
                Start-Sleep -Milliseconds 100
                $responseWait += 100
                
                if ($s.BytesToRead -gt 0) {
                    $resp += $s.ReadExisting()
                    if ($resp -match $HANDSHAKE_RESP) {
                        break
                    }
                }
            }
            
            if ($resp -match $HANDSHAKE_RESP) {
                Write-Status "Handshake successful!" -Color Green
                return $s
            } else {
                Write-Status "Attempt $i/$maxRetries : No ACK received (got: '$resp')" -Color Yellow
                $s.Close()
            }
        } catch {
            Write-Status "Attempt $i/$maxRetries failed: $_" -Color Yellow
            if ($s -and $s.IsOpen) { 
                try { $s.Close() } catch {}
            }
        }
        
        Start-Sleep -Seconds 2
    }
    
    return $null
}

# ============================================
# Main
# ============================================

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  BadUSB Host Sync Script" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""

$targetPort = $ComPort

if ($targetPort -eq "") {
    # Wait for new device
    $targetPort = Wait-ForNewPort
    
    # Give device time to initialize (USB enumeration + CDC ready)
    # Device needs time for: USB enumeration + CDC ACM class init + RX thread start
    Write-Status "Waiting for device to initialize (5 seconds)..." -Color Gray
    Start-Sleep -Seconds 5
}

$serial = Connect-CDCPort $targetPort

if ($null -eq $serial) {
    Write-Status "Failed to connect to CDC port. Exiting." -Color Red
    exit 1
}

try {
    # Wait for device to signal DONE
    Write-Status "Waiting for device DONE signal..." -Color Yellow
    $buf = ""
    $start = Get-Date
    
    while (-not $buf.Contains($SYNC_DEVICE)) {
        if (((Get-Date) - $start).TotalSeconds -gt $TimeoutSeconds) {
            Write-Status "Timeout waiting for device!" -Color Red
            break
        }
        
        try {
            if ($serial.BytesToRead -gt 0) {
                $data = $serial.ReadExisting()
                $buf += $data
                Write-Host $data -NoNewline -ForegroundColor Cyan
            }
        } catch {}
        
        Start-Sleep -Milliseconds 50
    }
    
    if ($buf.Contains($SYNC_DEVICE)) {
        Write-Host ""
        Write-Status "Device signaled DONE!" -Color Green
        
        # Send sync signal
        Write-Status "Sending NOLOGO_SYNC to device..." -Color Yellow
        $serial.Write($SYNC_HOST)
        
        Write-Status "Sync complete!" -Color Green
    }
} finally {
    if ($serial.IsOpen) {
        $serial.Close()
    }
}

Write-Host ""
Write-Host "============================================" -ForegroundColor Green
Write-Host "  Done!" -ForegroundColor Green
Write-Host "============================================" -ForegroundColor Green
