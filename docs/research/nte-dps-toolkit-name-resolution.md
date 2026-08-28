# `nte-dps-toolkit` 抓包名称解析链路

> 调研日期：2026-08-28（Asia/Shanghai）  
> 调研对象：[`kongbaiz/nte-dps-toolkit`](https://github.com/kongbaiz/nte-dps-toolkit)  
> 代码基线：[`bff9f569ae19fc659b61a3a810407c01be5fc71a`](https://github.com/kongbaiz/nte-dps-toolkit/tree/bff9f569ae19fc659b61a3a810407c01be5fc71a)  
> 来源范围：仅使用该仓库的 README、源码、随包资源和提交历史。

## 结论

该项目不是把抓包中的数字 ID 直接交给 FNamePool，也不是在线翻译。其名称链路是：

```text
角色：包内 4 位角色 ID
  → characters.json
  → name_zh

技能：包内 GameplayEffect UniqueIndex
  → gameplay_effect_mapping.json
  → GE_ 技术名
  → skill_damage.json 的 GAName
  → ability_tips.json 的 name_zh
  → 可选的 GameplayEffect 组件语义中文名
```

因此，其中文名称来自**预先导出并随程序分发的游戏 DataTable/本地化资源**，而不是抓包负载本身携带的中文，也不是运行时内存名称解析。

## 1. 抓包提供的稳定标识

### 角色 ID

解析器会尝试全部 0～7 位 bit shift，在负载中寻找形如 `05 00 00 00 + 四位 ASCII 数字 + 00` 的角色声明，只接受 `1000..=9999`。[来源：角色声明解析](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/src/engine/parser.rs#L3273-L3304)

同一数据包只有一个角色 ID 时，该 ID 成为 `packet_char_id`；否则解码器会回退到同一 UDP 四元组上一次确认的会话角色。[来源：包角色与会话回退](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/src/engine/capture.rs#L6274-L6292)

### GameplayEffect UniqueIndex

解析器从多种 Active/Compact GameplayEffect 线格式中读取一个小端 `u32 unique_index`，过滤 `0`、`u32::MAX` 和重复值，并保留其字节位置与 bit shift。[来源：GameplayEffect 解析](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/src/engine/parser.rs#L3154-L3255)

这里的 `UniqueIndex` 是查静态映射表的键；代码没有把它当作 FName 索引。

## 2. 角色 ID 如何变成中文名

`load_characters` 读取 `res/data/characters/characters.json`，把 JSON 的字符串键解析为 `u32`，形成 `HashMap<u32, CharacterInfo>`。[来源：角色表加载](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/src/engine/parser.rs#L566-L575)

伤害记录确定 `char_id` 后，用 `characters.get(&char_id)` 查表：优先 `name_zh`，中文为空时使用 `name_en`；查不到时显示 `未知角色` 或 `未知角色(id)`。[来源：命中角色命名](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/src/engine/parser.rs#L3393-L3426)

角色资源本身就是显式 ID 字典。例如 `1036` 的 `name_zh` 是“残虹”。[来源：characters.json 示例](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/res/data/characters/characters.json#L83-L90)

展示层还会按照当前 UI 语言从 `CharacterInfo` 重新选择 `name_zh` 或 `name_en`，资源名缺失时保留命中记录中的 fallback。[来源：Main DPS 角色本地化](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/src-tauri/src/contract/main_dps.rs#L443-L456)

## 3. 技能 ID 如何变成中文名

技能转换分三次 join，而不是一步完成。

### 3.1 `UniqueIndex → GE_ 技术名`

`load_gameplay_effect_mapping` 把 `gameplay_effect_mapping.json` 转成 `HashMap<u32, String>`。它同时兼容精简的 `effects` 对象，以及游戏原始 DataTable 的 `Rows[*].UniqueIndex` 格式。[来源：GE 映射加载](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/src/engine/parser.rs#L921-L958)

匹配命中与 GameplayEffect 后，`apply_gameplay_effect` 用 `unique_index` 查出 GE 技术名，写入命中的 `gameplay_effect_index` 和 `gameplay_effect_name`。[来源：命中 GE 富化](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/src/engine/capture.rs#L5902-L5927)

### 3.2 `GE_ 技术名 → GA_ 技能名`

`AbilityCatalog` 读取 `skill_damage.json`。原始 DataTable 格式中，以 GE 行名为键，并从该行的 `GAName` 取得所属 GameplayAbility 名称。[来源：技能结构表加载](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/src/engine/parser.rs#L1124-L1199)

### 3.3 `GA_ 技能名 → 本地化显示名`

`ability_tips.json` 以 GA 名为键保存 `name_zh`、`name_en` 和 `name_ja`。简中模式的字段优先级是 `name_zh → name_en → name_ja`；读取时还会移除游戏富文本标签。[来源：技能说明本地化加载](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/src/engine/parser.rs#L1201-L1252)

`resolve_damage_name` 先执行 `GE → GA → 本地化技能名`。若 `gameplay_effect_semantics.json` 为这个 GE 定义了更细的伤害组件名，则根据 `show_parent_ability` 输出“技能名 · 组件名”或仅输出组件名。[来源：运行时名称解析](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/src/storage/ability_names.rs#L229-L265)

控制台技能页面的实际显示优先级是：

1. GE 对应的本地化伤害名；
2. GA 对应的本地化技能名；
3. 已有 `damage_name`；
4. 已有普通标签或技术名。

[来源：技能 Contract 回退链](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/src-tauri/src/contract/skills.rs#L118-L143)

### 完整示例

仓库中可以完整追踪这一条：

```text
包内 index 957
→ GE_Player_Sagiri_UltraSkill1_Damage
→ GA_Sagiri_UltraSkill
→ 千钧隆重的饕餮宴
```

- `957 → GE_Player_Sagiri_UltraSkill1_Damage`：[GameplayEffect 映射](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/res/data/skills/gameplay_effect_mapping.json#L6700-L6706)
- `GE → GA_Sagiri_UltraSkill`：[技能伤害表](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/res/data/skills/skill_damage.json#L6111-L6118)
- `GA → 千钧隆重的饕餮宴`：[技能说明表](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/res/data/skills/ability_tips.json#L5511-L5514)

## 4. 中文资源来自哪里

当前仓库的 `asset_manifest.json` 记录了资源输入及 SHA-256：

- 角色：`DT_Character.json`；
- GE index 映射：`DT_GameplayEffectMappingData.json`；
- GE 到 GA：`DT_SkillDamageData.json`；
- GA 技能显示名：`DT_GameplayAbilityTipsData.json`；
- 简中本地化：`Localization/Game/zh-CN/game.json`。

[来源：资源 Manifest 输入](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/res/data/asset_manifest.json#L1-L25) · [简中本地化输入与输出](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/res/data/asset_manifest.json#L67-L87)

公开 README 明确说明：“资源导出与后处理工具链不随公开仓库发布，只同步必要且可分发的资源”。因此，**当前公开仓库不能从游戏包完整复现这些 JSON 的生成过程**。[来源：README 贡献边界](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/README.md#L297-L304)

正常 Cargo 构建只收集并嵌入仓库已有的 `res/`；它不会在构建时读取游戏进程、FNamePool 或重新导出 DataTable。[来源：build.rs 入口](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/build.rs#L29-L68) · [资源收集](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/build.rs#L79-L116)

提交历史中曾公开过 `tools/nte_asset_pipeline.py`，后于 [`0a0d4bd`](https://github.com/kongbaiz/nte-dps-toolkit/commit/0a0d4bd2e9aab68e71d5018fd359297113899648) 删除。删除前的脚本说明 exporter 阶段与后处理分离，并展示了角色 `ItemName.SourceString`、技能 `Name.SourceString` 等字段如何生成 `name_zh`。[来源：历史角色生成代码](https://github.com/kongbaiz/nte-dps-toolkit/blob/e11304dabbe484a148b43b5560e12d8591d1e1e8/tools/nte_asset_pipeline.py#L191-L255) · [历史技能名生成代码](https://github.com/kongbaiz/nte-dps-toolkit/blob/e11304dabbe484a148b43b5560e12d8591d1e1e8/tools/nte_asset_pipeline.py#L279-L314) · [历史资源构建入口](https://github.com/kongbaiz/nte-dps-toolkit/blob/e11304dabbe484a148b43b5560e12d8591d1e1e8/tools/nte_asset_pipeline.py#L459-L526)

`gameplay_effect_semantics.json` 是额外的校正/细分层。代码与提交历史能够证明它被独立维护和多次调整，但公开资料没有明确声明它“完全由人工生成”，因此不能把这一点当作确定事实。明确可见的人工修补包括 `repair_stale_zankou_names`：它会用当前国服技能名替换部分过时名称。[来源：残虹名称修补](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/src/storage/ability_names.rs#L101-L146)

## 5. 未知值如何回退

- 未知角色：解码阶段使用 `未知角色` 或 `未知角色(id)`；其他投影在资源名和命中 fallback 都不可用时可退到 `#id`。[来源：命中角色回退](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/src/engine/parser.rs#L3411-L3426) · [时间轴角色回退](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/src/core/timeline.rs#L277-L294)
- 未知 GE index：`apply_gameplay_effect` 查不到映射时直接返回，不伪造名称；包诊断只显示数字 index 和位置。[来源：未知 GE 处理](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/src/engine/capture.rs#L5902-L5913) · [诊断显示](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/src/engine/capture.rs#L6749-L6767)
- 未知技能：详情页依次尝试 GE/GA 本地化名、非技术 `damage_name`、普通稳定标签；全部失败时显示 `Unmapped Skill`。[来源：详情技能回退](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/src-tauri/src/contract/main_dps_detail.rs#L880-L941)
- 当前语言缺名：`ability_tips` 会跨语言回退，因此简中 UI 中的新技能可能显示英文或日文，而不是空白。[来源：语言字段优先级](https://github.com/kongbaiz/nte-dps-toolkit/blob/bff9f569ae19fc659b61a3a810407c01be5fc71a/src/engine/parser.rs#L1201-L1252)

## 6. 对 `NTECombatTools` 的直接启示

`nte-dps-toolkit` 证明了一条可行但有版本维护成本的路线：从已授权导出的官方资源构建本地版本化字典，运行时只用稳定技术标识做 join。

它不能直接证明 Anomaly 的 `source_id` 等于抓包中的 GameplayEffect `UniqueIndex`。当前 `NTECombatTools` 更适合继续以 Anomaly 已提供的 `participant_path_utf8` 和 `source_name_utf8` 技术路径作为身份键；若要复用对方思路，应建立“技术路径/GE 名 → 官方导出表 → 中文显示名”的离线生成链，而不是把 `source_id` 强行套入对方的数字映射表。

此外，对方的中文数据并非运行时自动发现：游戏版本更新后仍需要重新导出、核对映射覆盖率，并保留未知值和数据版本提示。
