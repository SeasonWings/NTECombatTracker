# Anomaly NTE combat v1：暴击元数据扩展需求

## 现状证据

- Anomaly v1.7.0 的 `CharacterOnDamaged` 采集路径只发布
  `ANOMALY_NTE_DAMAGE_V1_CHARACTER_EVENT`。
- SDK 文档明确说明该路径不携带客户端展示值、暴击、头部命中或命中位置语义。
- 本项目现场样本共 563 条正伤害事件，全部为 `flags=16`，没有事件设置
  `ANOMALY_NTE_DAMAGE_V1_CRITICAL`。
- `statistics.critical_count` 由同一事件 flags 聚合，因此不能作为独立暴击来源。

## 所需 ABI 能力

首选在伤害事件上增加明确的“暴击元数据有效”语义，使消费者能够区分：

1. 已确认非暴击；
2. 已确认暴击；
3. 当前采集源没有暴击信息。

如果暴击只能从独立的客户端展示事件获得，则该事件必须提供可与角色伤害事件稳定关联的
标识；关联需要覆盖同 tick 多段伤害、延迟展示、合并飘字和无飘字伤害，不能依靠伤害值或
时间邻近猜测。

在上述能力发布前，本项目保留数值列但将暴击质量标记为 `unavailable`，不从倍率、角色面板
或 HP 差值推断暴击。
