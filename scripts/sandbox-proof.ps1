# Prove that isolation and the Windows ACL sandbox compose.
#
# The sandbox runner starts this launcher with the restricted (low) token; the
# launcher creates the private desktop at that integrity and starts the real
# pwsh there, which inherits the same token. This script builds that exact
# chain through the sandbox runner's standalone mode.
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$launcher = Join-Path $PSScriptRoot '..\bin\dsh-winsta-run.exe'
$inner = Join-Path $PSScriptRoot 'sandbox-inner.ps1'
$harness = $env:DSH_NODE_EXECUTABLE
$runner = Join-Path (Split-Path -Parent (Split-Path -Parent $env:DSH_INSTALL_DIR)) '@deepseek-ai\dsh-sandbox-windows-acl\lib\runner.js'

$root = Join-Path $env:TEMP 'dsh-backstage-sandbox-test'
$workspace = Join-Path $root 'ws'
$tempRoot = Join-Path $root 'tmp'
Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $workspace, $tempRoot | Out-Null

@($launcher, $inner, $harness) | ForEach-Object {
    if (-not (Test-Path $_)) { throw "missing test input: $_" }
}
# The runner lives inside the packaged app.asar; Test-Path cannot inspect it,
# so its path is passed through to the launcher directly.

$arguments =
    '"' + $runner + '" ' +
    '--workspace "' + $workspace + '" --temp "' + $tempRoot + '" --mode workspace-write -- ' +
    '"' + $launcher + '" --allow-low --desktop "DSH-SANDBOX" -- ' +
    '"C:\WINDOWS\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "' + $inner + '"'

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $harness
$psi.Arguments = $arguments
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
$psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8
$psi.EnvironmentVariables['DSH_ISOLATED_DESKTOP'] = 'DSH-SANDBOX'
$psi.EnvironmentVariables['DSH_ISOLATION_MODE'] = 'workspace-write'
$psi.EnvironmentVariables['DSH_BACKSTAGE_SANDBOX_ROOT'] = $root

$process = [System.Diagnostics.Process]::Start($psi)
$stdoutTask = $process.StandardOutput.ReadToEndAsync()
$stderrTask = $process.StandardError.ReadToEndAsync()
$exited = $process.WaitForExit(90000)
if (-not $exited) {
    $process.Kill()
    Write-Output '--- TIMEOUT (chain did not settle) ---'
} else {
    Write-Output $stdoutTask.Result
    $stderr = $stderrTask.Result
    if ($stderr.Trim().Length -gt 0) {
        Write-Output '--- stderr ---'
        Write-Output $stderr
    }
    Write-Output ('--- chain exit code: ' + $process.ExitCode + ' ---')
}

Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
