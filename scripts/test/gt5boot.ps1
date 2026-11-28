# Gran Turismo 5: boot to the main menu with milestone-timed presses
# (milestone.ps1, $GT5BootHold), idle there -Settle seconds with a window shot
# every -Gap seconds, and summarise the frame rate.
#   scripts\test\gt5boot.ps1 -Tag t [-Settle 60] [-Gap 10] [-Fresh] [-FreshSave] NAME=VALUE ...
#   -Fresh: first-boot install state (scripts\test\gt5_reset.py), so the
#   install prompts run; -FreshSave also removes the save (GT5 creates it).
#   -Movie: no presses; shots start at installed00.dat instead of the menu,
#   so they cover the logos and the opening movie of an installed boot.
#   -Route carlist|grid|race: drive on to the car list ($GT5CarListHold), the
#   race's grid screen ($GT5GridHold) or into the race ($GT5RaceHold); a
#   PAD_HOLD=... argument still overrides.
#   -ShotsFrom <milestone>: start the shots there instead (grid: RaceLoad,
#   race: RaceStart).
#   -FrameTest (with -Route race): hold Cross (throttle) from RaceStart, log
#   every presented frame (GT5P_FRAME_LOG) and report frames, mean FPS,
#   median/p99/max interval, late (>20 ms) share and FIFO walk ms/frame over
#   the -Settle window after -SampleDelay s (default 20), to
#   scratch\pv\<t>.frames.txt. -Cpu adds per-thread CPU over the window.
#   Each screen milestone reached is printed with its time after the menu.
#   -Track <n> / -Car <n> (with -Route race): Rights on the track list / car
#   list (Get-GT5RaceRoute; -Track 6 is Eiger Nordwand). -Tab <n>: the track
#   list tab first (2 = City; -Tab 2 alone is Rome). Check the shots.
#   -Exe <path>: run another build of gt5.exe (default build\gt5.exe), e.g.
#   build_pgo_use\gt5.exe (scripts\pgo\pgo.py).
#   -ExtraHold "<presses>": appended to the PAD_HOLD script (e.g. a camera
#   change: "@RaceStart+3000-3200:TRIANGLE").
#   -Monza: the full-field slow stretch after a Monza start (progress-gt5.md,
#   2026-11-26): -Route race -Tab 1 -Track 2 -Car 37 -FrameTest, sampled from
#   2 s after RaceStart for 30 s (any of those given explicitly wins), with
#   GT5P_PRESENT_PROF, RSX_DRAWPROF and RSX_REPLAYPROF on. Besides the frame
#   test it averages their lines printed inside the window (recording stages,
#   replay stages, same-pipeline runs, walk/GPU) into scratch\pv\<t>.prof.txt.
#   -> scratch\pv\<t>.out/.err, <t>.menu<N>.png
[CmdletBinding(PositionalBinding = $false)]
param([string]$Tag = "gt5", [int]$Settle = 60, [int]$Gap = 10, [switch]$Fresh, [switch]$FreshSave, [switch]$Movie,
      [ValidateSet("menu", "carlist", "grid", "race")][string]$Route = "menu", [string]$ShotsFrom = "",
      [switch]$FrameTest, [int]$SampleDelay = 20, [switch]$Cpu, [string]$ExtraHold = "", [string]$Exe = "",
      [int]$Track = 0, [int]$Car = 0, [int]$Tab = 0, [switch]$Monza,
      [Parameter(ValueFromRemainingArguments = $true)] $Vars)
