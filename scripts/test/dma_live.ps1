# Sequential DMA on/off comparisons. gt5boot waits for any existing gt5.exe
# and stops only its own child. Never changes or clears shader caches.
[CmdletBinding()]
param(
    [ValidateSet("HSR", "Eiger", "CarList", "Movie")][string]$Case = "HSR",
    [string]$Tag = "dma-live",
    [string]$Order = "1,0,0,1",
    [int]$Settle = 60,
    [int]$SampleDelay = 20,
    [string]$Exe = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $Exe) { $Exe = "$root\build\gt5.exe" }
$record = "$root\scratch\dma-live"
New-Item -ItemType Directory -Force $record | Out-Null
. "$PSScriptRoot\milestone.ps1"
$values = @($Order -split ',')
if (@($values | Where-Object { $_ -notin @('0', '1') }).Count) { throw "Order must contain only 0 and 1" }
$sha = (Get-FileHash -Algorithm SHA256 $Exe).Hash
$index = 0
foreach ($value in $values) {
    $index++
    $name = "$Tag-$($Case.ToLower())-$index-fast$value"
    $vars = @("PS3_VERBOSE=0", "SPU_DMA_FAST=$value", "GT5P_RESOLUTION=720p", "RSX_MSAA=2",
              "RSX_PRESENT_SYNC=0", "RSX_SHADER_WARMUP=1")
    $h = @{ Tag = $name; Settle = $Settle; Gap = 20; Exe = $Exe; SampleDelay = $SampleDelay }
    if ($Case -eq "Movie") {
        $h.Movie = $true
        $vars += "PAD_SKIP_MOVIE=0"
    } else {
        $h.FrameTest = $true
        $h.Route = "race"
        if ($Case -eq "CarList") { $h.Route = "carlist"; $h.ShotsFrom = "CarList" }
        if ($Case -eq "Eiger") {
            # Measured Phase 45 route: six Rights on the track list.
            $s = @("$GT5BootHold", "@MusicDecode+5000-5100:RIGHT")
            for ($t = 7000; $t -le 60000; $t += 3000) { $s += "@MusicDecode+$t-$($t+200)!TrackList:CROSS" }
            for ($k = 0; $k -lt 6; $k++) { $t = 3000 + 700 * $k; $s += "@TrackList+$t-$($t+150):RIGHT" }
            for ($t = 9000; $t -le 120000; $t += 3000) { $s += "@TrackList+$t-$($t+200)!RaceLoad:CROSS" }
            for ($t = 15000; $t -le 75000; $t += 3000) { $s += "@RaceLoad+$t-$($t+200)!RaceStart:CROSS" }
            $s += "@RaceStart+1000-900000:CROSS"
            $vars += "PAD_HOLD=" + ($s -join ',')
        }
    }
    "Starting $name (exe SHA256 $sha)"
    $log = & "$PSScriptRoot\gt5boot.ps1" @h @vars
    $log | Set-Content "$record\$name.txt"
    $log
    if ($Case -ne "Movie" -and -not (Test-Path "$root\scratch\pv\$name.frames.txt")) {
        throw "No measured frame window for $name; inspect its logs before continuing"
    }
}
