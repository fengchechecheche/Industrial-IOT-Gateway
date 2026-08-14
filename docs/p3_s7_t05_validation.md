# P3-S7-T05 验收报告候选稿

> 工作块：P3-S7-T05——贡献边界、简历素材与阶段收口  
> 检查日期：2026-08-15  
> 冻结输入：`a1fbcdc7661c6fee61c688259ee222872a3cce87`  
> 当前状态：`PASS`  
> 用户审核：全部通过  
> 固化授权：公共安全版本及 CSV 追踪表已授权同步独立仓库

## 1. 验收结论

T05 产物已生成并完成用户逐份审核，格式、追踪关系、数字边界、隐私和权限检查均通过。公共安全版本及 CSV 追踪表已获授权同步到独立仓库；简历候选仍只保留在 Windows 规划目录。未修改正式 CV 或作品集，未执行 Git 写操作、tag、Release 或硬件部署。

最终验收标记为：

```text
PASS_P3_S7_T05_CLAIM_LEDGER_AND_CAREER_HANDOFF_COMPLETE
```

## 2. 冻结输入

| 输入 | 结果 |
|---|---|
| 独立仓库 HEAD | `a1fbcdc7661c6fee61c688259ee222872a3cce87` |
| 工作区状态 | clean |
| G3 正式 run | `20260809T155317Z_g3_b293e40_001`，15/15 PASS |
| G4 正式 run | `20260813T213614Z_soak_release_dd671cf_001`，28800.046 秒，224/224 强制 oracle PASS |
| x86_64 G5 | `20260814T111107Z_systemd_d2bcfde9_001`，10/10 PASS |
| x86_64 bundle | `20260814T112347Z_d2bcfde9_001`，9 文件、双目录归档一致 |
| ARM64 T01 | `20260814T171411Z_arm64_native_e62de0e_003`，Release CTest 191/191 PASS |
| ARM64 T02 | `20260814T181641Z_systemd_124724e0_001`，systemd 10/10 PASS |
| 发布范围 | Linux x86_64 软件 MVP |
| 真实硬件 | `hardware_validated=false`，G6=`NOT_RUN_OPTIONAL` |
| ARM64 长稳/bundle | 均为 false |
| 公开发布 | 未创建 tag，未上传 GitHub Release |

T04 VMware 原始私有证据本轮无法通过当前主机名重新连接读取；该结果只从已经固化、带哈希的 `docs/p3_s7_t04_validation.md` 引用，没有伪造“本轮重新读取 VMware 原始目录”的记录。G3/G4 原始 summary 与 ARM64 两份机器矩阵已在本轮只读复核。

## 3. 候选产物

| 文件 | SHA-256 | 结果 |
|---|---|---|
| `P3-S7-T05_claim_ledger.yaml` | `962f3177ae31ba8b00ed16010aa6fef46d63c3b0fef8631d2eb902f1e63c04b1` | PASS；用户已审核并固化 |
| `P3-S7-T05_claim_source_map.csv` | `097bb858e711701854ec6da36fe2a325b8a661cf61d831b7894918d1f0d72101` | PASS；与已审核台账同步 |
| `P3-S7-T05_resume_claim_candidates.md` | `d8f7ced656e7a6516d782c1d13082d9082ef49c8ca6af265ac55b5767f38eb3b` | PASS；用户已审核，仅保留 Windows 版本 |
| `P3-S7-T05_contribution_boundary.md` | `5b1043b6aa9d0667684123b4c16cb6c4c396b9bed1fdb997db460bc2d8ce0518` | PASS；用户已审核并固化 |
| `p3_s7_t05_工程贡献与证据化表述.md` | `ff22722ebe3183694d308e9cca5089b01ff259620896f6e8592444ab347436d2` | PASS；用户已审核并固化 |
| `P3-S7-T05_validation.md` | 自描述文件，不在自身内容中冻结哈希 | PASS |

## 4. Fact 与 claim 统计

| 项目 | 数量 | 结果 |
|---|---:|---|
| 事实 | 17 | PASS |
| claim | 14 | PASS |
| `public_safe=true` | 10 | PASS |
| `REJECTED_CLAIM` | 4 | PASS |
| 重复 fact ID | 0 | PASS |
| 重复 claim ID | 0 | PASS |
| 无事实来源 claim | 0 | PASS |
| 无证据引用 claim | 0 | PASS |

