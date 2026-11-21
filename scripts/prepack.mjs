/**
 * npm `prepack` guard: never publish a package without the Windows launcher.
 *
 * The launcher is built by `build.ps1` (MSVC). GitHub Actions runs that build
 * before `npm pack`/`npm publish`; a local publish must run it too. Failing
 * here is deliberate: an installed bundle without the binary would fail every
 * isolated command with a preflight error.
 */
import { existsSync, statSync } from 'node:fs'
import { fileURLToPath } from 'node:url'

const launcher = fileURLToPath(new URL('../bin/dsh-winsta-run.exe', import.meta.url))

if (!existsSync(launcher)) {
  console.error('dsh-backstage: bin/dsh-winsta-run.exe is missing.')
  console.error('Build the launcher before packing or publishing:')
  console.error('  powershell -NoProfile -ExecutionPolicy Bypass -File ./build.ps1')
  console.error('The GitHub Actions publish workflow builds it automatically.')
  process.exit(1)
}

if (statSync(launcher).size === 0) {
  console.error('dsh-backstage: bin/dsh-winsta-run.exe is empty; rebuild it with build.ps1.')
  process.exit(1)
}
