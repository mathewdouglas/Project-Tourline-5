# Live audio route; gt5boot owns and stops only its own process.
# Invoke only after the measurement owner's handoff. The guard is repeated
# immediately before every launch; shader caches are neither moved nor deleted.
[CmdletBinding()]
param(
 [Parameter(Mandatory)][string]$Exe,
 [ValidateSet('menu','movie','race')][string]$Route='menu',
 [string]$Tag='audio-live',
 [ValidateSet(0,1)][int]$Native=1,
 [ValidateSet(0,1)][int]$Scrub=1,
 [switch]$DefaultScrub,
 [int]$Settle=30
)
$ErrorActionPreference='Stop'
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$record=Join-Path $root 'scratch/audio-live'
New-Item -ItemType Directory -Force $record | Out-Null
$vars=@('PS3_VERBOSE=0','PS3_HEADLESS=1','GT5P_NO_SETTINGS=1',
 'AUDIO_RATE=1','AUDIO_PEAK=1',"GT5P_AUDIO_NATIVE=$Native","AUDIO_NONFINITE_SCRUB=$Scrub",
 "AUDIO_WAV=$record/$Tag.f32",'PAD_SKIP_MOVIE=0')
if($DefaultScrub){$vars=@($vars | Where-Object {$_ -notlike 'AUDIO_NONFINITE_SCRUB=*'})}
$h=@{Tag=$Tag;Exe=$Exe;Settle=$Settle;Gap=10}
if($Route -eq 'movie'){$h.Movie=$true;$h.ShotsFrom='OpeningMovie'}
elseif($Route -eq 'race'){$h.Route='race';$h.ShotsFrom='RaceStart';$h.ExtraHold='@RaceStart+1000-900000:CROSS'}
else{$h.Route='menu'}
# Own read-only wait guard: 20 consecutive seconds free, then gt5boot rechecks.
$quiet=0
while($quiet -lt 20){
 if(@(Get-Process gt5,gt5p -ErrorAction SilentlyContinue).Count){$quiet=0}else{$quiet++}
 Start-Sleep -Seconds 1
}
$sha=(Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash
@{route=$Route;native=$Native;scrub=$(if($DefaultScrub){'default'}else{$Scrub});exe=$Exe;sha256=$sha;verbose=0;started=(Get-Date).ToString('o')} |
 ConvertTo-Json | Set-Content "$record/$Tag.run.json"
$previous=[Environment]::GetEnvironmentVariable('AUDIO_NONFINITE_SCRUB','Process')
try {
 if($DefaultScrub){[Environment]::SetEnvironmentVariable('AUDIO_NONFINITE_SCRUB',$null,'Process')}
 & "$PSScriptRoot/gt5boot.ps1" @h @vars | Tee-Object -FilePath "$record/$Tag.run.txt"
} finally {
 if($DefaultScrub){[Environment]::SetEnvironmentVariable('AUDIO_NONFINITE_SCRUB',$previous,'Process')}
}