四条拒绝 claim 分别阻止：真实 RS485/CAN 硬件已验证、工业级/生产可用、ARM64 长稳与 bundle 已完成、GitHub Release 已发布。

## 5. 检查结果

### 5.1 格式与追踪

- WSL PyYAML 6.0.1 成功解析 YAML：17 facts、14 claims；
- Python 标准库成功解析 CSV：14 行，claim ID 唯一；
- YAML 与 CSV 的 claim 顺序、状态和 statement 一致；
- 所有 claim ID 符合 `^P3-C[0-9]{3}$`；
- 所有 claim 的 `source_fact_ids` 均存在，`evidence_refs` 非空；
- 所有公开安全 claim 均具有允许渠道；
- UTF-8 无替换字符，已清除零宽字符。

### 5.2 数字一致性

- G3：正式 summary 状态 PASS，`scenario_count=15`；
- G4：正式 summary 状态 PASS，`long_soak_pass=true`，时长 28800.046 秒，224 个 oracle 全部 enforced 且 passed；
- x86_64 G5/bundle：按已固化 T04 报告引用 10/10、9 文件和双目录一致；
- ARM64 T01：按正式 build matrix/report 引用 Release 191/191 及各质量门；
- ARM64 T02：正式 matrix 状态 PASS，10 个场景全部 PASS，`failures=[]`，SHA256SUMS 有效；
- 未将 CTest、oracle 和 systemd 场景相加为虚构的总通过率。

### 5.3 公开安全与贡献边界

以下内容扫描结果为 0：

- Linux 私有用户目录；
- Windows 绝对路径；
- IPv4 地址；
- SSH 用户/主机组合；
- 凭据或设备序列号；
- UTF-8 损坏字符。

个人实现与 Paho、Mosquitto、yaml-cpp、nlohmann/json、GoogleTest、PTY/termios 和 systemd 均已分开表述。禁止词仅出现在“禁止表述”或被拒绝 claim 中，不作为候选能力声明。

### 5.4 权限与清理

- 未修改 C++、CMake、测试、配置、systemd unit、profile 或原始证据；
- 未修改正式 CV、作品集或投递材料；
- 未同步候选文档到独立仓库；
- 未执行 `git add`、`git commit`、`git push` 或其他 Git 写操作；
- 未创建 tag 或 GitHub Release；
- 未创建持久临时脚本、临时 CSV、`.pyc`、`__pycache__`、补丁候选或构建目录。

## 6. 验收矩阵

| 验收项 | 当前结果 | 说明 |
|---|---|---|
| 输入身份 | PASS | HEAD、正式 run 和平台身份已冻结 |
| 数字一致性 | PASS | G3/G4/ARM64 原始或机器证据复核；x86_64 G5/bundle引用固化报告 |
| Claim 追踪 | PASS | 14/14 有事实与证据 |
| 状态边界 | PASS | 实现、结果、发布、硬件状态分离 |
| 贡献边界 | PASS | 个人、第三方、平台、未完成范围明确 |
| 简历候选 | PASS | 一句话、三条、两条、ARM64 扩展均有 claim ID |
| 公共安全 | PASS | 无私有路径、IP、主机名、凭据或序列号 |
| 文档解析 | PASS | YAML、CSV、Markdown/UTF-8 检查通过 |
| 权限边界 | PASS | 无 Git/发布/CV/独立仓库写操作 |
| 用户审核 | PASS | 六份候选产物全部审核通过 |

## 7. 审核建议

审核进度：

- `P3-S7-T05_resume_claim_candidates.md`：`APPROVED`（2026-08-15）；
- `P3-S7-T05_claim_ledger.yaml` 与派生追踪表：`APPROVED`（2026-08-15）；
- `P3-S7-T05_contribution_boundary.md`：`APPROVED`（2026-08-15）；
- `p3_s7_t05_工程贡献与证据化表述.md`：`APPROVED`（2026-08-15）；
- 本验收报告：`APPROVED`（2026-08-15）。

下一步请优先审核以下内容：

正式维护版本同步路径：

- `docs/p3_s7_t05_claim_ledger.yaml`；
- `docs/p3_s7_t05_claim_source_map.csv`；
- `docs/p3_s7_t05_contribution_boundary.md`；
- `docs/learning/p3_s7_t05_工程贡献与证据化表述.md`；
- `docs/p3_s7_t05_validation.md`。

简历候选不进入独立仓库。本次授权不包含正式 CV、作品集、Git 索引/提交/远程、tag 或 GitHub Release。
