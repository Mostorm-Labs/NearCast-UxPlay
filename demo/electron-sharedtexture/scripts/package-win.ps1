param(
  [string]$Configuration = "Release",
  [string]$Platform = "win32-x64",
  [string]$BuildRoot,
  [string]$MsysRoot,
  [string]$OutputRoot
)

$ErrorActionPreference = "Stop"

function Resolve-FullPath([string]$PathValue) {
  return [System.IO.Path]::GetFullPath($PathValue)
}

function Assert-ChildPath([string]$Parent, [string]$Child) {
  $parentFull = Resolve-FullPath $Parent
  $childFull = Resolve-FullPath $Child
  if (!$childFull.StartsWith($parentFull, [System.StringComparison]::OrdinalIgnoreCase)) {
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

function Get-NpmPackagePath([string]$NodeModulesRoot, [string]$PackageName) {
  $parts = $PackageName -split "/"
  $packagePath = $NodeModulesRoot
  foreach ($part in $parts) {
    $packagePath = Join-Path $packagePath $part
  }
  return $packagePath
}

function Copy-NpmPackageDependency(
  [string]$PackageName,
  [string]$SourceNodeModulesRoot,
  [string]$DestinationNodeModulesRoot,
  [hashtable]$CopiedPackages
) {
  if ($CopiedPackages.ContainsKey($PackageName)) {
    return
  }
  $CopiedPackages[$PackageName] = $true

  $source = Get-NpmPackagePath $SourceNodeModulesRoot $PackageName
  $destination = Get-NpmPackagePath $DestinationNodeModulesRoot $PackageName

  if (!(Test-Path $source)) {
    throw "Production dependency not found: $source. Run npm install in $(Split-Path -Parent $SourceNodeModulesRoot) first."
  }

  Copy-DirectoryContents $source $destination

  $packageJsonPath = Join-Path $source "package.json"
  if (!(Test-Path $packageJsonPath)) {
    return
  }

  $packageJson = Get-Content -LiteralPath $packageJsonPath -Raw | ConvertFrom-Json
  if (!$packageJson.dependencies) {
    return
  }

  foreach ($dependency in $packageJson.dependencies.PSObject.Properties.Name) {
    Copy-NpmPackageDependency $dependency $SourceNodeModulesRoot $DestinationNodeModulesRoot $CopiedPackages
  }
}

function Copy-NpmProductionDependencies([string]$DemoRoot, [string]$AppRoot) {
  $packageJsonPath = Join-Path $DemoRoot "package.json"
  $nodeModulesRoot = Join-Path $DemoRoot "node_modules"

  if (!(Test-Path $packageJsonPath)) {
    throw "package.json not found: $packageJsonPath."
  }
  if (!(Test-Path $nodeModulesRoot)) {
    throw "node_modules not found: $nodeModulesRoot. Run npm install in $DemoRoot first."
  }

  $packageJson = Get-Content -LiteralPath $packageJsonPath -Raw | ConvertFrom-Json

  if (!$packageJson.dependencies) {
    return
  }

  $destinationNodeModulesRoot = Join-Path $AppRoot "node_modules"
  New-Item -ItemType Directory -Force -Path $destinationNodeModulesRoot | Out-Null

  $copiedPackages = @{}
  foreach ($dependency in $packageJson.dependencies.PSObject.Properties.Name) {
    Copy-NpmPackageDependency $dependency $nodeModulesRoot $destinationNodeModulesRoot $copiedPackages
  }
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$demoRoot = Split-Path -Parent $scriptDir
$repoRoot = Resolve-FullPath (Join-Path $demoRoot "..\..")

if (!$BuildRoot) {
  $BuildRoot = Join-Path $repoRoot "build"
}
if (!$MsysRoot) {
  $MsysRoot = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { "D:\msys64" }
}
if (!$OutputRoot) {
  $OutputRoot = Join-Path $demoRoot "dist"
}

$electronDist = Join-Path $demoRoot "node_modules\electron\dist"
$packageRoot = Join-Path $OutputRoot "UxPlaySharedTexture-$Platform"
$appRoot = Join-Path $packageRoot "resources\app"
$runtimeRoot = Join-Path $packageRoot "resources\uxplay-runtime"
$runtimePluginRoot = Join-Path $runtimeRoot "lib\gstreamer-1.0"
$runtimeScannerRoot = Join-Path $runtimeRoot "libexec\gstreamer-1.0"

$uxplayExe = Join-Path $BuildRoot "uxplay.exe"
$gstPluginSource = Join-Path $BuildRoot "lib\gstreamer-1.0"
$scannerSource = Join-Path $MsysRoot "mingw64\libexec\gstreamer-1.0\gst-plugin-scanner.exe"

if (!(Test-Path $electronDist)) {
  throw "Electron runtime not found: $electronDist. Run npm install in $demoRoot first."
}
if (!(Test-Path $uxplayExe)) {
  throw "UxPlay executable not found: $uxplayExe. Build UxPlay first."
}
if (!(Test-Path $gstPluginSource)) {
  throw "GStreamer plugin directory not found: $gstPluginSource. Run the DLL/plugin copy step first."
}

Assert-ChildPath $OutputRoot $packageRoot

if (Test-Path $packageRoot) {
  try {
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
  } catch {
    throw "Unable to remove existing package directory: $packageRoot. Close any running UxPlaySharedTexture/Electron process using this folder and try again. $($_.Exception.Message)"
  }
}

Write-Host "Packaging Electron runtime..."
Copy-DirectoryContents $electronDist $packageRoot

$electronExe = Join-Path $packageRoot "electron.exe"
$appExe = Join-Path $packageRoot "UxPlaySharedTexture.exe"
if (Test-Path $appExe) {
  Remove-Item -LiteralPath $appExe -Force
}
Rename-Item -LiteralPath $electronExe -NewName "UxPlaySharedTexture.exe"

Write-Host "Copying Electron app files..."
New-Item -ItemType Directory -Force -Path $appRoot | Out-Null
$appFiles = @(
  "package.json",
  "package-lock.json",
  "main.js",
  "index.html",
  "pin.html",
  "pin-renderer.js",
  "renderer.js",
  "README.md",
  "DEMO_INTRODUCTION.md",
  "WEBSOCKET_PROTOCOL.md",
  "websocket_protocol_advance.md"
)
foreach ($file in $appFiles) {
  $source = Join-Path $demoRoot $file
  if (Test-Path $source) {
    Copy-Item -LiteralPath $source -Destination $appRoot -Force
  }
}

Write-Host "Copying Electron app production dependencies..."
Copy-NpmProductionDependencies $demoRoot $appRoot

Write-Host "Copying UxPlay runtime..."
New-Item -ItemType Directory -Force -Path $runtimeRoot | Out-Null
Copy-Item -LiteralPath $uxplayExe -Destination $runtimeRoot -Force
Get-ChildItem -Path $BuildRoot -Filter "*.dll" -File | ForEach-Object {
  Copy-Item -LiteralPath $_.FullName -Destination $runtimeRoot -Force
}

Write-Host "Copying GStreamer plugins..."
Copy-DirectoryContents $gstPluginSource $runtimePluginRoot

if (Test-Path $scannerSource) {
  Write-Host "Copying GStreamer plugin scanner..."
  New-Item -ItemType Directory -Force -Path $runtimeScannerRoot | Out-Null
  Copy-Item -LiteralPath $scannerSource -Destination $runtimeScannerRoot -Force
} else {
  Write-Warning "GStreamer plugin scanner not found: $scannerSource"
}

$dllCount = (Get-ChildItem -Path $runtimeRoot -Filter "*.dll" -File).Count
$pluginCount = (Get-ChildItem -Path $runtimePluginRoot -Filter "*.dll" -File).Count
$packageSize = "{0:N1} MB" -f ((Get-ChildItem -Path $packageRoot -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1MB)

Write-Host ""
Write-Host "Packaged: $packageRoot"
Write-Host "Executable: $appExe"
Write-Host "UxPlay DLLs: $dllCount"
Write-Host "GStreamer plugins: $pluginCount"
Write-Host "Package size: $packageSize"
