# Backstage — 隔离桌面 Shell（Windows）

## 它做什么

本插件把 DeepSeek Harness 的 PowerShell shell 放到**当前窗口站内的私有桌面**上运行：

- 命令启动的窗口、GUI 程序、自动化测试都出现在这个私有桌面上；
- 它们不在你的屏幕上显示、不抢焦点、收不到你的键鼠输入；
- 命令的 stdout / stderr / 退出码 / stdin / 超时 / 后台作业 / 输出截断行为与原生 shell 一致。

它不改动 DeepSeek Harness 安装目录：内置的 `pwsh-sandbox` 行被本插件的 `backstage` 行替换，全部逻辑通过 profile bundle 提供。

## 四档模式

在 **设置 → 插件 → 本插件 → 行 `backstage`** 的配置页里选择，切换立即生效（volatile 配置，无需重启），并写入 profile 的 Cordis patch 持久保存。

| 选项 | 行为 |
|---|---|
| **始终使用**（默认） | 内置 `pwsh` 工具的每条命令都在隔离桌面运行；不提供 `pwsh_headless`。 |
| **建议使用** | 内置 `pwsh` 保持不变；额外提供 `pwsh_headless`，并在系统提示中要求模型：除非用户明确要求“展示窗口”“显示内容给我”等，始终使用 `pwsh_headless`。 |
| **按需使用** | 内置 `pwsh` 保持不变；额外提供 `pwsh_headless`，并提示模型在自行运行测试、脚本、自动化时使用它，以免影响你使用计算机。 |
| **从不使用** | 完全关闭隔离：内置 shell 行为与原生一致，也不提供 `pwsh_headless`。设置项保留，便于随时切回。 |

`pwsh_headless` 与 `pwsh` 参数完全相同（`command`、`description`、`timeoutMs`、`workdir`、`run_in_background`、沙箱升级参数），区别只在它通过隔离桌面执行。隔离命令的 `$env:DSH_ISOLATED_DESKTOP` 会给出私有桌面名。

## 配置项

| 字段 | 默认 | 说明 |
|---|---|---|
| `mode` | `always` | 四档模式（设置页可见）。 |
| `desktopName` | 空 | 固定私有桌面名；留空时每个 Harness 进程生成 `DSH-<pid>-<随机>`。 |
| `launcherPath` | 空 | 启动器路径覆盖；留空使用包内 `bin/dsh-winsta-run.exe`。 |
| `onUnavailable` | `error` | 私有桌面不可用时让命令失败；设为 `plain` 则退化为不隔离运行并记录警告。 |

其余字段（`timeoutMs`、`maxOutputBytes` 等）与内置 PowerShell 执行器一致，照常生效。

## 工作原理

```
danger-full-access：
  dsh-subprocess runner(Job) → 启动器（宿主令牌）→ 建桌面 → pwsh（继承桌面）
受限沙箱（workspace-write / read-only）：
  dsh-subprocess runner(Job) → sandbox runner（宿主令牌，仍在 Default 桌面）
      → 启动器（WRITE_RESTRICTED + Low，--allow-low）→ 建桌面
      → pwsh（继承受限令牌与私有桌面）
```

- 启动器调用 `CreateDesktopW` 在当前窗口站创建（或打开）私有桌面，再以
  `STARTUPINFO.lpDesktop = "<桌面名>"` 启动目标程序，并把调用方的标准句柄传下去。
- 子进程及其后代都继承这个桌面，所以整棵进程树都被隔离。
- 受限模式下启动器位于沙箱 runner **内层**：runner 仍然运行在你的默认桌面上（它是 Electron 进程，无法在非输入桌面初始化），由它给启动器降权；启动器在受限令牌下创建私有桌面并启动真正的 `pwsh`，后者继承同一令牌与同一桌面。文件沙箱语义完全不变。
- 进程仍由 `ctx.subprocess` 的 Job 持有：`kill`、超时、宿主退出会终止整棵树。
- 桌面对象在最后一个关联进程退出后自动消失；后台 GUI 程序存活时桌面保持，后续命令以同名桌面重连，可继续自动化。

## Windows 输入模型（重要）

一个登录会话只有一条输入流。物理键鼠与 `SendInput` / `keybd_event` / `mouse_event` 合成的输入都进入这条流，只会投递给**当前输入桌面**。因此：

