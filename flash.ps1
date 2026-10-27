<#
.SYNOPSIS
    Flash ESP32 NodeMCU directly from Antigravity IDE.
.EXAMPLE
    .\flash.ps1                     # Compile & flash wingUI to auto-detected COM port
    .\flash.ps1 -Monitor            # Flash wingUI and open Serial Monitor
    .\flash.ps1 -Sketch wingUI_sprmini # Flash wingUI_sprmini
    .\flash.ps1 -CompileOnly        # Only verify / compile
    .\flash.ps1 -Port COM3          # Explicitly specify COM port
#>

[CmdletBinding()]
param(
    [string]$Sketch = "wingUI",
    [string]$Port = "",
    [int]$Baud = 115200,
    [string]$FQBN = "esp32:esp32:nodemcu-32s",
    [switch]$CompileOnly,
    [switch]$UploadOnly,
    [switch]$Monitor
)

# 1. Locate arduino-cli
$cliPath = "G:\Arduino\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
if (-not (Test-Path $cliPath)) {
    $found = (Get-Command "arduino-cli" -ErrorAction SilentlyContinue)
    if ($found) {
        $cliPath = $found.Source
    } else {
        Write-Error "arduino-cli.exe not found at '$cliPath' or in system PATH."
        exit 1
    }
}

# Resolve sketch path
$sketchPath = Join-Path $PSScriptRoot $Sketch
if (-not (Test-Path $sketchPath)) {
    Write-Error "Sketch directory not found: '$sketchPath'"
    exit 1
}

# 2. Auto-detect COM port if flashing and not provided
if (-not $CompileOnly -and [string]::IsNullOrWhiteSpace($Port)) {
    Write-Host "[ESP32] Detecting serial ports..." -ForegroundColor Cyan
    $portsJson = & $cliPath board list --json | ConvertFrom-Json -ErrorAction SilentlyContinue
    if ($portsJson -and $portsJson.detected_ports) {
        foreach ($p in $portsJson.detected_ports) {
            # Filter for USB serial port
            if ($p.port.protocol_label -match "USB" -or $p.port.label -match "USB") {
                $Port = $p.port.address
                break
            }
        }
    }
    
    if ([string]::IsNullOrWhiteSpace($Port)) {
        # Fallback to .NET serial ports
        $allPorts = [System.IO.Ports.SerialPort]::GetPortNames() | Where-Object { $_ -ne "COM1" }
        if ($allPorts.Count -gt 0) {
            $Port = $allPorts[0]
        }
    }

    if ([string]::IsNullOrWhiteSpace($Port)) {
        Write-Error "No USB serial port detected. Please specify -Port COMx (e.g. -Port COM3)."
        exit 1
    }
    Write-Host "[ESP32] Using port: $Port" -ForegroundColor Green
}

# 3. Actions
if ($CompileOnly) {
    Write-Host "[ESP32] Compiling $Sketch ($FQBN)..." -ForegroundColor Cyan
    & $cliPath compile --fqbn $FQBN $sketchPath
    exit $LASTEXITCODE
}

if ($UploadOnly) {
    Write-Host "[ESP32] Uploading to $Port ($FQBN)..." -ForegroundColor Cyan
    & $cliPath upload -p $Port -b $FQBN $sketchPath
    exit $LASTEXITCODE
}

Write-Host "[ESP32] Compiling and Flashing $Sketch to $Port..." -ForegroundColor Cyan
& $cliPath compile --fqbn $FQBN -u -p $Port $sketchPath

if ($LASTEXITCODE -ne 0) {
    Write-Error "[ESP32] Flash failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

Write-Host "[ESP32] Successfully flashed $Sketch!" -ForegroundColor Green

if ($Monitor) {
    Write-Host "[ESP32] Starting Serial Monitor on $Port @ $Baud bps (Press Ctrl+C to exit)..." -ForegroundColor Yellow
    & $cliPath monitor -p $Port -c baudrate=$Baud
}
