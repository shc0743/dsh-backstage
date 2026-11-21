/**
 * Backstage — isolated-desktop patch for the shipped PowerShell executor.
 *
 * The shipped `pwsh-sandbox` executor is NOT replaced. Replacing the `shell`
 * service cannot take over from an already-running provider, so enabling such
 * a bundle live fails with `service "shell" has been registered`. Instead this
 * plugin mounts after the shipped executor and patches its prototype methods
 * in place (`resolve`, `argv`, `confine`); disabling the plugin restores them.
 *
 * Every command that should be isolated is started through
 * `bin/dsh-winsta-run.exe`, which creates (or opens) a private desktop in the
 * caller's window station and starts the real program there with
 * `STARTUPINFO.lpDesktop`. The child inherits that desktop, so all of its
 * windows, tests and automation stay invisible to the user and never take
 * focus or input.
 *
 * In a confined sandbox mode the launcher is placed *inside* the sandbox
 * runner: the runner spawns this launcher with the restricted token, the
 * launcher creates the isolated desktop at that integrity and starts the real
 * `pwsh` there, which inherits the same token. Isolation and confinement
 * compose without running the Electron-based sandy runner on a non-input
 * desktop.
 *
 * See docs/DETAIL.md.
 */

import { AsyncLocalStorage } from 'node:async_hooks'
import { spawnSync } from 'node:child_process'
import { randomBytes } from 'node:crypto'
import { fileURLToPath } from 'node:url'

import z from '@deepseek-ai/schemastery'
import { ESCALATION_TARGETS, sandboxPermissionsDescription } from '@deepseek-ai/dsh-sandbox'
import { defineTool, ToolArgsError, ToolContextUnavailableError } from '@deepseek-ai/dsh-tools'
import { parseExitStatus } from '@deepseek-ai/dsh-shell'

/** The bundled launcher, resolved beside this module. */
const DEFAULT_LAUNCHER = fileURLToPath(new URL('./bin/dsh-winsta-run.exe', import.meta.url))

/** Marker embedded in a pwsh_headless command so the executor can see it. */
const MARKER = `DSH-HEADLESS-${randomBytes(8).toString('hex')}`

/** Cross-check: true while a pwsh_headless call is being dispatched. */
const headlessScope = new AsyncLocalStorage()

/** The four settings-page modes. */
export const MODES = ['always', 'suggest', 'on-demand', 'never']

/** Prompt hint for the suggest mode (the user's rule, kept verbatim in intent). */
const SUGGEST_HINT =
  'You should always use the `pwsh_headless` tool to run commands: it runs on an isolated desktop '
  + 'and cannot disturb the user. Use the normal `pwsh` tool only when the user explicitly asks to '
  + 'show a window or to see the content on their own screen (for example 「展示窗口」「显示内容给我」).'

/** Prompt hint for the on-demand mode. */
const ON_DEMAND_HINT =
  'When you run tests, scripts, or automation yourself — especially anything that may open windows '
  + 'or take focus — use the `pwsh_headless` tool so the user\'s computer stays undisturbed. Use the '
  + 'normal `pwsh` tool when the user wants to see the result on screen.'

/** Tool description shared by both headless-capable modes. */
const HEADLESS_DESCRIPTION =
  'Execute a PowerShell command (`pwsh -Command`) on an isolated desktop and return its stdout/stderr. '
  + 'The command and every process it starts run on a private desktop of the agent: windows are not '
  + 'shown to the user, never take focus, and receive no user input. Parameters are identical to the '
  + '`pwsh` tool; prefer this tool for your own tests and automation, and use `pwsh` only when the user '
  + 'asks to see something on their screen. '
  + 'Each call runs in a fresh pwsh process; pass `workdir` instead of using `cd`. Paths use native '
  + 'Windows form (`C:\\...`); read environment variables with `$env:NAME`. '
  + '`$env:DSH_ISOLATED_DESKTOP` names the private desktop. '
  + 'Long output is truncated to its tail; the full output is saved to a file whose path is reported '
  + 'when available.'

/**
 * Backstage configuration. Every field is volatile so a change is committed
 * into the running plugin instead of remounting it.
 */
