# Native synthetic/SDL virtual-controller checks; no game data or hardware effects.
param([string]$Runtime = '', [string]$Sdl = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (!$Runtime) { $Runtime = $env:PS3RECOMP_DIR }
if (!$Runtime) { $Runtime = Join-Path (Split-Path $root -Parent) 'ps3recomp-tourline5' }
if (!$Sdl) { $Sdl = Join-Path $root 'build/SDL2.dll' }
$dir = Join-Path $root ('scratch/controls-ports-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $dir | Out-Null
Copy-Item -LiteralPath $Sdl -Destination "$dir/SDL2.dll"
@"
@echo off
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
"C:\Program Files\LLVM\bin\clang-cl.exe" /nologo /O2 /Gy /MD /std:c17 /D_CRT_SECURE_NO_WARNINGS /I"$Runtime" /I"$Runtime/include" /Fo"$dir/test.obj" /Fe"$dir/test.exe" "$PSScriptRoot/controls_polish.c" /link /OPT:REF user32.lib
"@ | Set-Content -LiteralPath "$dir/build.cmd" -Encoding ASCII
& cmd /c "$dir/build.cmd"
if ($LASTEXITCODE) { throw 'Controls fixture build failed' }
$names = @('PAD_MODERN_CONTROLS','PAD_ADAPTIVE_TRIGGERS','PAD_TRIGGER_RUMBLE','PAD_SDL_RUMBLE','PAD_CONTROLS_POLISH','PAD_NO_SDL')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }
try {
    foreach ($name in $names) { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
    $env:PAD_MODERN_CONTROLS='1'; $env:PAD_ADAPTIVE_TRIGGERS='1'; $env:PAD_TRIGGER_RUMBLE='1'; $env:PAD_SDL_RUMBLE='1'
    foreach ($mode in @('shapes','packets','effects','capability-size','feedback-hints','guest-feedback','ports','worker')) {
        & "$dir/test.exe" $mode
        if ($LASTEXITCODE) { throw "Controls $mode failed" }
    }
    $env:PAD_CONTROLS_POLISH='0'
    & "$dir/test.exe" rollback
    if ($LASTEXITCODE) { throw 'Controls rollback failed' }
    $env:PAD_NO_SDL='1'
    & "$dir/test.exe" fallback
    if ($LASTEXITCODE) { throw 'Controls fallback failed' }
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
}
"Evidence: $dir"
