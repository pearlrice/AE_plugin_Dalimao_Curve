$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$source = Join-Path $PSScriptRoot 'popup_ui_preview.cpp'
$output = Join-Path $root 'x64\tests'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$exe = Join-Path $output 'popup_ui_preview.exe'
$command = 'call "' + $vcvars + '" >nul && cl /nologo /std:c++20 /EHsc /W4 /utf-8 "' + $source + '" /Fo"' + $output + '\popup_ui_preview.obj" /Fe:"' + $exe + '"'
& $env:ComSpec /d /c $command
if ($LASTEXITCODE -ne 0) { throw 'Popup preview build failed' }
& $exe (Join-Path $output 'popup-preview.png') 96
if ($LASTEXITCODE -ne 0) { throw 'Popup preview failed' }
& $exe (Join-Path $output 'popup-preview-150.png') 144
if ($LASTEXITCODE -ne 0) { throw 'Popup 150% preview failed' }
& $exe (Join-Path $output 'popup-preview-error.png') 144 error
if ($LASTEXITCODE -ne 0) { throw 'Popup wrapped error preview failed' }
