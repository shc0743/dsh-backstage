/**
 * Settings card for the Backstage bundle.
 *
 * The Plugins page renders this component for the bundle's `backstage` row
 * and supplies the row's Host-owned configuration form. The card edits the
 * single `mode` field; every choice is submitted immediately, so there is no
 * save button. Modes are volatile on the Host side, so a change takes effect
 * without restarting the harness.
 */

window.__ModuleLoader__.load({
  id: 'dsh-backstage',
  factory(require) {
    const React = require('react')
    const NS = 'backstage'
    const ROW_KEY = 'dsh-backstage#backstage'
    const MODES = ['always', 'suggest', 'on-demand', 'never']

    const DICT = {
      zh: {
        title: 'Backstage 隔离桌面',
        summary: '在隔离桌面中运行 shell 命令，不打扰你使用计算机。',
        intro: '选择内置 shell 的隔离方式。切换立即生效，无需重启。',
        'mode.always': '始终使用',
        'mode.always.hint': '内置 shell 的所有命令都在私有桌面运行：窗口对你不可见、不抢焦点。',
        'mode.suggest': '建议使用',
        'mode.suggest.hint': '内置 shell 不变，另外提供 pwsh_headless 工具，并在提示中要求模型优先使用它。',
        'mode.on-demand': '按需使用',
        'mode.on-demand.hint': '内置 shell 不变，另外提供 pwsh_headless 工具，提示模型只在自行测试/自动化时使用。',
        'mode.never': '从不使用',
        'mode.never.hint': '完全关闭隔离：行为与原生 shell 相同，也不提供 pwsh_headless。',
        descriptor: '模式',
        unavailable: '当前不可写入：设置由更高优先级的配置层控制。',
      },
      en: {
        title: 'Backstage isolated desktop',
        summary: 'Runs shell commands on an isolated desktop without disturbing your computer.',
        intro: 'Choose how the built-in shell is isolated. Changes apply immediately; no restart is needed.',
        'mode.always': 'Always',
        'mode.always.hint': 'Every built-in shell command runs on a private desktop: windows stay invisible and never take focus.',
        'mode.suggest': 'Suggest',
        'mode.suggest.hint': 'The built-in shell stays unchanged; a pwsh_headless tool is added and the prompt tells the model to prefer it.',
        'mode.on-demand': 'On demand',
        'mode.on-demand.hint': 'The built-in shell stays unchanged; pwsh_headless is added for the model to use for its own tests and automation.',
        'mode.never': 'Never',
        'mode.never.hint': 'Isolation is off: behavior matches the native shell and no pwsh_headless tool is offered.',
        descriptor: 'Mode',
        unavailable: 'Not writable: a higher-priority configuration layer owns this setting.',
      },
    }

    function lookup(locale, key) {
      return (DICT[locale] && DICT[locale][key]) || DICT.en[key] || key
    }

    function currentMode(form) {
      const value = form && form.state && form.state.value
      const mode = value && typeof value === 'object' ? value.mode : undefined
      return MODES.indexOf(mode) >= 0 ? mode : 'always'
    }

    function ConfigCard(props) {
      const translate = props.t || ((key) => lookup('zh', key))
      const form = props.form
      if (props.view === 'summary') {
        const mode = currentMode(form)
        return translate('mode.' + mode)
      }
      if (!form) {
        return React.createElement('p', { style: { opacity: 0.75, fontSize: 12 } }, translate('unavailable'))
      }
      const state = form.state || {}
      const mode = currentMode(form)
      const writable = state.writable === true && state.revision !== undefined
      return React.createElement(
        'div',
        { style: { display: 'flex', flexDirection: 'column', gap: 8 } },
        React.createElement('p', { style: { margin: 0, opacity: 0.75, fontSize: 12 } }, translate('intro')),
        MODES.map((candidate) => React.createElement(
          'label',
          { key: candidate, style: { display: 'flex', gap: 8, alignItems: 'flex-start', cursor: writable ? 'pointer' : 'default' } },
          React.createElement('input', {
            type: 'radio',
            name: 'dsh-winsta-mode',
            checked: mode === candidate,
            disabled: !writable,
            style: { marginTop: 3 },
            onChange: () => {
              if (!writable || mode === candidate) return
              void form.mutate(
                [{ op: 'set', path: ['mode'], value: candidate }],
                state.revision,
              )
            },
          }),
          React.createElement(
            'span',
            null,
            React.createElement('span', { style: { fontWeight: 600 } }, translate('mode.' + candidate)),
            React.createElement('span', { style: { display: 'block', opacity: 0.75, fontSize: 12 } },
              translate('mode.' + candidate + '.hint')),
          ),
        )),
        writable ? null : React.createElement(
          'p',
          { style: { margin: 0, opacity: 0.75, fontSize: 12 } },
          translate('unavailable'),
        ),
      )
    }

    return {
      inject: ['slots', 'locale'],
      apply(ctx) {
        ctx.effect(() => ctx.locale.register(NS, {
          zh: DICT.zh,
          en: DICT.en,
        }), 'dsh-winsta-shell: locale')
        ctx.effect(() => ctx.slots.inject('plugins.row.config', () => ctx.slots.register({
          name: 'plugins.row.config',
          key: ROW_KEY,
          locale: NS,
          inject: () => ({}),
        }, ConfigCard)), 'dsh-winsta-shell: settings card')
      },
    }
  },
})
