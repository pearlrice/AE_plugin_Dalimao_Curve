$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$vcvars='C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$source=Join-Path $PSScriptRoot 'native_graph_probe.cpp'
$output=Join-Path $root 'x64\tests'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$exe=Join-Path $output 'native_graph_probe.exe'
$command='call "'+$vcvars+'" >nul && cl /nologo /std:c++20 /EHsc /W4 /utf-8 "'+$source+'" /Fo"'+$output+'\native_graph_probe.obj" /Fe:"'+$exe+'"'
& $env:ComSpec /d /c $command
if($LASTEXITCODE -ne 0) { throw 'Probe build failed' }
