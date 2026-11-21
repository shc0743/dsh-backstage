# Runs INSIDE the confined (workspace-write) pwsh on the isolated desktop.
# Reports its desktop via the launcher's probe mode and checks that writes are
# still confined to the granted roots.
$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$launcher = Join-Path $PSScriptRoot '..\bin\dsh-winsta-run.exe'
$sandboxRoot = $env:DSH_BACKSTAGE_SANDBOX_ROOT
if ([string]::IsNullOrWhiteSpace($sandboxRoot)) {
    $sandboxRoot = Join-Path $env:TEMP 'dsh-backstage-sandbox-test'
}

$probe = Join-Path $env:TEMP ('winsta-probe-' + $PID + '.txt')
& $launcher --probe $probe | Out-Null
Write-Output '--- probe (from the confined process) ---'
if (Test-Path $probe) {
    (Get-Content -Raw $probe) -replace "`r", '' | ForEach-Object { $_.Trim() }
} else {
    Write-Output '(probe file missing)'
}
Remove-Item $probe -ErrorAction SilentlyContinue
Write-Output ('DSH_ISOLATED_DESKTOP=[' + $env:DSH_ISOLATED_DESKTOP + ']')
Write-Output ('DSH_ISOLATION_MODE=[' + $env:DSH_ISOLATION_MODE + ']')

Write-Output '--- write confinement ---'
$inside = Join-Path $env:TEMP ('inside-' + $PID + '.txt')
try { Set-Content -LiteralPath $inside -Value 'inside' -Encoding UTF8 -ErrorAction Stop; Write-Output 'private-temp-write=allowed' }
catch { Write-Output ('private-temp-write=DENIED ' + $_.Exception.GetType().Name) }
Remove-Item $inside -ErrorAction SilentlyContinue

$workspaceFile = Join-Path $sandboxRoot 'ws\confined.txt'
try { Set-Content -LiteralPath $workspaceFile -Value 'ws' -Encoding UTF8 -ErrorAction Stop; Write-Output 'workspace-write=allowed' }
catch { Write-Output ('workspace-write=DENIED ' + $_.Exception.GetType().Name) }

$outsideFile = Join-Path $sandboxRoot 'outside.txt'
try { Set-Content -LiteralPath $outsideFile -Value 'nope' -Encoding UTF8 -ErrorAction Stop; Write-Output 'outside-write=ALLOWED (BAD)' }
catch { Write-Output ('outside-write=denied (' + $_.Exception.GetType().Name + ')') }

$systemFile = 'C:\Windows\Temp\dsh-winsta-outside.txt'
try {
    Set-Content -LiteralPath $systemFile -Value 'nope' -Encoding UTF8 -ErrorAction Stop
    Write-Output 'system-write=ALLOWED (BAD)'
    Remove-Item $systemFile -ErrorAction SilentlyContinue
} catch {
    Write-Output ('system-write=denied (' + $_.Exception.GetType().Name + ')')
}

Write-Output 'sandbox-inner-done'
exit 0
