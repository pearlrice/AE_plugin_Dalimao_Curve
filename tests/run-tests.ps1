$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$sdkRoot = if ($env:AE_SDK_ROOT) {
    $env:AE_SDK_ROOT
} else {
    Join-Path $projectRoot 'AfterEffectsSDK_25.6_61_win\ae25.6_61.64bit.AfterEffectsSDK'
}
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$outputDirectory = Join-Path $projectRoot 'x64\tests'

if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "VS2022 x64 compiler environment was not found: $vcvars"
}
if (-not (Test-Path -LiteralPath (Join-Path $sdkRoot 'Examples\Headers\AE_GeneralPlug.h'))) {
    throw "After Effects SDK headers were not found under: $sdkRoot"
}
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

$headers = Join-Path $sdkRoot 'Examples\Headers'
$spHeaders = Join-Path $headers 'SP'
$util = Join-Path $sdkRoot 'Examples\Util'
$modelSource = Join-Path $PSScriptRoot 'curve_preset_model_tests.cpp'
$integrationSource = Join-Path $PSScriptRoot 'curve_preset_integration_tests.cpp'
$suiteHandlerSource = Join-Path $projectRoot 'DalimaoCurves\AEGP_SuiteHandler.cpp'
$modelExe = Join-Path $outputDirectory 'curve_preset_model_tests.exe'
$integrationExe = Join-Path $outputDirectory 'curve_preset_integration_tests.exe'
$objectDirectory = ($outputDirectory -replace '\\', '/') + '/'

$commands = @(
    ('call "' + $vcvars + '" >nul')
    ('cd /d "' + $projectRoot + '"')
    ('cl /nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /Fo"' + $objectDirectory + '" ' +
        '/Fd:"' + (Join-Path $outputDirectory 'model-compiler.pdb') + '" "' + $modelSource + '" /Fe:"' + $modelExe + '"')
    ('"' + $modelExe + '"')
    ('cl /nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /DWIN32 /D_WINDOWS /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ' +
        '/Fo"' + $objectDirectory + '" /Fd:"' + (Join-Path $outputDirectory 'integration-compiler.pdb') + '" ' +
        '/I"' + (Join-Path $projectRoot 'DalimaoCurves') + '" /I"' + $util + '" /I"' + $headers + '" /I"' + $spHeaders + '" ' +
        '"' + $integrationSource + '" "' + $suiteHandlerSource + '" ' +
        'user32.lib gdi32.lib gdiplus.lib shell32.lib ole32.lib comctl32.lib /Fe:"' + $integrationExe + '"')
    ('"' + $integrationExe + '"')
)

$command = $commands -join ' && '
if ($env:DALIMAO_TEST_VERBOSE) {
    Write-Host $command
}
& $env:ComSpec /d /c $command
if ($LASTEXITCODE -ne 0) {
    throw "Curve preset tests failed with exit code $LASTEXITCODE"
}
