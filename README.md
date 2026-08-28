# NTE Combat Stats

Anomaly v1.7.0 的只读游戏内战斗统计、本地日志和纯 Win32 Overlay 插件。它消费 `anomaly.nte.combat`，显示当前玩家队伍轮换期间的输出、承伤、遭遇 DPS、10 秒 DPS、伤害来源、参与者和最近事件，并将遭遇内宿主可见的完整伤害事件保存为可重放 JSON Lines。

## 构建

环境要求：Windows x64、Visual Studio 2022 C++ Build Tools、CMake 3.22+，以及 Anomaly v1.7.0 SDK。

统一使用打包脚本完成 Release 配置、构建、测试、插件校验和归档：

```powershell
.\script\package.ps1 `
  -AnomalySdkDir C:\AnomalySDK\lib\cmake\AnomalySDK `
  -AnomalyPluginTool C:\AnomalyTools\anomaly-plugin.exe
```

也可以通过 `ANOMALY_SDK_DIR` 和 `ANOMALY_PLUGIN_TOOL` 环境变量提供路径。只有明确无法使用校验工具时才传入 `-SkipValidation`。

脚本默认清理并重建仓库根目录下的 `build/`，避免不同 CMake 生成器或旧包文件相互污染；确认现有缓存兼容时可传入 `-Incremental`。

全部配置、编译和打包暂存文件位于 `build/`；可校验插件目录为 `build/package/NteCombatStats/`。唯一最终交付物输出到 `dist/NteCombatStats-<version>-Release.zip`。ZIP 内不额外嵌套目录，结构固定为：

```text
├── locales/
│   └── zh-CN.json
├── NteCombatOverlay.exe
├── THIRD_PARTY_NOTICES.txt
├── manifest.json
└── plugin.dll
```

脚本默认已经执行 `ctest` 和插件包校验。需要单独复查暂存包时可执行：

```powershell
anomaly-plugin validate .\build\package\NteCombatStats
anomaly-test-host --plugin .\build\package\NteCombatStats --reload 10 --ticks 60
```

将包目录复制到游戏 Runtime 的 `Anomaly/plugins/NteCombatStats/` 后启用插件。

## 统计语义

- 第一条当前受控角色相关事件开启遭遇战，10 秒无相关伤害后结束。
- 同一 World 内切换角色不会重置；切换 World 会清空。只有实际作为攻击者或受击者出现的受控角色才计为参战角色，同一路径的 generation 变化会合并。
- 只有正 `final_damage` 进入伤害总量和 DPS；零值与负值保留在事件日志。
- 数据来自客户端 `CharacterOnDamaged` 广播，不是服务端权威战斗日志。
- 插件严格只读，不申请内存读写、技能调用或外部网络能力。本地 Overlay 只连接 Runtime 的 `127.0.0.1` WebSocket 广播。

## Win32 Overlay

插件从自身包目录自动启动 `NteCombatOverlay.exe`。Overlay 默认 560×400、透明置顶，采用紧凑深灰表格、稳定角色配色和伤害比例条，支持输出/承伤页签、角色→技能来源→逐次命中的同窗钻取，以及只读历史战斗查看。标题栏可手动结束当前统计；结束后实时页清空，结果保存到历史。

- 输出首页固定显示角色、总伤害、DPS、DPS%、暴击%和最高伤害来源；暴击%只统计最终伤害大于 0 的输出事件。

- 点击列头切换排序；拖动顶栏移动，四边与四角均可缩放，底部页签切换输出和承伤。
- 标题栏“历史”可浏览已关闭 Encounter；详情显示客户端接收时间、最终/基础/显示伤害和命中标记。
- `Ctrl+Shift+F12` 切换锁定。锁定后窗口不激活并允许鼠标穿透。
- Overlay 不会强制显示游戏当前隐藏的光标；游戏进程或插件停止时会立即退出。
- 托盘菜单可显示、隐藏、解锁和退出。
- 配置保存在 `%LOCALAPPDATA%\NTECombatStats\overlay-settings.json`。
- Overlay 只接收自有 `NteOverlaySnapshot`，不使用 ACT `CombatData`、OverlayPlugin、CEF 或 WebView2。

## 本地日志

玩家首次造成或承受伤害时创建遭遇；遭遇开始后保存同一 World 中宿主发布的所有可见伤害事件，但只有玩家相关事件会延长 10 秒脱战计时。日志使用客户端接收时间，并非服务器权威时间。

日志默认不限量保存在：

```text
%LOCALAPPDATA%\NTECombatStats\logs\v1\
├── catalog.json
└── reports\<report-id>\
    ├── report.json
    ├── actors.jsonl
    ├── abilities.jsonl
    ├── encounters.jsonl
    ├── events-000001.jsonl
    └── index.json
```

每条原始事件保存 sequence、tick sequence、完整 damage flags、显示/基础/最终伤害、命中位置、伤害与反应类型、参与者和来源。64 位标识及伤害值以十进制字符串编码。游戏内“历史”页可查看基础汇总并经二次确认删除关闭的 Report。

`PARTIAL`、`OVERFLOW`、`CURSOR_GAP`、聚合截断、写入队列丢失和异常关闭都会使日志被标记为不完整。原始事件适合由后续工具重算伤害指标，但不包含治疗、Buff、施法、服务器权威时间、Boss 难度或账号身份。

## 风险

Anomaly 会加载到游戏进程。即使本插件只读，使用第三方工具仍可能违反游戏规则或带来崩溃、数据泄露和封号风险，请仅在自有测试环境中使用。
