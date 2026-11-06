param(
    [string]$Compiler = "g++"
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$build = Join-Path $PSScriptRoot ".build"
New-Item -ItemType Directory -Path $build -Force | Out-Null
$exe = Join-Path $build "api_integration_test.exe"
$oldPath = $env:PATH
Push-Location $root
try {
    $env:PATH = "$(Split-Path -Parent $Compiler);$oldPath"
    & $Compiler -std=c++17 -Wall -Wextra -Werror -pedantic `
        -Itests/integration/mocks -Iinclude `
        tests/integration/api_integration_test.cpp -o tests/integration/.build/api_integration_test.exe
    if ($LASTEXITCODE -ne 0) { throw "Integration test compilation failed ($LASTEXITCODE)" }
    & node (Join-Path $PSScriptRoot "verify_json.cjs") $exe
    if ($LASTEXITCODE -ne 0) { throw "Integration tests failed ($LASTEXITCODE)" }
} finally {
    Pop-Location
    $env:PATH = $oldPath
}