export const Config = z.object({
  /** Settings-page mode: always / suggest / on-demand / never. */
  mode: z.union(MODES).default('always').volatile(),
  /** Fixed desktop name; empty means one fresh name per harness process. */
  desktopName: z.string().volatile(),
  /** Launcher executable override; empty means the bundled binary. */
  launcherPath: z.string().volatile(),
  /** What to do when isolation is not available: fail the command, or run it unisolated. */
  onUnavailable: z.union(['error', 'plain']).default('error').volatile(),
})

/** Required services: the executor to patch and the sandbox seam it uses. */
export const inject = ['shell', 'sandbox']

/** Parse KEY=VALUE lines from the launcher's diagnostic output. */
function parseFacts(text) {
  const facts = {}
  for (const line of String(text ?? '').split(/\r?\n/u)) {
    const index = line.indexOf('=')
    if (index > 0) facts[line.slice(0, index).trim()] = line.slice(index + 1)
  }
  return facts
}

/** Render content blocks produced by the delegated pwsh definition. */
function blocksToText(blocks) {
  return (Array.isArray(blocks) ? blocks : [])
    .filter(block => block !== null && typeof block === 'object' && block.type === 'text')
    .map(block => String(block.text ?? ''))
    .join('')
}

/**
 * Mount the runtime patch over the shipped PowerShell executor.
 * @param ctx - the plugin context.
 * @param config - the Backstage configuration.
 */
