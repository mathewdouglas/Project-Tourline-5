# Renderer-failure checks with RSX_FAIL_TEST (runtime switch).
#  -Mode silent     launcher, PT5_SETUP_UI=0: report saved, no dialog, no restart
#  -Mode noreport   launcher, PT5_SETUP_UI=0, PT5_CRASH_REPORT=0
#  -Mode dialog     launcher with its dialog: "Save report and restart", then the
#                   restarted engine fails again and the dialog is closed
#  -Mode direct     engine without the launcher: its own message box
# -Op close|present|remove[:frame] is RSX_FAIL_TEST (docs/renderer-errors.md).
# -Work is a private test folder: engine/ (gt5_211.exe, tourline5.exe and a
# pt5-release.ini with the engine's SHA-256) and home/ (a launcher home with
# a 2.11 setup, PT5_LAUNCHER_HOME). -GameDir is the disc's PS3_GAME folder,
# for -Mode direct. Logs, dialog shots and copies of play.err go to -Work.
param([Parameter(Mandatory)][string]$Work,[string]$Mode='silent',[string]$Op='close:300',[string]$Tag='',[string]$GameDir='')
$ErrorActionPreference='Stop'
$work=(Resolve-Path $Work).Path -replace '\\','/'
if(!$Tag){$Tag="fatal-$Mode-$($Op -replace '[:]','-')"}
if (@(Get-Process gt5,gt5_211,tourline5 -ErrorAction SilentlyContinue).Count) { throw 'Game slot in use' }
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes,System.Drawing
if(-not ('FatalShot' -as [type])){Add-Type @'
using System; using System.Runtime.InteropServices;
public class FatalShot { [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f); [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r); [StructLayout(LayoutKind.Sequential)] public struct RECT {public int L,T,R,B;} }
'@}
function Shot($hwnd,$name){
 $r=New-Object FatalShot+RECT;[FatalShot]::GetWindowRect([IntPtr]$hwnd,[ref]$r)|Out-Null
 $bmp=New-Object System.Drawing.Bitmap ([Math]::Max(16,$r.R-$r.L)),([Math]::Max(16,$r.B-$r.T));$g=[System.Drawing.Graphics]::FromImage($bmp);$dc=$g.GetHdc()
 try{[FatalShot]::PrintWindow([IntPtr]$hwnd,$dc,2)|Out-Null}finally{$g.ReleaseHdc($dc);$g.Dispose()}
 $bmp.Save("$work/$name.png");$bmp.Dispose()
}
function FindWindowByTitle($title,[int]$seconds){
 $root=[System.Windows.Automation.AutomationElement]::RootElement
 $cond=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,$title)
 for($i=0;$i -lt $seconds*5;$i++){ $w=$root.FindFirst([System.Windows.Automation.TreeScope]::Children,$cond); if($w){return $w}; Start-Sleep -Milliseconds 200 }
 return $null
}
function Press($win,$prefix){
 $all=$win.FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.Condition]::TrueCondition)
 foreach($e in $all){ $n=$e.Current.Name; if($n -and $n.StartsWith($prefix) -and $e.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button){
   $e.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke(); return $n } }
 throw "No button starting '$prefix' in '$($win.Current.Name)'"
}
function Names($win){ ($win.FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.Condition]::TrueCondition) | ForEach-Object { $_.Current.Name } | Where-Object { $_ }) -join ' | ' }
function FatalLines($file){ if(Test-Path $file){ @(Select-String -Path $file -Pattern '\[renderer-fatal\] operation=' | ForEach-Object Line) } else { @() } }
$reports="$work/home/logs/reports"
$before=@(if(Test-Path $reports){Get-ChildItem $reports -Filter *.zip | ForEach-Object Name})
"== ${Tag}: RSX_FAIL_TEST=$Op"
$env:RSX_FAIL_TEST=$Op;$env:PS3_VERBOSE='0';$env:FS_HIDE='grim/file/contents'
Remove-Item Env:GT5P_SETTINGS_UI,Env:PT5_CRASH_REPORT,Env:PT5_SETUP_UI -ErrorAction SilentlyContinue
$owned=@()
try{
if($Mode -eq 'direct'){
 $env:GT5P_ROOT="$work/home/targets/BCUS98114-v211";if(!$GameDir){throw '-Mode direct needs -GameDir'};$env:GT5P_GAME_DIR=$GameDir
 $env:GT5P_NO_SETTINGS='1';$env:GT5P_RESOLUTION='720p'
 Remove-Item Env:GT5P_LAUNCHER_RESTART -ErrorAction SilentlyContinue
 $p=Start-Process "$work/engine/gt5_211.exe" -WorkingDirectory "$work/home/targets/BCUS98114-v211" -PassThru -RedirectStandardOutput "$work/$Tag.out" -RedirectStandardError "$work/$Tag.err"
 $owned+=$p;$null=$p.Handle
 $box=FindWindowByTitle 'Project Tourline 5 - graphics error' 120
 if(!$box){throw 'No engine message box'}
 "box: $(Names $box)"
 Shot $box.Current.NativeWindowHandle "$Tag-box"
 Press $box 'OK' | Out-Null
 if(!$p.WaitForExit(30000)){throw 'Engine did not exit after OK'}
 'engine exit 0x{0:X8}' -f $p.ExitCode
 "fatal lines: $((FatalLines "$work/$Tag.err").Count)"; FatalLines "$work/$Tag.err"
 return
}
$env:PT5_LAUNCHER_HOME="$work/home";$env:PT5_STORAGE_LINK='0';$env:PT5_VOLMAP='0'
Remove-Item Env:GT5P_ROOT,Env:GT5P_GAME_DIR,Env:GT5P_SETTINGS,Env:PAD_HOLD,Env:GT5P_NO_SETTINGS -ErrorAction SilentlyContinue
if($Mode -ne 'dialog'){$env:PT5_SETUP_UI='0'}
if($Mode -eq 'noreport'){$env:PT5_CRASH_REPORT='0'}
$l=Start-Process "$work/engine/tourline5.exe" -ArgumentList '--target=02.11' -WorkingDirectory "$work/engine" -PassThru -RedirectStandardOutput "$work/$Tag-launcher.out" -RedirectStandardError "$work/$Tag-launcher.err"
$owned+=$l;$null=$l.Handle
if($Mode -ne 'dialog'){
 if(!$l.WaitForExit(180000)){throw 'Launcher did not exit'}
 'launcher exit 0x{0:X8}' -f $l.ExitCode
 "launcher stderr: $((Get-Content "$work/$Tag-launcher.err" -Raw))"
 "fatal lines in play.err: $((FatalLines "$work/home/logs/play.err").Count)"; FatalLines "$work/home/logs/play.err"
 Copy-Item "$work/home/logs/play.err" "$work/$Tag-play.err"
} else {
 $engines=@()
 foreach($round in 1,2){
  $dlg=FindWindowByTitle 'Project Tourline 5 - problem report' 150
  if(!$dlg){throw "No report dialog (round $round)"}
  Get-Process gt5_211 -ErrorAction SilentlyContinue | ForEach-Object { "round $round engine still alive: $($_.Id)" }
  "round $round dialog: $(Names $dlg)"
  Shot $dlg.Current.NativeWindowHandle "$Tag-dialog$round"
  Copy-Item "$work/home/logs/play.err" "$work/$Tag-play$round.err"
  "round $round fatal lines: $((FatalLines "$work/home/logs/play.err").Count)"; FatalLines "$work/home/logs/play.err"
  if($round -eq 1){
   "pressed: $(Press $dlg 'Save report and restart')"
   $e=$null;for($n=0;$n -lt 150;$n++){$e=Get-Process gt5_211 -ErrorAction SilentlyContinue | Select-Object -First 1;if($e){break};Start-Sleep -Milliseconds 200}
   if(!$e){throw 'No engine after restart'}
   "restarted engine $($e.Id)"
  } else {
   "pressed: $(Press $dlg 'Close')"
   if(!$l.WaitForExit(30000)){throw 'Launcher did not exit after Close'}
   'launcher exit 0x{0:X8}' -f $l.ExitCode
   Start-Sleep -Seconds 3
   $again=@(Get-Process gt5_211 -ErrorAction SilentlyContinue)
   "engines after close: $($again.Count)"
  }
 }
}
$after=@(if(Test-Path $reports){Get-ChildItem $reports -Filter *.zip | ForEach-Object Name})
$new=@($after | Where-Object { $before -notcontains $_ })
"new reports: $($new -join ', ')"
}finally{
 foreach($o in $owned){ if($o -and !$o.HasExited){Stop-Process -Id $o.Id -Force -ErrorAction SilentlyContinue} }
 Get-Process gt5_211 -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$($work -replace '/','\')*" } | Stop-Process -Force -ErrorAction SilentlyContinue
 foreach($k in 'RSX_FAIL_TEST','PT5_LAUNCHER_HOME','PT5_STORAGE_LINK','PT5_VOLMAP','PT5_SETUP_UI','PT5_CRASH_REPORT','GT5P_ROOT','GT5P_GAME_DIR','GT5P_NO_SETTINGS','GT5P_RESOLUTION'){Remove-Item "Env:$k" -ErrorAction SilentlyContinue}
}
