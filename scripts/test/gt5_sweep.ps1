# Gran Turismo 5: race routes one after another into one private shader cache,
# to measure how many new driver pipelines each track or car adds.
#   scripts\test\gt5_sweep.ps1 -Tag s -Cache scratch\sweep\cache -Runs "0:0,6:0,1:0" [-Settle 40] [-FrameTest] NAME=VALUE ...
#   -Runs: comma-separated <track rights>:<car rights> (gt5boot.ps1 -Track/-Car;
#   0:0 is High Speed Ring with the first car). Each run is gt5boot.ps1
#   -Route race -Tag <Tag>-<i>; check its shots for the track and car.
#   -FrameTest holds the throttle, so the car moves (no steering).
# Prints per run: recipes before/after, new, race-set size, badge seconds, and
# appends the same to scratch\pv\<Tag>.sweep.tsv. The cache is never one the
# player uses unless named: pass a private directory.
[CmdletBinding(PositionalBinding = $false)]
param([Parameter(Mandatory)] [string]$Tag, [Parameter(Mandatory)] [string]$Cache, [Parameter(Mandatory)] [string]$Runs,
      [int]$Settle = 40, [int]$Gap = 20, [switch]$FrameTest,
      [Parameter(ValueFromRemainingArguments = $true)] $Vars)
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Cache = [IO.Path]::GetFullPath((Join-Path $root $Cache))
New-Item -ItemType Directory -Force $Cache | Out-Null
$tsv = "$root\scratch\pv\$Tag.sweep.tsv"
function Get-Recipes { @(Get-ChildItem "$Cache\*\warmup-v1\pso_*.recipe" -ErrorAction SilentlyContinue | ForEach-Object Name) }
function Get-RaceSet { $f = @(Get-ChildItem "$Cache\*\warmup-v1\race-v1.txt" -ErrorAction SilentlyContinue)
    if ($f) { @(Get-Content $f[0].FullName | Where-Object { $_.Trim() }).Count } else { 0 } }
if (-not (Test-Path $tsv)) { "run`ttrack`tcar`tbefore`tafter`tnew`trace_set`tbadge_s`tbadges`treached" | Set-Content $tsv }
$i = 0
foreach ($r in $Runs -split ",") {
    $track, $car = [int[]]($r -split ":"); $i++
    $t = "$Tag-$i"; $before = Get-Recipes
    $a = @{ Tag = $t; Route = "race"; Track = $track; Car = $car; Settle = $Settle; Gap = $Gap }
    if ($FrameTest) { $a.FrameTest = $true }
    [string[]]$v = @($Vars | Where-Object { $_ }) + @("RSX_SHADER_CACHE=$Cache")
    # A boot can stall at the main menu (seen once in seven); retry once.
    for ($try = 0; $try -lt 2; $try++) {
        & "$PSScriptRoot\gt5boot.ps1" @a @v | Out-Host
        if (Select-String -Path "$root\scratch\pv\$t.out" -Pattern '\[milestone\] RaceStart' -Quiet -ErrorAction SilentlyContinue) { break }
    }
    $after = Get-Recipes
    $new = @($after | Where-Object { $before -notcontains $_ }).Count
    $badge = 0.0; $n = 0
    Select-String -Path "$root\scratch\pv\$t.err" -Pattern '^\[shader-badge\] shown ([0-9.]+) s' -ErrorAction SilentlyContinue |
        ForEach-Object { $badge += [double]$_.Matches[0].Groups[1].Value; $n++ }
    $reached = [bool](Select-String -Path "$root\scratch\pv\$t.out" -Pattern '\[milestone\] RaceStart' -Quiet -ErrorAction SilentlyContinue)
    $row = "$t`t$track`t$car`t$($before.Count)`t$($after.Count)`t$new`t$(Get-RaceSet)`t$([Math]::Round($badge, 1))`t$n`t$reached"
    Add-Content $tsv $row; "SWEEP $row"
}