export function apply(ctx, config) {
  if (process.platform !== 'win32') return
  const shell = ctx.shell
  if (shell === undefined || shell === null
    || typeof shell.resolve !== 'function'
    || typeof shell.argv !== 'function'
    || typeof shell.confine !== 'function') {
    ctx.logger?.warn?.('dsh-backstage: the mounted shell executor does not expose the expected pwsh seam; skipping the isolation patch')
    return
  }

  // Patch the executor prototype in place; the class owns exactly one
  // instance, and the own/inherited distinction decides how to restore it.
  const proto = Object.getPrototypeOf(shell)
  const savedResolve = proto.resolve
  const savedArgv = proto.argv
  const savedConfine = proto.confine
  const hadResolve = Object.prototype.hasOwnProperty.call(proto, 'resolve')
  const hadArgv = Object.prototype.hasOwnProperty.call(proto, 'argv')
  const hadConfine = Object.prototype.hasOwnProperty.call(proto, 'confine')

  const generatedDesktop = `DSH-${process.pid}-${randomBytes(3).toString('hex')}`
  const currentMode = () => config.mode.get()
  const desktopName = () => (config.desktopName.get() ?? '').trim() || generatedDesktop
  const launcherExecutable = () => (config.launcherPath.get() ?? '').trim() || DEFAULT_LAUNCHER
  const unavailablePolicy = () => config.onUnavailable.get()

  /** Whether this command should run on the isolated desktop. */
  const activeFor = (command) => {
    const mode = currentMode()
    if (mode === 'always') return true
    if (mode === 'never') return false
    return String(command ?? '').includes(MARKER)
  }

  /** Cached preflight verdict together with the launcher it probed. */
  let preflight

  /** Run the launcher preflight once per launcher path. */
  const isolationUsable = () => {
    const launcher = launcherExecutable()
    if (preflight !== undefined && preflight.launcher === launcher) return preflight.ok
    let result
    try {
      result = spawnSync(launcher, ['--diag'], { encoding: 'utf8', windowsHide: true, timeout: 30_000 })
    } catch (error) {
      preflight = { launcher, ok: false, detail: String(error) }
      return false
    }
    const facts = parseFacts(result.stdout)
    const rid = Number(facts.INTEGRITY_RID)
    const hasRid = Number.isFinite(rid) && rid > 0
    let detail
    if (result.error !== undefined && result.error !== null) {
      detail = String(result.error)
    } else if (hasRid && rid < 8192) {
      detail = `启动器以低完整性运行（Integrity RID=${rid}），命令会被降权；请把插件包放到不带 Low 完整性标签的目录`
    } else if (facts.DESKTOP_OK !== '1') {
      detail = `无法创建私有桌面（win32=${facts.WIN32 ?? 'unknown'}）`
    } else if (result.status !== 0) {
      detail = `启动器自检退出码 ${String(result.status)}`
    }
    const ok = detail === undefined
    preflight = { launcher, ok, detail }
    if (!ok && unavailablePolicy() === 'plain') {
      ctx.logger?.warn?.(`dsh-backstage: 隔离桌面不可用，命令将以不隔离方式运行：${detail}`)
    }
    return ok
  }

  /** Throw the actionable isolation-unavailable error. */
  const assertIsolation = () => {
    const detail = preflight?.detail ?? 'unknown'
    throw new Error(
      'DSH_WINSTA_UNAVAILABLE: 隔离桌面不可用（' + detail + '）。'
      + '请把插件包移到不带 Low 完整性标签的目录（不要放在 DSH 的 default-workspace 下），'
      + '或在设置里把模式切换为「从不使用」。',
    )
  }

  /** Resolve isolation for one command, honoring the unavailable policy. */
  const ensureIsolation = () => {
    if (isolationUsable()) return true
    if (unavailablePolicy() === 'plain') return false
    assertIsolation()
    return false
  }

  proto.resolve = function resolve(request) {
    const spec = savedResolve.call(this, request)
    const mode = currentMode()
    const expected = headlessScope.getStore() === true
    if (expected && mode !== 'always' && mode !== 'never'
      && !String(request.command ?? '').includes(MARKER)) {
      throw new Error(
        'DSH_WINSTA_MARKER_LOST: pwsh_headless 未能把隔离标记传给执行器，为避免命令静默跑在用户桌面上，已中止。',
      )
    }
    if (!activeFor(request.command)) return spec
    if (!ensureIsolation()) return spec
    return {
      ...spec,
      env: {
        ...spec.env,
        DSH_ISOLATED_DESKTOP: desktopName(),
        DSH_ISOLATION_MODE: mode,
      },
    }
  }

  proto.argv = function argv(spec) {
    const plain = savedArgv.call(this, spec)
    if (!activeFor(spec.command)) return plain
    if (!ensureIsolation()) return plain
    return [launcherExecutable(), '--desktop', desktopName(), '--', ...plain]
  }

  proto.confine = async function confine(spec, policy, signal) {
    if (!activeFor(spec.command)) return savedConfine.call(this, spec, policy, signal)
    if (!ensureIsolation()) return savedConfine.call(this, spec, policy, signal)
    const wrapped = [
      launcherExecutable(),
      '--allow-low',
      '--desktop',
      desktopName(),
      '--',
      ...savedArgv.call(this, spec),
    ]
    return ctx.sandbox.confine(wrapped, policy, signal)
  }

  // Restore the executor when the plugin unloads or is disabled.
  ctx.effect(() => () => {
    if (hadResolve) proto.resolve = savedResolve
    else delete proto.resolve
    if (hadArgv) proto.argv = savedArgv
    else delete proto.argv
    if (hadConfine) proto.confine = savedConfine
    else delete proto.confine
  }, 'dsh-backstage: executor patch')

  /** The optional model-facing tool for the suggest and on-demand modes. */
  const buildHeadlessTool = () => defineTool({
    name: 'pwsh_headless',
    description: HEADLESS_DESCRIPTION,
    parameters: {
      description: {
        type: 'string',
        required: true,
        description: 'Clear, concise description of what this command does in active voice, '
          + '5-10 words (shown in the UI). Examples: "Run unit tests" → "Run unit tests".',
      },
      command: { type: 'string', required: true, description: 'The PowerShell command to execute.' },
      timeoutMs: {
        type: 'number',
        description: 'Timeout in milliseconds. The executor applies its configured default and cap; '
          + 'on expiry the command moves to the background as a job instead of being killed.',
      },
      workdir: {
        type: 'string',
        description: 'Working directory for this command. Defaults to the session workspace; '
          + 'a relative path is resolved against it.',
      },
      run_in_background: {
        type: 'boolean',
        description: 'Run in the background and return a job id immediately (collect with job_output, '
          + 'stop with job_kill). No timeout applies.',
      },
      sandbox_permissions: {
        type: 'string',
        enum: [...ESCALATION_TARGETS],
        description: sandboxPermissionsDescription('command'),
      },
      justification: {
        type: 'string',
        description: 'Required with sandbox_permissions: one sentence for the user explaining why '
          + 'this exact command needs the wider access. Use the language of the user\'s current request.',
      },
    },
    output: {
      schema: { type: 'string' },
      render: (_args, value) => [{ type: 'text', text: String(value) }],
    },
    presentCall: (args) => {
      if (args.run_in_background === true) {
        return {
          card: 'generic',
          title: args.command,
          kind: 'execute',
          rawInput: args.command,
          content: [{ type: 'text', text: args.description }],
        }
      }
      return {
        card: 'terminal',
        title: args.command,
        description: args.description,
        ...args.workdir !== undefined ? { cwd: args.workdir } : {},
      }
    },
    presentResult: (args, result) => {
      const block = result.content.length === 1 ? result.content[0] : undefined
      if (block === undefined || block.type !== 'text') return undefined
      const raw = block.text
      const isBackground = typeof args === 'object' && args !== null && args.run_in_background === true
      const isPromoted = result?.value?.kind === 'promoted'
      if (isBackground || isPromoted || result.isError) {
        return { card: 'generic', content: [{ type: 'text', text: '```console\n' + raw.replace(/\n+$/u, '') + '\n```' }] }
      }
      const { body, ...exit } = parseExitStatus(raw)
      return { card: 'terminal', output: body, ...exit }
    },
    async execute(args, exec) {
      if (typeof args.command !== 'string' || args.command.trim().length === 0) {
        throw new ToolArgsError(['invalid command: expected a non-empty string'])
      }
      const toolsCtx = toolsContext
      if (toolsCtx === undefined) {
        throw new ToolContextUnavailableError('pwsh_headless is unloading: the tools service is unavailable')
      }
      const inner = toolsCtx.tools.executable('pwsh', exec.agent, exec.toolSet)
      if (inner === undefined) {
        throw new ToolContextUnavailableError(
          'pwsh_headless requires the pwsh tool in this composition; it is not available to this agent',
        )
      }
      const marked = { ...args, command: `$null = '${MARKER}'; ` + args.command }
      const value = await headlessScope.run(true, () => inner.execute(marked, exec))
      return blocksToText(inner.output.render(marked, value))
    },
  })

  /** Injected tools context while the tools service is present. */
  let toolsContext
  /** Disposer of the currently registered pwsh_headless tool. */
  let headlessDispose

  /** Register or unregister the headless tool for the current mode. */
  const syncHeadlessTool = () => {
    const toolsCtx = toolsContext
    if (toolsCtx === undefined) return
    const mode = currentMode()
    const visible = mode === 'suggest' || mode === 'on-demand'
    if (visible && headlessDispose === undefined) {
      headlessDispose = toolsCtx.tools.register(buildHeadlessTool(), {
        when: () => {
          const current = currentMode()
          return current === 'suggest' || current === 'on-demand'
        },
      })
    } else if (!visible && headlessDispose !== undefined) {
      headlessDispose()
      headlessDispose = undefined
    }
  }

  ctx.inject(['tools'], (toolsCtx) => {
    toolsContext = toolsCtx
    toolsCtx.effect(() => {
      syncHeadlessTool()
      const off = ctx.on('loader/volatile-update', () => { syncHeadlessTool() })
      return () => {
        off()
        if (headlessDispose !== undefined) {
          headlessDispose()
          headlessDispose = undefined
        }
        toolsContext = undefined
      }
    }, 'dsh-backstage: pwsh_headless tool')
  })

  ctx.inject(['systemPrompt'], (promptCtx) => {
    promptCtx.effect(() => promptCtx.systemPrompt.section({
      name: 'tool:pwsh_headless',
      order: promptCtx.systemPrompt.getSectionOrder('TOOL_PWSH') + 1,
      text: () => {
        const mode = currentMode()
        if (mode === 'suggest') return SUGGEST_HINT
        if (mode === 'on-demand') return ON_DEMAND_HINT
        return ''
      },
    }))
  })
}
