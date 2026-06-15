param(
  [string]$Msys2Root
)

$ErrorActionPreference = "Stop"

function Test-Msys2Root([string]$RootPath) {
  if ([string]::IsNullOrWhiteSpace($RootPath)) {
    return $false
  }

  $shellCmd = Join-Path $RootPath "msys2_shell.cmd"
  $mingwGdb = Join-Path $RootPath "mingw64\bin\gdb.exe"
  $ucrtGdb = Join-Path $RootPath "ucrt64\bin\gdb.exe"
  return (Test-Path $shellCmd) -and ((Test-Path $mingwGdb) -or (Test-Path $ucrtGdb))
}

function Resolve-Msys2Root {
  param([string]$InputRoot)

  if (Test-Msys2Root $InputRoot) {
    return (Resolve-Path $InputRoot).Path
  }

  if (Test-Msys2Root $env:MSYS2_ROOT) {
    return (Resolve-Path $env:MSYS2_ROOT).Path
  }

  $candidates = @(
    "C:\msys64",
    "D:\msys64",
    "E:\msys64",
    "C:\tools\msys64",
    "D:\tools\msys64"
  )

  foreach ($candidate in $candidates) {
    if (Test-Msys2Root $candidate) {
      return (Resolve-Path $candidate).Path
    }
  }

  $manual = Read-Host "MSYS2 not found automatically. Enter MSYS2 root (example: C:\msys64)"
  if (Test-Msys2Root $manual) {
    return (Resolve-Path $manual).Path
  }

  throw "Invalid MSYS2 root: '$manual'. Expected msys2_shell.cmd and mingw64\bin\gdb.exe or ucrt64\bin\gdb.exe under this folder."
}

$resolvedRoot = Resolve-Msys2Root -InputRoot $Msys2Root
[Environment]::SetEnvironmentVariable("MSYS2_ROOT", $resolvedRoot, "User")
$env:MSYS2_ROOT = $resolvedRoot

Write-Host "MSYS2_ROOT configured successfully."
Write-Host "  MSYS2_ROOT=$resolvedRoot"
Write-Host "If VS Code was already open, run 'Developer: Reload Window' to refresh debug/task environment."
