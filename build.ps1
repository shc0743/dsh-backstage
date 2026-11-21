# Build the isolated-desktop launcher with MSVC.
#
# The launcher must live on a path that does NOT carry the Low mandatory
# integrity label: a Low-labeled executable starts a Low-integrity process,
# and such a process cannot create a desktop in WinSta0. Keep this bundle
# outside any Low-labeled directory (the DSH default workspace is Low-labeled).

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $root 'src\winsta-run.c'
$binDir = Join-Path $root 'bin'
$objDir = Join-Path $root 'build'
$output = Join-Path $binDir 'dsh-winsta-run.exe'

if (-not (Test-Path $source)) { throw "source not found: $source" }
New-Item -ItemType Directory -Force -Path $binDir, $objDir | Out-Null

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere not found: $vswhere" }
$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'no Visual Studio C++ toolset found' }
$vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars" }

# Static CRT (/MT) keeps the launcher self-contained; console subsystem keeps
# stderr usable for launcher diagnostics on the inherited pipes.
$object = Join-Path $objDir 'winsta-run.obj'
$command = '"' + $vcvars + '" >nul && cl /nologo /W4 /O2 /utf-8 /MT /DUNICODE /D_UNICODE ' +
    '"' + $source + '" /Fo:"' + $object + '" /Fe:"' + $output + '" user32.lib advapi32.lib'
cmd /c $command
if ($LASTEXITCODE -ne 0) { throw "cl failed with exit code $LASTEXITCODE" }

$hash = (Get-FileHash -Algorithm SHA256 $output).Hash
Write-Host "built  : $output"
Write-Host "sha256 : $hash"