if ($Monza) {
    $Route = "race"; $FrameTest = $true
    if (-not $PSBoundParameters.ContainsKey("Tab")) { $Tab = 1 }
    if (-not $PSBoundParameters.ContainsKey("Track")) { $Track = 2 }
    if (-not $PSBoundParameters.ContainsKey("Car")) { $Car = 37 }
    if (-not $PSBoundParameters.ContainsKey("SampleDelay")) { $SampleDelay = 2 }
    if (-not $PSBoundParameters.ContainsKey("Settle")) { $Settle = 30 }
}
. "$PSScriptRoot\milestone.ps1"
Add-Type -AssemblyName System.Drawing
if (-not ("WinShot" -as [type])) {
Add-Type @'
using System; using System.Runtime.InteropServices;
public class WinShot { [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
 [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; } }
'@ }
if ($Cpu -and -not ("SpuThreadName" -as [type])) {
Add-Type @'
using System; using System.Runtime.InteropServices;
public class SpuThreadName {
 [DllImport("kernel32.dll")] static extern IntPtr OpenThread(uint a, bool i, uint id);
 [DllImport("kernel32.dll")] static extern int GetThreadDescription(IntPtr h, out IntPtr p);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
 [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr p);
 public static string Get(uint id) { IntPtr h=OpenThread(0x0800,false,id), p=IntPtr.Zero;
  if(h==IntPtr.Zero) return "";
  try { return GetThreadDescription(h,out p)>=0 ? Marshal.PtrToStringUni(p) : ""; }
  finally { if(p!=IntPtr.Zero) LocalFree(p); CloseHandle(h); }
 }
}
'@ }
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$dir = "$root\scratch\pv"; New-Item -ItemType Directory -Force $dir | Out-Null
$set = @{ GT5P_NO_SETTINGS = "1"; FS_HIDE = "grim/file/contents"; PS3_VERBOSE = "0"; GT5P_PROFVIEW = "0";
          PAD_MILESTONE_FILES = $GT5MilestoneFiles; PAD_HOLD = $GT5BootHold }
if ($Route -eq "carlist") { $set.PAD_HOLD = $GT5CarListHold }
if ($Route -eq "grid") { $set.PAD_HOLD = $GT5GridHold; if (-not $ShotsFrom) { $ShotsFrom = "RaceLoad" } }
if ($Route -eq "race") { $set.PAD_HOLD = Get-GT5RaceRoute $Track $Car $Tab; if (-not $ShotsFrom) { $ShotsFrom = "RaceStart" } }
if ($Movie) { $set.Remove("PAD_HOLD") }
if ($FrameTest) { $set.GT5P_FRAME_LOG = "$dir\$Tag.frames.csv"; $set.PAD_HOLD += ",@RaceStart+1000-900000:CROSS" }
if ($ExtraHold) { $set.PAD_HOLD += ",$ExtraHold" }
if ($Monza) { $set.GT5P_PRESENT_PROF = "1"; $set.RSX_DRAWPROF = "1"; $set.RSX_REPLAYPROF = "1" }
foreach ($v in $Vars) { if (-not $v) { continue }; $k, $val = "$v" -split "=", 2; $set[$k] = $val }
$p = $null
function Stop-GT5 {
    # Only this script's child belongs to us. Another test session may be
    # waiting to launch; never stop its game during startup or cleanup.
    if ($p -and -not $p.HasExited) {
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        $p.WaitForExit(10000) | Out-Null
    }
}
try {
if ($Fresh -or $FreshSave) {
    $a = @("$PSScriptRoot\gt5_reset.py"); if ($FreshSave) { $a += "--save" }
    & "$root\.venv\Scripts\python.exe" @a
}
foreach ($k in $set.Keys) { Set-Item "Env:$k" $set[$k] }
if (-not $Exe) { $Exe = "$root\build\gt5.exe" }
while ($other = @(Get-Process gt5 -ErrorAction SilentlyContinue)) {
    "waiting for gt5.exe pid $(($other | ForEach-Object { $_.Id }) -join ', ')"
    Start-Sleep -Seconds 2
}
$p = Start-Process -FilePath $Exe -WorkingDirectory $root -PassThru `
    -RedirectStandardOutput "$dir\$Tag.out" -RedirectStandardError "$dir\$Tag.err"
foreach ($k in $set.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
$tl = New-LogTail "$dir\$Tag.out"
$limit = if ($Fresh -or $FreshSave) { 600 } else { 240 }
$ms = if ($Movie) { "installed00.dat" } else { "MusicDecode" }
$tm = Wait-Milestone $p $tl $ms $limit
$failOn = if ($Route -ne "menu") { "GTLife" } else { "" }   # a race route that opened GT Life missed Arcade
if ($tm -lt 0) { "$ms not reached (log: $dir\$Tag.out)" }
elseif ($ShotsFrom -and ($t2 = Wait-Milestone $p $tl $ShotsFrom $limit -FailOn $failOn) -lt 0) {
    if ($t2 -eq -2) { "route missed Arcade: GT Life opened (log: $dir\$Tag.out)" } else { "$ShotsFrom not reached (log: $dir\$Tag.out)" }
}
else {
    "$ms at $tm s"; if ($ShotsFrom) { "$ShotsFrom $t2 s after that" }
    if ($FrameTest) { Start-Sleep -Seconds $SampleDelay }
    $tS0 = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    $errLine0 = if ($Monza) { @(Get-Content "$dir\$Tag.err" -ErrorAction SilentlyContinue).Count } else { 0 }
    if ($Cpu) { $p.Refresh(); $cpu0 = $p.TotalProcessorTime.TotalSeconds; $thr0 = @{}; foreach ($t in $p.Threads) { $thr0[$t.Id] = $t.TotalProcessorTime.TotalSeconds } }
    $sw = [Diagnostics.Stopwatch]::StartNew(); $i = 0
    while ($sw.Elapsed.TotalSeconds -lt $Settle) {
        Start-Sleep -Seconds $Gap; $i++
        $p.Refresh(); if ($p.HasExited) { "exited"; break }
        $h = $p.MainWindowHandle; if ($h -eq 0) { continue }
        $r = New-Object WinShot+RECT; [WinShot]::GetClientRect($h, [ref]$r) | Out-Null
        $bmp = New-Object System.Drawing.Bitmap ([Math]::Max($r.R, 16)), ([Math]::Max($r.B, 16))
        $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
        [WinShot]::PrintWindow($h, $dc, 3) | Out-Null; $g.ReleaseHdc($dc); $g.Dispose()
        $bmp.Save("$dir\$Tag.menu$i.png"); $bmp.Dispose()
    }
    $tS1 = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    $errLine1 = if ($Monza) { @(Get-Content "$dir\$Tag.err" -ErrorAction SilentlyContinue).Count } else { 0 }
    if ($Cpu -and -not $p.HasExited) {
        $p.Refresh(); $span = ($tS1 - $tS0) / 1000.0
        $rows = foreach ($t in $p.Threads) { $d = $t.TotalProcessorTime.TotalSeconds - [double]$thr0[$t.Id]
            if ($d -gt 0.02 * $span) { [pscustomobject]@{ Tid = $t.Id; PctCore = (100 * $d / $span); CpuSeconds = $d; Name = [SpuThreadName]::Get($t.Id) } } }
        [ordered]@{ sample_start_ms=$tS0; sample_end_ms=$tS1; seconds=$span;
            process_cpu_seconds=($p.TotalProcessorTime.TotalSeconds-$cpu0); threads=@($rows) } |
            ConvertTo-Json -Depth 5 | Set-Content "$dir\$Tag.cpu.json"
        $rep = "process {0:N2} cores over {1:N0} s`n" -f (($p.TotalProcessorTime.TotalSeconds - $cpu0) / $span), $span
        $rep += ($rows | Sort-Object PctCore -Descending | Select-Object -First 20 | Format-Table -AutoSize | Out-String)
        $rep | Set-Content "$dir\$Tag.cpu.txt"; $rep
    }
    if ($FrameTest) {
        Start-Sleep -Milliseconds 1500
        $fr = @(Get-Content "$dir\$Tag.frames.csv" | ForEach-Object {
            $c = $_ -split ","; if ($c.Count -eq 3) { ,([double[]]$c) } } | Where-Object { $_[0] -ge $tS0 -and $_[0] -le $tS1 })
        $iv = @(); for ($k = 1; $k -lt $fr.Count; $k++) { $iv += $fr[$k][0] - $fr[$k - 1][0] }
        if ($iv.Count -gt 10) {
            $sorted = $iv | Sort-Object
            $pct = { param($q) $sorted[[Math]::Min($sorted.Count - 1, [int]($q * $sorted.Count))] }
            $late = @($iv | Where-Object { $_ -gt 20 }).Count
            $walk = ($fr | ForEach-Object { $_[1] } | Measure-Object -Average).Average
            $walkCpu = ($fr | ForEach-Object { $_[2] } | Measure-Object -Average).Average
            $span = ($fr[-1][0] - $fr[0][0]) / 1000.0
            $txt = "frames {0} over {1:N1} s: {2:N1} FPS; interval median {3:N1} ms, p99 {4:N1} ms, max {5:N1} ms; late (>20 ms) {6:N1}%; walk {7:N2} ms/frame (cpu {8:N2})" -f `
                $fr.Count, $span, (($fr.Count - 1) / $span), (& $pct 0.5), (& $pct 0.99), $sorted[-1], (100.0 * $late / $iv.Count), $walk, $walkCpu
            $txt | Set-Content "$dir\$Tag.frames.txt"; "FrameTest: $txt"
            [ordered]@{
                tag = $Tag; route = $Route; exe = $Exe
                sample_start_ms = $tS0; sample_end_ms = $tS1
                frames = $fr.Count; seconds = $span; fps = (($fr.Count - 1) / $span)
                median_ms = (& $pct 0.5); p99_ms = (& $pct 0.99); max_ms = $sorted[-1]
                late_percent = (100.0 * $late / $iv.Count); walk_ms = $walk; walk_cpu_ms = $walkCpu
                spu_dma_fast = $set.SPU_DMA_FAST; ps3_verbose = $set.PS3_VERBOSE
            } | ConvertTo-Json | Set-Content "$dir\$Tag.frames.json"
        } else { "FrameTest: too few frames in the window ($($fr.Count))" }
    }
    if ($Monza) {
        # The 5 s profiler lines printed between the window's start and end.
        $lines = @(Get-Content "$dir\$Tag.err" -ErrorAction SilentlyContinue)
        $last = [Math]::Min($errLine1, $lines.Count) - 1
        $win = if ($last -ge $errLine0) { @($lines[$errLine0..$last]) } else { @() }
        function Get-ProfAvg([string[]]$rows, [string]$pattern) {
            $sum = [ordered]@{}; $n = 0
            foreach ($r in $rows) {
                $n++
                foreach ($m in [regex]::Matches($r, $pattern)) {
                    $k = $m.Groups['k'].Value; $sum[$k] = [double]$sum[$k] + [double]$m.Groups['v'].Value
                }
            }
            if ($n -eq 0) { return "no lines in the window" }
            "($n lines) " + (($sum.Keys | ForEach-Object { "{0} {1:N2}" -f $_, ($sum[$_] / $n) }) -join ", ")
        }
        $dp = @($win | Where-Object { $_ -like '`[drawprof`]*' })
        $rp = @($win | Where-Object { $_ -like '`[replayprof`]*' })
        $pt = @($win | Where-Object { $_ -like '`[present-thread`] 5 s:*' })
        $pp = @($win | Where-Object { $_ -like '`[present-prof`]*' })
        $rpStages = @($rp | ForEach-Object { ($_ -split '\|')[0] })
        $prof = @(
            "recording stages, ms/frame: " + (Get-ProfAvg $dp '(?<k>[a-z_]+) (?<v>[\d.]+) ms \(')
            "replay stages, ms/frame: " + (Get-ProfAvg $rpStages '(?<k>[a-z]+) (?<v>[\d.]+)')
            "replay counts per frame: " + (Get-ProfAvg $rp '(?<v>[\d.]+) (?<k>draws|pipeline changes|target switches)')
            "draws per frame in same-pipeline runs of length: " + (Get-ProfAvg $rp '(?<k>1|2-3|4-7|8-15|16-63|64\+): [\d.]+/(?<v>[\d.]+)')
            "present thread, ms/frame: " + (Get-ProfAvg $pt '(?<k>replay) (?<v>[\d.]+) ms|waited (?<v>[\d.]+) ms (?<k>for it)|(?<v>[\d.]+) ms (?<k>for the GPU)|\((?<v>[\d.]+) (?<k>KiB)\)')
            "present loop, s per 5 s (gpu ms/frame): " + (Get-ProfAvg $pp '(?<k>fifo|present|pump) (?<v>[\d.]+)s|gpu (?<v>[\d.]+) ms (?<k>avg)')
        )
        $prof | Set-Content "$dir\$Tag.prof.txt"
        $prof | ForEach-Object { "Monza: $_" }
    }
}
} finally { Stop-GT5 }
# Screen milestones, in seconds after the menu (MusicDecode).
$mt = @{}; Select-String -Path "$dir\$Tag.out" -Pattern '^\[milestone\] (\S+) at (\d+) ms' |
    ForEach-Object { $mt[$_.Matches[0].Groups[1].Value] = [int]$_.Matches[0].Groups[2].Value }
if ($mt.MusicDecode) {
    ($mt.GetEnumerator() | Where-Object { $_.Value -gt $mt.MusicDecode } | Sort-Object Value |
        ForEach-Object { "{0} +{1:N1}" -f $_.Key, (($_.Value - $mt.MusicDecode) / 1000) }) -join ", "
}
# Frames per 5 s from the present thread, oldest first.
$f = Select-String -Path "$dir\$Tag.err" -Pattern 'present-thread\] 5 s: (\d+) frames' |
     ForEach-Object { $_.Matches[0].Groups[1].Value }
"frames per 5 s: $($f -join ' ')"