- 隔离桌面收不到合成输入；合成输入还可能落到你正在用的桌面上。
- **不要在隔离 shell 里用 `SendInput` / AutoHotkey / pyautogui 之类的方式驱动窗口。**
- 正确做法（在同一个隔离桌面内运行，完全可用）：
  - UI Automation（`AutomationElement`、Invoke/Value/Text Pattern；WinAppDriver/Appium、pywinauto 的 `uia` 后端）；
  - 向窗口句柄 `PostMessage` / `SendMessage`（`WM_KEYDOWN`、`WM_CHAR`、`WM_LBUTTONDOWN`、`WM_SETTEXT`、`WM_CLOSE` 等；pywinauto 的 `win32` 后端）；
  - 浏览器用 WebDriver / CDP（Playwright、Selenium），完全不依赖合成输入；
  - 应用自带的 CLI、测试钩子、COM 接口。
- 截图请用 `PrintWindow` 或 CDP；`CopyFromScreen` 抓取的是你可见的桌面。
- 本桌面内的 `SetForegroundWindow` 只影响隔离桌面，不会抢你的前台窗口。

## 限制

- **不是安全沙箱**：隔离桌面属于同一用户会话，隔离只解决“不打扰”，文件写入限制仍由 `ctx.sandbox` 负责。
- UWP / 打包应用激活发生在交互桌面，无法纳入隔离。
- 依赖 `GetAsyncKeyState` / Raw Input / DirectInput 的程序收不到 `PostMessage` 按键；提权窗口可能被 UIPI 拦截。
- 只隔离 `pwsh` 与 `pwsh_headless`；终端 / PTY、MCP 服务、subagent 自身进程、Harness 进程本身不隔离。
- 私有桌面与你的 `Default` 桌面共享剪贴板与窗口站资源（这正是兼容性更好的原因）。
- 「从不使用」仍挂载本插件（为了让设置项可切回），执行路径逐字透传，行为等同原生。
- 桌面随最后一个关联进程消失；跨 Harness 重启默认换名，残留后台 GUI 程序可用固定 `desktopName` 重新接上或手动结束。

## 安装、卸载与回滚

- 安装（已发布版本）：`plugin_manager` 的 `install_bundle`，target 填 `dsh-backstage`。
- 安装（本地目录）：target 填本包绝对目录。
- 本插件**替换 `ctx.shell` 服务提供者**。在运行中的 profile 里做这种替换时，一次实时重组合可能报 `service "shell" has been registered`：旧提供者尚未释放，新提供者已在注册。**重启 DeepSeek Harness 后组合是正确的**（本包在 profile 的 bundle 列表末尾，先禁用内置行再挂载本行）。
- 为减少重组合，插件所有配置字段都声明为 volatile：设置页与配置改动会**原地生效，不重挂载插件**。
- 回滚：设置里切到「从不使用」即可停用隔离；彻底移除用 `plugin_manager` 的 `set_bundle`（禁用）或 `remove_bundle`，内置 `pwsh-sandbox` 行会自动恢复。

## 发布（npm / GitHub Actions）

- 包 `dsh-backstage@1.0.0`，Apache-2.0；npm 上该名字可用（发布前已确认）。
- `.github/workflows/publish.yml` 在 `windows-latest` 上：`./build.ps1` 构建启动器 → `node --check` → `--list/--diag` 冒烟（非阻断）→ `npm pack` 上传 tarball 制品 → 推送 `v*` 标签时 `npm publish --access public`。
- 使用 **npm trusted publishing（OIDC）**：npm 包里已把 trusted publisher 配成 GitHub 仓库 `shc0743/dsh-backstage` + 工作流文件 `publish.yml`，工作流声明 `id-token: write`，发布时不需要任何 token secret。不要再配 `NODE_AUTH_TOKEN`，否则会走 token 认证而不是可信发布交换。
- `scripts/prepack.mjs` 在打包/发布前检查 `bin/dsh-winsta-run.exe` 是否存在，避免发布缺少启动器的包；本机与 CI 都必须先构建。
- 发布出的 tarball 只包含 CI 构建的启动器；不要把本地构建的 exe 提交进 Git（`.gitignore` 已忽略）。

## 重建启动器

启动器源码 `src/winsta-run.c`，需要 Visual Studio 的 C++ 工具集：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

产物 `bin/dsh-winsta-run.exe`（静态 CRT，控制台子系统，自包含）。

自检：

```powershell
.\bin\dsh-winsta-run.exe --diag      # INTEGRITY_RID / STATION / DESKTOP_OK / WIN32 / INPUT_DESKTOP
.\bin\dsh-winsta-run.exe --selftest  # 建桌面、起探针、验证窗口只在该桌面、Default 上不可见
```

**启动器必须位于不带 Low 完整性标签的目录。** 从 Low 标签位置启动会让进程降为 Low 完整性，隔离命令也会随之降权；启动器会检测并拒绝运行，预检也会给出提示。不要把本包放进 DSH 的 `default-workspace`（该目录带 Low 标签）。

