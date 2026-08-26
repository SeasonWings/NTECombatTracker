# NTE Combat Stats

Anomaly v1.7.0 的只读游戏内战斗统计插件。它只消费 `anomaly.nte.combat`，显示当前玩家队伍轮换期间的输出、承伤、遭遇 DPS、10 秒 DPS、伤害来源、参与者和最近事件。

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

## 风险

Anomaly 会加载到游戏进程。即使本插件只读，使用第三方工具仍可能违反游戏规则或带来崩溃、数据泄露和封号风险，请仅在自有测试环境中使用。
