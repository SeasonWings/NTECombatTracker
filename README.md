# NTE Combat Stats

Anomaly v1.7.0 的只读游戏内战斗统计与本地日志插件。它消费 `anomaly.nte.combat`，显示当前玩家队伍轮换期间的输出、承伤、遭遇 DPS、10 秒 DPS、伤害来源、参与者和最近事件，并将遭遇内宿主可见的完整伤害事件保存为可重放 JSON Lines。

## 构建

环境要求：Windows x64、Visual Studio 2022 C++ Build Tools、CMake 3.22+，以及 Anomaly v1.7.0 SDK。

```powershell
cmake --preset default -DAnomalySDK_DIR=C:/AnomalySDK/lib/cmake/AnomalySDK
cmake --build --preset release
ctest --preset release
```

插件包生成在 `build/package/NteCombatStats/`，目录结构固定为：

```text
NteCombatStats/
├── locales/
│   └── zh-CN.json
├── manifest.json
└── plugin.dll
```

部署前可执行：

```powershell
anomaly-plugin validate .\build\package\NteCombatStats
anomaly-test-host --plugin .\build\package\NteCombatStats --reload 10 --ticks 60
```

将包目录复制到游戏 Runtime 的 `Anomaly/plugins/NteCombatStats/` 后启用插件。

## 统计语义

- 第一条当前受控角色相关事件开启遭遇战，10 秒无相关伤害后结束。
- 同一 World 内切换角色不会重置；切换 World 会清空。
- 只有正 `final_damage` 进入伤害总量和 DPS；零值与负值保留在事件日志。
- 数据来自客户端 `CharacterOnDamaged` 广播，不是服务端权威战斗日志。
- 插件严格只读，不申请内存读写、技能调用或网络能力。

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