## 实测证据

环境：Windows 11 10.0.26300，用户 `DESKTOP-L6CK423\shc07`，非提权；DSH 0.2.2-rc.5（Desktop profile）。

启动器：

- `bin\dsh-winsta-run.exe --diag` → `INTEGRITY_RID=8192`、`STATION=WinSta0`、`DESKTOP_OK=1`、`WIN32=0`。
- `--selftest` 连续运行 4 次全部 `RESULT: PASS`：探针进程运行在 `DSH-SELFTEST-<pid>` 桌面、能在该桌面建窗、窗口只在隔离桌面可枚举、`Default` 上不存在、退出码正确。
- 手工链路（启动器 → PowerShell）：stdout/stderr 透传、`[exit code: 7]`、Unicode（中文/emoji）、带引号/空参数/结尾反斜杠/中文路径的参数逐字往返、cwd 与 `$env:DSH_*` 继承。
- 把 exe 复制到带 `Low Mandatory Level` 的工作区后 `--diag` 显示 `INTEGRITY_RID=4096`，运行模式按设计拒绝执行；插件预检会把它转成可执行的整改提示（把插件包移出 Low 目录）。

DSH 内（`always` 模式）：

- `pwsh` 工具：`$env:DSH_ISOLATION_MODE=always`、`$env:DSH_ISOLATED_DESKTOP=DSH-<pid>-<rand>`；stdout/stderr/`[exit code: 7]`、Unicode、cwd、`$env:DSH_*`、大输出截断与 spill 文件、后台作业与 `job_output` 均正常。
- 可见性证明（`scripts/verify.ps1`）：在 shell 内启动 `winver.exe`，枚举隔离桌面（窗口存在，含「关于 Windows」）与用户 `Default`（0 个窗口）→ `VERDICT=PASS`。
- `suggest` / `on-demand`：系统提示出现对应文案；内置 `pwsh` 不带隔离环境变量（跑在默认桌面）；`pwsh_headless` 可用且隔离（同样 `VERDICT=PASS`），后台作业正常。
- `never`：无提示、无 `pwsh_headless`、内置 `pwsh` 不带隔离变量。
- 模式切换通过 profile 配置的 volatile 更新即时生效，无需重启。
- 设置项：改名前的客户端槽 occupant（`@local/dsh-winsta-shell#pwsh-isolated`）在实时会话中验证为 active；改名后模块 id、行 id 与槽 key 已同步为 `dsh-backstage` / `backstage` / `dsh-backstage#backstage`，浏览器需刷新设置页以加载新的客户端模块。
- 失败路径：把 `launcherPath` 指向不存在的文件后，命令以
  `DSH_WINSTA_UNAVAILABLE: 隔离桌面不可用（Error: spawnSync … ENOENT）…` 失败，并给出整改建议。

发布包（`dsh-backstage@1.0.0`）：

- npm 上 `dsh-backstage` 未占用（发布前查询 404）；peer 依赖（`@deepseek-ai/dsh-*` 0.2.2-rc.5 起可用）均已发布到 npm。
- `pnpm pack` 通过 `prepack` 检查并产出 `dsh-backstage-1.0.0.tgz`，内容含 `bin/dsh-winsta-run.exe`、`.github/workflows/publish.yml`、`LICENSE`、`docs/DETAIL.md`、`scripts/*`、`src/winsta-run.c`。
- 发布 zip（`dsh-backstage-1.0.0.zip`）**不含**本地构建的二进制与 `build/`：启动器由 `.github/workflows/publish.yml` 在 `windows-latest` 上编译后才打包发布。

受限沙箱组合（`scripts/sandbox-proof.ps1`，standalone workspace-write）：

- 私有桌面内探针报告 `STATION=WinSta0`、`DESKTOP=DSH-SANDBOX`、`WINDOW_OK=1`。
- 写入：私有 temp 允许、工作区允许、`D:\…\dsh-sandbox-test\outside.txt` 拒绝、`C:\Windows\Temp\…` 拒绝（均为 `UnauthorizedAccessException`）。
- 整条链退出码 0，说明“隔离桌面 + 文件沙箱”可以叠加，且沙箱未被绕过。

已知未在本次验证中覆盖：重启后的组合（本会话不重启应用；组合逻辑与已验证的实时挂载一致）、改名后设置卡片需刷新页面加载（模块 id/行 id/槽 key 已核对一致；本会话没有浏览器控制能力，请在刷新后到 **设置 → 插件 → Backstage → 行 `backstage`** 确认四选一控件）、UWP 应用、提权窗口的 UIA。
