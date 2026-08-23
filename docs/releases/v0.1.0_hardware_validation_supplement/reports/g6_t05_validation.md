# P3-S7-G6-T05：证据、claim 与发布边界固化验收

> 文档状态：`APPROVED / PASS`
>
> 当前工程判定：`PASS`
>
> 前置状态：`T00～T04=PASS`
>
> 编制日期：2026-08-23

## 1. 工作块目标

T05 不修改网关代码、不重复运行硬件场景，也不重打 v0.1.0 包。它负责把 T00～T04 的事实组织成
可追踪证据矩阵，追加新的硬件事实和允许表述，并冻结“发布后硬件补充验证”与不可变 Release 的
边界。

## 2. 正式产物

| 正式来源文件 | 作用 |
|---|---|
| `docs/p3_s7_g6_t00_evidence_reuse.md` | 关闭项目五复用和 T00 限制 |
| `docs/p3_s7_g6_t01_validation.md` | 固化两只 TAS 单机、写入和持久化 |
| `docs/p3_s7_g6_evidence_matrix.md` | 串联 T00～T05 的判据与证据 |
| `docs/p3_s7_t05_claim_ledger.yaml` | 追加硬件事实和 claim，保留历史事实 |
| `docs/p3_s7_t05_claim_source_map.csv` | claim 到事实/证据的扁平追踪表 |
| `docs/learning/p3_s7_g6_t05_验收证据与声明边界.md` | 面向初学者的同步教程 |
| `docs/learning/README.md` | 把 G6-T05 教程加入学习路线并标记 G6 状态 |
| 本文档 | T05 验收、限制与后续移交 |

## 3. Claim 迁移规则

### 3.1 历史事实不删除

原 `P3-F014` 记录“真实 USB-RS485、商用从站、STM32 与 CAN 尚未执行”。该事实在当时成立，不能
删除或改写成从未存在。正式 ledger 将其标为历史限制，并通过新的 `P3-F021～P3-F024` 说明：

- 两只 TAS 单机和配置已通过；
- 三真实从站 RS485/MQTT 集成已通过；
- 真实故障隔离和 systemd 已通过；
- 真实三从站八小时长稳已通过；
- CAN 仍不属于项目三 G6 验收。

### 3.2 新增允许 claim

本次新增：

```text
P3-C017：真实三从站 RS485/MQTT 集成
P3-C018：17 场景真实故障隔离与 systemd 恢复
P3-C019：3600 秒预跑与 28800 秒真实硬件长稳
```

它们可以作为后续简历、面试和作品集的输入，但本阶段不直接修改正式 CV 或作品集。

### 3.3 继续拒绝的表述

- “项目三 RS485 和 CAN 全链路均已通过”；
- “工业级”“生产可用”“现场高可靠”；
- “传感器计量精度已认证”；
- “长距离/EMC/浪涌已验证”；
- “v0.1.0 已公开发布”或“原 Release 已改成硬件验证版”。

## 4. Release 边界

v0.1.0 的五文件本地双架构 Release 已经固定，不能因后续 G6 PASS 而修改其 manifest 或 index：

```text
release_status=LOCAL_RELEASE_READY
published=false
tag=null
hardware_validated=false
```

G6 的正确表述是：

```text
v0.1.0 发布后的真实硬件补充验证
```

若以后需要把硬件验证状态写进可发布资产，应单独规划新版本、变更日志、候选提交和双架构重建，
不能覆盖旧资产。

## 5. 机器检查合同

正式产物必须满足：

- YAML 可解析；fact/claim ID 唯一；新增 source fact 均存在；
- CSV 表头保持 10 列，行数与 claim 数一致，claim ID 无重复；
- YAML 与 CSV 的 statement、status、source facts、渠道和 review status 一致；
- Markdown 中关键提交、run ID、时长、成功率和最大间隔与正式报告一致；
- 不包含用户名、IP、设备唯一序列号、凭据、私钥或 Windows 私人绝对路径；
- 不出现把 CAN、计量精度或工业现场写成已验证的表述；
- `git diff --check` 通过；
- 不执行 `git add`、`git commit` 或 `git push`。

## 6. 正式结论

候选文件已经用户审核通过，正式维护版本已同步到独立仓库并再次通过一致性检查。T05 与 G6
正式关闭标记为：

```text
PASS_P3_S7_G6_T05_EVIDENCE_AND_CLAIMS_FINALIZED
PASS_P3_S7_G6_REAL_HARDWARE_SUPPLEMENTAL_VALIDATION
G6=PASS
```

## 7. 后续移交

G6 关闭后分成两个独立后继工作，不在本工作块自动执行：

1. 规划新的“硬件验证补充版本”，决定是否发布新版本；
2. 基于 `P3-C017～P3-C019` 单独规划 CV/作品集更新，并逐条选择表述。
