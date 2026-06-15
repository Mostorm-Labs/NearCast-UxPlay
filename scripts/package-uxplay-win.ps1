param(
  [string]$Configuration = "Release",
  [string]$Platform = "win32-x64-ucrt64",
  [string]$BuildRoot,
  [string]$MsysRoot,
  [string]$MsysEnvironment = "ucrt64",
  [string]$OutputRoot,
  [string]$PackageName = "UxPlay",
  [switch]$Build,
  [switch]$SkipZip
)

$ErrorActionPreference = "Stop"

function Resolve-FullPath([string]$PathValue) {
  return [System.IO.Path]::GetFullPath($PathValue)
}

function Assert-ChildPath([string]$Parent, [string]$Child) {
  $parentFull = (Resolve-FullPath $Parent).TrimEnd('\', '/')
  $childFull = Resolve-FullPath $Child
  $expectedPrefix = $parentFull + [System.IO.Path]::DirectorySeparatorChar

  if ($childFull -ne $parentFull -and !$childFull.StartsWith($expectedPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to operate outside expected directory. Parent: $parentFull Child: $childFull"
  }
}

function Copy-DirectoryContents([string]$Source, [string]$Destination) {
  if (!(Test-Path $Source)) {
    throw "Source directory not found: $Source"
  }

  New-Item -ItemType Directory -Force -Path $Destination | Out-Null
  Copy-Item -Path (Join-Path $Source "*") -Destination $Destination -Recurse -Force
}

function Invoke-UxPlayBuild(
  [string]$RepoRoot,
  [string]$BuildDir,
  [string]$MsysRootPath,
  [string]$MsysEnvironmentName,
  [string]$BuildConfiguration
) {
  $toolchainRoot = Join-Path $MsysRootPath $MsysEnvironmentName
  $toolchainBin = Join-Path $toolchainRoot "bin"
  $pkgConfigPath = Join-Path $toolchainRoot "lib\pkgconfig"

  if (!(Test-Path $toolchainBin)) {
    throw "MSYS2 $MsysEnvironmentName bin not found: $toolchainBin"
  }

  $env:PATH = "$toolchainBin;$env:PATH"
  $env:PKG_CONFIG_PATH = "$pkgConfigPath"

  Write-Host "Configuring UxPlay..."
  cmake -S $RepoRoot -B $BuildDir -G Ninja `
    -DCMAKE_BUILD_TYPE=$BuildConfiguration `
    -DCMAKE_C_COMPILER="$toolchainBin\gcc.exe" `
    -DCMAKE_CXX_COMPILER="$toolchainBin\g++.exe"

  Write-Host "Building UxPlay..."
  cmake --build $BuildDir --config $BuildConfiguration
}

function Write-UxPlayLauncher([string]$PackageRoot) {
  $launcherPath = Join-Path $PackageRoot "run-uxplay.bat"
  $launcher = @'
@echo off
setlocal
set "UXPLAY_HOME=%~dp0"
set "PATH=%UXPLAY_HOME%;%UXPLAY_HOME%libexec\gstreamer-1.0;%PATH%"
set "GST_PLUGIN_PATH=%UXPLAY_HOME%lib\gstreamer-1.0"
set "GST_PLUGIN_SYSTEM_PATH=%UXPLAY_HOME%lib\gstreamer-1.0"
if exist "%UXPLAY_HOME%libexec\gstreamer-1.0\gst-plugin-scanner.exe" set "GST_PLUGIN_SCANNER=%UXPLAY_HOME%libexec\gstreamer-1.0\gst-plugin-scanner.exe"
"%UXPLAY_HOME%uxplay.exe" %*
endlocal
'@

  Set-Content -LiteralPath $launcherPath -Value $launcher -Encoding ASCII
}

function Write-PackageReadme([string]$PackageRoot) {
  $readmePath = Join-Path $PackageRoot "PACKAGING_README.txt"
  $content = @'
UxPlay Windows standalone package

Run:
  run-uxplay.bat

Pass UxPlay options after the launcher, for example:
  run-uxplay.bat -n "Living Room UxPlay" -fs

The launcher sets PATH, GST_PLUGIN_PATH, GST_PLUGIN_SYSTEM_PATH, and
GST_PLUGIN_SCANNER so the bundled MSYS2 UCRT64/GStreamer runtime is used.

Bonjour note:
  UxPlay loads dnssd.dll at runtime for AirPlay service discovery. If this
  machine does not already have Bonjour installed, run:
    third-party\bonjour\bonjoursdksetup.exe
'@

  Set-Content -LiteralPath $readmePath -Value $content -Encoding ASCII
}

function Compress-PackageWithRetry([string]$SourceRoot, [string]$DestinationPath) {
  $sourcePath = Join-Path $SourceRoot "*"
  $maxAttempts = 5

  for ($attempt = 1; $attempt -le $maxAttempts; $attempt++) {
    try {
      if (Test-Path $DestinationPath) {
        Remove-Item -LiteralPath $DestinationPath -Force
      }
      Compress-Archive -Path $sourcePath -DestinationPath $DestinationPath -Force
      return
    } catch {
      if ($attempt -eq $maxAttempts) {
        throw
      }

      $delaySeconds = $attempt * 2
      Write-Warning "Zip attempt $attempt failed: $($_.Exception.Message)"
      Write-Warning "Retrying in $delaySeconds seconds..."
      Start-Sleep -Seconds $delaySeconds
    }
  }
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-FullPath (Join-Path $scriptDir "..")

if (!$BuildRoot) {
  $BuildRoot = Join-Path $repoRoot "build-ucrt64"
}
if (!$MsysRoot) {
  $MsysRoot = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { "D:\msys64" }
}
if (!$OutputRoot) {
  $OutputRoot = Join-Path $repoRoot "dist"
}

$BuildRoot = Resolve-FullPath $BuildRoot
$MsysRoot = Resolve-FullPath $MsysRoot
$OutputRoot = Resolve-FullPath $OutputRoot

if ($Build) {
  Invoke-UxPlayBuild $repoRoot $BuildRoot $MsysRoot $MsysEnvironment $Configuration
}

$packageRoot = Join-Path $OutputRoot "$PackageName-$Platform"
$zipPath = Join-Path $OutputRoot "$PackageName-$Platform.zip"
$uxplayExe = Join-Path $BuildRoot "uxplay.exe"
$gstPluginSource = Join-Path $BuildRoot "lib\gstreamer-1.0"
$msysGstPluginSource = Join-Path $MsysRoot "$MsysEnvironment\lib\gstreamer-1.0"
$scannerSource = Join-Path $MsysRoot "$MsysEnvironment\libexec\gstreamer-1.0\gst-plugin-scanner.exe"
$bonjourSdkSetup = Join-Path $repoRoot "bonjoursdksetup.exe"

if (!(Test-Path $uxplayExe)) {
  throw "UxPlay executable not found: $uxplayExe. Build UxPlay first or rerun this script with -Build."
}
if (!(Test-Path $gstPluginSource)) {
  if (Test-Path $msysGstPluginSource) {
    $gstPluginSource = $msysGstPluginSource
  } else {
    throw "GStreamer plugin directory not found: $gstPluginSource or $msysGstPluginSource. Install MSYS2 $MsysEnvironment GStreamer packages first."
  }
}

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
Assert-ChildPath $OutputRoot $packageRoot
Assert-ChildPath $OutputRoot $zipPath

if (Test-Path $packageRoot) {
  try {
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
  } catch {
    throw "Unable to remove existing package directory: $packageRoot. Close any process using this folder and try again. $($_.Exception.Message)"
  }
}

if (Test-Path $zipPath) {
  Remove-Item -LiteralPath $zipPath -Force
}

Write-Host "Creating package directory..."
New-Item -ItemType Directory -Force -Path $packageRoot | Out-Null

Write-Host "Copying UxPlay executable and DLLs..."
Copy-Item -LiteralPath $uxplayExe -Destination $packageRoot -Force
Get-ChildItem -Path $BuildRoot -Filter "*.dll" -File | ForEach-Object {
  Copy-Item -LiteralPath $_.FullName -Destination $packageRoot -Force
}

Write-Host "Copying GStreamer plugins..."
Copy-DirectoryContents $gstPluginSource (Join-Path $packageRoot "lib\gstreamer-1.0")

if (Test-Path $scannerSource) {
  Write-Host "Copying GStreamer plugin scanner..."
  $scannerDestination = Join-Path $packageRoot "libexec\gstreamer-1.0"
  New-Item -ItemType Directory -Force -Path $scannerDestination | Out-Null
  Copy-Item -LiteralPath $scannerSource -Destination $scannerDestination -Force
} else {
  Write-Warning "GStreamer plugin scanner not found: $scannerSource"
}

Write-Host "Copying documentation and third-party installer..."
$docRoot = Join-Path $packageRoot "doc"
New-Item -ItemType Directory -Force -Path $docRoot | Out-Null
$docFiles = @("README.md", "README.txt", "README.html", "LICENSE", "uxplay.1")
foreach ($file in $docFiles) {
  $source = Join-Path $repoRoot $file
  if (Test-Path $source) {
    Copy-Item -LiteralPath $source -Destination $docRoot -Force
  }
}

if (Test-Path $bonjourSdkSetup) {
  $bonjourDestination = Join-Path $packageRoot "third-party\bonjour"
  New-Item -ItemType Directory -Force -Path $bonjourDestination | Out-Null
  Copy-Item -LiteralPath $bonjourSdkSetup -Destination $bonjourDestination -Force
} else {
  Write-Warning "Bonjour SDK installer not found: $bonjourSdkSetup"
}

Write-UxPlayLauncher $packageRoot
Write-PackageReadme $packageRoot

$dllCount = (Get-ChildItem -Path $packageRoot -Filter "*.dll" -File).Count
$pluginCount = (Get-ChildItem -Path (Join-Path $packageRoot "lib\gstreamer-1.0") -Filter "*.dll" -File).Count
$packageSize = "{0:N1} MB" -f ((Get-ChildItem -Path $packageRoot -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1MB)

if (!$SkipZip) {
  Write-Host "Creating zip archive..."
  Compress-PackageWithRetry $packageRoot $zipPath
}

Write-Host ""
Write-Host "Packaged: $packageRoot"
if (!$SkipZip) {
  Write-Host "Archive: $zipPath"
}
Write-Host "Executable: $(Join-Path $packageRoot 'uxplay.exe')"
Write-Host "Launcher: $(Join-Path $packageRoot 'run-uxplay.bat')"
Write-Host "UxPlay DLLs: $dllCount"
Write-Host "GStreamer plugins: $pluginCount"
Write-Host "Package size: $packageSize"
