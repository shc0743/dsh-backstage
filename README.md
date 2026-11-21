# Backstage

`dsh-backstage` is a DeepSeek Harness bundle for **Windows** that runs shell
commands on a private desktop. Windows created by the agent's commands, its GUI
applications and its automated tests never appear on the user's screen, never
take focus and receive no input. A four-way mode setting decides when this
happens, and two of the modes add a separate `pwsh_headless` tool.

It never edits the DeepSeek Harness installation. The shipped `pwsh-sandbox`
executor row is disabled and this bundle's `backstage` row provides `ctx.shell`.

## Modes

| Value | Settings label | Behavior |
|---|---|---|
| `always` (default) | 始终使用 / Always | Every built-in `pwsh` command runs on the private desktop. No `pwsh_headless` tool. |
| `suggest` | 建议使用 / Suggest | The built-in `pwsh` is unchanged; `pwsh_headless` is offered and the prompt asks the model to always use it unless the user asks to see a window or content on their screen. |
| `on-demand` | 按需使用 / On demand | Same tool, prompt asks the model to use it for its own tests and automation. |
| `never` | 从不使用 / Never | Isolation is off: the built-in shell behaves exactly as before and `pwsh_headless` is hidden. |

The setting lives on the Plugins settings page under this bundle's `backstage`
row. It is a volatile configuration field, so a change applies without
restarting the harness and persists through the profile's Cordis patch.

## Configuration

| Field | Default | Meaning |
|---|---|---|
| `mode` | `always` | `always` / `suggest` / `on-demand` / `never` (shown in Settings). |
| `desktopName` | empty | Fixed private desktop name. Empty generates `DSH-<pid>-<random>` once per harness process. |
| `launcherPath` | empty | Launcher executable override. Empty uses `bin/dsh-winsta-run.exe`. |
| `onUnavailable` | `error` | Fail the command when the isolated desktop cannot be created, or (`plain`) run it unisolated and log a warning. |

The shipped executor's own settings (`timeoutMs`, `maxOutputBytes`, …) keep
working unchanged.

## Install

The published tarball contains the launcher built by CI. Install it with the
Plugin Manager:

```text
plugin_manager  install_bundle  target: dsh-backstage
```

For a local checkout, pass its absolute directory instead. The bundle must live
on a path **without a Low mandatory integrity label**; a Low launcher would run
every command at Low integrity, and the executor refuses with an actionable
error when that happens.

After the first installation, the profile may need one DeepSeek Harness restart
to compose the replaced `shell` provider cleanly (the live load can report a
service-handover error while the old provider is still registered).

## Build and verify

The launcher is C source compiled with MSVC:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
.\bin\dsh-winsta-run.exe --diag      # INTEGRITY_RID / STATION / DESKTOP_OK / WIN32 / INPUT_DESKTOP
.\bin\dsh-winsta-run.exe --selftest  # creates a desktop, proves the probe window lives only there
```

`scripts/verify.ps1` proves visibility isolation from inside the harness shell;
`scripts/sandbox-proof.ps1` proves that a confined workspace-write command runs
on the isolated desktop while file writes stay confined.

## Publishing

`.github/workflows/publish.yml` builds the launcher on `windows-latest`, checks
the JavaScript, packs the tarball and publishes it. Pushing a `v*` tag publishes
to npm; `workflow_dispatch` only builds and uploads the tarball artifact.

Publishing uses **npm trusted publishing (OIDC)**: the npm package is configured
with the GitHub repository `shc0743/dsh-backstage` and the workflow file
`publish.yml`, and the workflow grants `id-token: write`. No token secret is
needed. Do not add `NODE_AUTH_TOKEN`; npm would then authenticate with a token
instead of the trusted-publisher exchange.

The published package must never contain a locally built launcher; CI builds it
right before `npm pack`/`npm publish`, and `scripts/prepack.mjs` refuses to pack
when the binary is missing.

## Important boundaries

- **Not a security boundary.** The isolated desktop belongs to the same user
  session. File write confinement and process isolation remain the job of
  `ctx.sandbox`; this bundle only keeps the agent's windows away from the user.
- **Synthesized input cannot reach the isolated desktop.** `SendInput`,
  `keybd_event` and `mouse_event` enter the session input stream and are
  delivered to the *input* desktop only. Drive isolated windows with UI
  Automation, `PostMessage`/`SendMessage`, WebDriver/CDP or the application's
  own test hooks — not with input synthesis.
- **Screenshots:** `CopyFromScreen` captures the user's visible desktop. Use
  `PrintWindow` or a CDP screenshot from inside the isolated shell.
- UWP/packaged apps activate on the interactive desktop and cannot be
  isolated here.
- Only the `pwsh` shell and `pwsh_headless` are isolated; terminals, MCP
  servers, subagents and the harness process itself are not.

See [docs/DETAIL.md](docs/DETAIL.md) for the implementation, the verified
evidence, the Windows input-desktop model and the rebuild instructions.

## License

Apache-2.0. See [LICENSE](LICENSE).
