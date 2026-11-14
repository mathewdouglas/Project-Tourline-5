# Private SPU experiment run. One game at a time; original runner owns cleanup.
[CmdletBinding()]
param(
 [ValidateSet('HSR','Eiger','CarList','Menu','Movie')][string]$Case='HSR',
 [Parameter(Mandatory=$true)][string]$Tag,
 [string]$Exe='scratch/spu-codegen/cache-only/player/gt5.exe',
 [int]$Slots=16, [int]$Settle=60, [int]$SampleDelay=20,
 [string]$Trace='', [switch]$Verify,
 [string]$CaptureFile='', [string]$AudioFile='',
 [string[]]$Extra=@()
)
$ErrorActionPreference='Stop'
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not [IO.Path]::IsPathRooted($Exe)) { $Exe=Join-Path $root $Exe }
if (-not (Test-Path -LiteralPath $Exe)) { throw "Missing private exe: $Exe" }
$record=Join-Path $root 'scratch/spu-codegen/live'
New-Item -ItemType Directory -Force $record | Out-Null
if (Test-Path "$record/$Tag.json") { throw "Run tag already exists: $Tag" }
. "$PSScriptRoot/milestone.ps1"
$vars=@('PS3_VERBOSE=0','GT5P_RESOLUTION=720p','RSX_MSAA=2','RSX_PRESENT_SYNC=0',
 'RSX_SHADER_COMPILER=dxc','RSX_SHADER_WARMUP=1','AUDIO_RATE=1','AUDIO_PEAK=1',
 'AUDIO_NONFINITE_SCRUB=0',"SPU_JOB_FP_SLOTS=$Slots")
if ($Trace) { $vars+="SPU_JOB_FP_TRACE=$Trace"; $vars+='SPU_JOB_FP_STATS=1' }
if ($Verify) { $vars+='SPU_JOB_FP_VERIFY=1'; $vars+='SPU_JOB_FP_STATS=1' }
if ($CaptureFile) { $vars+="GT5P_SPU_CAPTURE=$CaptureFile"; $vars+='GT5P_SPU_CAPTURE_N=16' }
if ($AudioFile) { $vars+="AUDIO_WAV=$AudioFile" }
$h=@{Tag=$Tag;Settle=$Settle;Gap=20;Exe=$Exe;SampleDelay=$SampleDelay;Cpu=$true}
if ($Case -eq 'Movie') { $h.Movie=$true; $vars+='PAD_SKIP_MOVIE=0' }
else {
 $h.FrameTest=$true; $h.Route='race'
 if ($Case -eq 'CarList') { $h.Route='carlist';$h.ShotsFrom='CarList' }
 if ($Case -eq 'Menu') { $h.Route='menu' }
 if ($Case -eq 'Eiger') {
  $s=@("$GT5BootHold",'@MusicDecode+5000-5100:RIGHT')
  for($t=7000;$t -le 60000;$t+=3000) { $s+="@MusicDecode+$t-$($t+200)!TrackList:CROSS" }
  for($k=0;$k -lt 6;$k++) { $t=3000+700*$k;$s+="@TrackList+$t-$($t+150):RIGHT" }
  for($t=9000;$t -le 120000;$t+=3000) { $s+="@TrackList+$t-$($t+200)!RaceLoad:CROSS" }
  for($t=15000;$t -le 75000;$t+=3000) { $s+="@RaceLoad+$t-$($t+200)!RaceStart:CROSS" }
  $s+='@RaceStart+1000-900000:CROSS';$vars+='PAD_HOLD='+($s -join ',')
 }
}
$vars+= $Extra
$manifest=[ordered]@{tag=$Tag;case=$Case;exe=$Exe;sha256=(Get-FileHash -Algorithm SHA256 $Exe).Hash;
 slots=$Slots;settle=$Settle;sample_delay=$SampleDelay;vars=$vars;started_utc=[DateTime]::UtcNow.ToString('o')}
$manifest | ConvertTo-Json -Depth 5 | Set-Content "$record/$Tag.json"
$log=& "$PSScriptRoot/gt5boot.ps1" @h @vars
$log | Set-Content "$record/$Tag.txt"
$log
if ($Case -ne 'Movie' -and -not (Test-Path "$root/scratch/pv/$Tag.frames.json")) {
 throw "No measured window for $Tag; inspect logs"
}
