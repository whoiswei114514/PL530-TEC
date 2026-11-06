param([string]$Compiler = 'g++')

$ErrorActionPreference = 'Stop'
$project = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$build = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $build -Force | Out-Null
$compilerPath = (Get-Command $Compiler -CommandType Application).Source
$oldPath = $env:PATH
Push-Location -LiteralPath $project
try {
  $env:PATH = (Split-Path $compilerPath -Parent) + ';' + $oldPath
  $exe = Join-Path $build 'config_store_test.exe'
  & $compilerPath '-std=c++17' '-Wall' '-Wextra' '-Werror' '-pedantic' `
    '-Itests/storage' '-Iinclude' 'src/config_store.cpp' `
    'tests/storage/config_store_test.cpp' '-o' 'tests/storage/build/config_store_test.exe'
  if ($LASTEXITCODE -ne 0) { throw 'Storage test compilation failed.' }
  & $exe
  if ($LASTEXITCODE -ne 0) { throw 'Storage tests failed.' }
} finally {
  $env:PATH = $oldPath
  Pop-Location
}
