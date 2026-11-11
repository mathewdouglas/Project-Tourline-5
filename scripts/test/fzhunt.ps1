# GT5 race freezes (Phase 32): N High Speed Ring races, each a gt5boot.ps1
# -FrameTest window (throttle held), with stall dumps after 1 s. Per run:
# the longest frame interval, every gap over 1 s, and how many [lockline]
# long-wait reports and stall dumps it logged. Then the totals.
#   scripts\test\fzhunt.ps1 [-N 6] [-Prefix fz] [-Settle 50] [-StopOnFreeze] NAME=VALUE ...
#   -> scratch\pv\<prefix><i>.*  (gt5boot.ps1 output), and the summary on stdout
[CmdletBinding(PositionalBinding = $false)]
param([int]$N = 6, [string]$Prefix = "fz", [int]$Settle = 50, [switch]$StopOnFreeze,
      [Parameter(ValueFromRemainingArguments = $true)] $Vars)
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$dir = "$root\scratch\pv"
$freezes = 0; $runs = 0
for ($i = 1; $i -le $N; $i++) {
    $t = "$Prefix$i"
    $named = @{ Tag = $t; Route = "race"; FrameTest = $true; Settle = $Settle; Gap = 25 }
    $a = @("RSX_PRESENT_SYNC=0", "GT5P_STALL_MS=1000", "GT5P_STALL_DUMPS=3", "GT5P_STALL_SECOND_MS=300")
    foreach ($v in $Vars) { if ($v) { $a += "$v" } }
    $o = & "$PSScriptRoot\gt5boot.ps1" @named @a 2>&1 | Out-String
    $ft = ($o -split "`n" | Where-Object { $_ -match "FrameTest" }) -join " "
    if (-not $ft) { "$t no race window: $(($o -split "`n" | Select-Object -Last 3) -join ' | ')"; continue }
    $runs++
    # The longest frame interval inside the measured window.
    $max = if ($ft -match 'max ([\d.,]+) ms') { [double]($Matches[1] -replace ',', '') } else { 0 }
    $ll = @(Select-String -Path "$dir\$t.err" -Pattern '^\[lockline\]' -ErrorAction SilentlyContinue).Count
    $st = @(Select-String -Path "$dir\$t.err" -Pattern '^\[stall\] no present' -ErrorAction SilentlyContinue).Count
    $frz = $max -ge 1000
    if ($frz) { $freezes++ }
    "{0}: max {1:N0} ms{2}; lockline reports {3}; stall dumps {4}" -f $t, $max, $(if ($frz) { " FREEZE" } else { "" }), $ll, $st
    if ($frz -and $StopOnFreeze) { break }
}
"runs with a race window: $runs; with a frame gap >= 1 s: $freezes"
