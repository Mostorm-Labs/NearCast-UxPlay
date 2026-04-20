$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$demoRoot = Split-Path -Parent $scriptDir
$nativeRoot = Join-Path $demoRoot "native"
$buildDir = Join-Path $nativeRoot "build"

$msysRoot = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { "D:\msys64" }
$mingwRoot = Join-Path $msysRoot "mingw64"
$mingwBin = Join-Path $mingwRoot "bin"
$pkgConfigPath = Join-Path $mingwRoot "lib\pkgconfig"

if (!(Test-Path $mingwBin)) {
  throw "MSYS2 MinGW bin not found: $mingwBin"
}

$env:PATH = "$mingwBin;$env:PATH"
$env:PKG_CONFIG_PATH = "$pkgConfigPath"

cmake -S $nativeRoot -B $buildDir -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER="$mingwBin\gcc.exe" `
  -DCMAKE_CXX_COMPILER="$mingwBin\g++.exe"

cmake --build $buildDir --config Release
