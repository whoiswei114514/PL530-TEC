param([string]$Compiler = 'g++')

$ErrorActionPreference = 'Stop'
$project = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$build = Join-Path $project '.pio\native-tests'
New-Item -ItemType Directory -Path $build -Force | Out-Null
$compilerPath = (Get-Command $Compiler -CommandType Application).Source
$oldPath = $env:PATH
Push-Location -LiteralPath $project
try {
  $env:PATH = (Split-Path $compilerPath -Parent) + ';' + $oldPath
  & $compilerPath '-std=c++17' '-Wall' '-Wextra' '-Werror' '-DARDUINO=1' '-DESP32=1' `
    '-Itests/native' '-Iinclude' 'src/thermal_controller.cpp' `
    'tests/native/controller_safety_test.cpp' '-o' '.pio/native-tests/controller_safety_test.exe'
  if ($LASTEXITCODE -ne 0) { throw 'Native test compilation failed.' }
  & '.\.pio\native-tests\controller_safety_test.exe'
  if ($LASTEXITCODE -ne 0) { throw 'Native controller safety tests failed.' }
  & $compilerPath '-std=c++17' '-Wall' '-Wextra' '-Werror' '-DARDUINO=1' '-DESP32=1' `
    '-Itests/native' '-Iinclude' 'src/tmp117_sensor.cpp' `
    'tests/native/sensor_test.cpp' '-o' '.pio/native-tests/sensor_test.exe'
  if ($LASTEXITCODE -ne 0) { throw 'Native sensor test compilation failed.' }
  & '.\.pio\native-tests\sensor_test.exe'
  if ($LASTEXITCODE -ne 0) { throw 'Native sensor tests failed.' }
} finally {
  $env:PATH = $oldPath
  Pop-Location
}
