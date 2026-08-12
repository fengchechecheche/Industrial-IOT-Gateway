# P3-S5-T01 验收报告候选稿

> 工作块：P3-S5-T01——执行主要软件故障矩阵  
> 验证日期：2026-08-12  
> 仓库：`/home/iot-gw/projects/industrial_iot_gateway`  
> 当前状态：`AWAITING_FORMAL_RUN`，不得写为最终 PASS

## 1. 当前结论

预提交实现与探索性验证已通过，但正式 S5 故障矩阵尚未执行。

最终探索性运行 `20260812T132741Z_g3_uncommi_001` 为 F01–F15 15/15 PASS、未关闭失败 0，
68 项 SHA256 校验通过，manifest 正确写入 `gate=G3`、`stage=S5`、`task=P3-S5-T01`。
由于其 `source_revision=uncommitted` 且 `exploratory=true`，它只能证明当前本地内容具备进入
正式运行的条件，不能作为绑定提交的 S5 验收证据。

## 2. 合同与历史基线复核

- `software.json` 包含且仅包含 F01–F15，场景 ID 唯一；
- F06、F10、F11、F13 要求非负恢复指标；
- runner 会继续执行失败场景之后的其余场景；
- 超时回收整个进程组；
- 退出码保持 0/2/3/4/5 合同；
- 当前不存在 `release.json`，本工作块继续使用已审核 `software` profile。

历史基线 `artifacts/baseline/20260809T155317Z_g3_b293e40_001/` 的只读复核结果：

| 项目 | 结果 |
|---|---|
| 提交 | `b293e402fe3b45e60de3c8f09512378c959f4bbd` |
| 正式标志 | `exploratory=false` |
| 场景 | 15/15 PASS |
| 未关闭失败 | 0 |
| 恢复指标 | F06=20 ms、F10=1426 ms、F11=20 ms、F13=30 ms |
| 事件 | 972 条，ID 无重复且引用可追踪 |
| SHA256 | 68/68 一致 |

## 3. 测试先行与修复记录

### 3.1 S5 manifest 所属阶段错误

审计发现 `EvidenceWriter` 把 `stage=S4`、`task=P3-S4-T03` 写死。先加入
`test_records_requested_stage_and_task_in_manifest`，首次运行因构造函数不接受 `stage` 参数而
按预期失败；随后对 CLI、`run_scenarios()` 和 `EvidenceWriter` 做最小参数化，runner 单元测试
8/8 PASS。

### 3.2 Paho publish token 生命周期竞态

TSan 首轮为 175/176。F10 的 GoogleTest 断言全部通过，但 TSan 报告 Paho 接收线程正在
`token::on_success()` 广播条件变量时，MQTT worker 已析构同一个 delivery token。

修复采用固定两代 token 保活：当前 token 与上一代 token 均为 `MqttPublishSink::Impl` 成员，
且声明顺序保证 `async_client` 先析构、token 后析构。该结构固定占用两个指针，不会形成无界缓存。
修复后 F10 在 TSan 下连续三次定向 PASS，全量 TSan 176/176 PASS。

## 4. 最终预提交质量门

| 质量门 | 结果 |
|---|---|
| Debug，MQTT ON、PTY、warnings-as-errors | PASS，176/176 |
| ASan/UBSan，MQTT ON、PTY | PASS，176/176 |
| Release，MQTT ON、PTY | PASS，176/176 |
| clang-tidy 18，Clang 18、全目标 | PASS，构建完成 |
| TSan，MQTT ON、PTY、WSL2 workaround | PASS，176/176 |
| clang-format 18 | PASS |
| Python `py_compile` | PASS |
| `software.json` 解析 | PASS |

首次 Debug 预检误用了不存在的 `GATEWAY_WARNINGS_AS_ERRORS` 变量，CMake 明确报告未使用；随后
使用项目真实选项 `GATEWAY_ENABLE_WARNINGS_AS_ERRORS=ON` 完整复验，不能把首次配置当作该门证据。

## 5. 最终探索性矩阵

| 项目 | 结果 |
|---|---|
| run_id | `20260812T132741Z_g3_uncommi_001` |
| source_revision | `uncommitted` |
| exploratory | `true` |
| stage/task | `S5` / `P3-S5-T01` |
| F01–F15 | 15/15 PASS |
| 未关闭失败 | 0 |
| 恢复指标 | F06=20 ms、F10=1426 ms、F11=20 ms、F13=30 ms |
| 事件 | 975 条 |
| SHA256 | 68/68 一致 |

探索性目录位于 `/tmp/industrial_iot_gateway_s5_t01_exploratory/`，完成候选记录后必须删除，
不得复制到正式证据目录。

## 6. 正式运行待办

必须先形成一个包含以下变更、且能代表被测内容的提交 ID：

- runner 的 `--stage` / `--task` 参数及 manifest 传递；
- 元数据合同测试；
- MQTT publish token 两代有界保活；
- S5-T01 教程、运行说明和本候选报告。

获得并确认提交 ID 后，重新执行 Debug 构建，并运行：

```bash
python3 tools/run_fault_matrix.py \
  --profile software \
  --build-dir /tmp/industrial_iot_gateway_s5_t01_debug \
  --output-root artifacts/fault_matrix/s5 \
  --source-revision <CONFIRMED_COMMIT_ID> \
  --stage S5 \
  --task P3-S5-T01
```

只有同时满足 `exploratory=false`、15/15 PASS、未关闭 P0/P1 为 0、SHA256 和追踪关系通过，
才能将本报告改为最终 `PASS`。正式结果产生前，P3-S5-T02 不应记为 `NOT_TRIGGERED`。

## 7. 结论边界

当前结果不证明 8 小时长稳、真实 RS485、STM32、CAN、传感器、生产 TLS/认证或跨主机网络。
本任务没有检查 Git 工作区状态，也没有执行 `git add`、`git commit` 或 `git push`。
