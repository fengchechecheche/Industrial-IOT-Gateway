# P3-S5-T03 验收报告

> 验收日期：2026-08-12  
> 工作块：冻结长稳 profile、门限与执行工具  
> 结论：PASS（T03 工具链与 90 秒 smoke）；T04 预跑和 S6 8 小时长稳均未执行

## 1. 验收结论

T03 已实现 release/preflight/smoke profile、schema/语义 validator、共享 PTY 仿真库、C++ soak
driver、Python runner、资源采样、日志轮转、确定性摘要和机器失败退出码。最终 90 秒 smoke
覆盖静默、650 ms 延迟、坏 CRC、截断帧、异常码 0x02、broker 停止和 PTY 断开七类故障，
逐项触发、分类和恢复 oracle 通过。

`long_soak_pass` 在 smoke 中固定为 false。当前结论不包含 3600 秒 preflight、28800 秒 release、
真实 RS485/STM32、树莓派部署或硬件稳定性。

## 2. 交付物

- `config/soak/software_release.json`、`software_preflight.json`；
- `tests/data/soak_profiles/software_smoke.json` 和非法重叠样本；
- `schemas/soak_profile.schema.json`、`soak_evidence.schema.json`；
- `tools/run_soak.py`、`tools/summarize_soak.py`、`tools/soak/`；
- `tools/soak/gateway_soak_driver.cpp`；
- 公共 `PtyBusHarness` 头文件与 `gateway_simulation` 库；
- soak profile、runner、summary 单元测试和 CTest smoke；
- `docs/soak_protocol.md` 与中文教程。

## 3. 关键缺陷与修复

### 3.1 延迟晚帧错误关闭健康串口

首轮 smoke 注入 650 ms 延迟后，晚帧会进入下一请求上下文。解析器正确拒绝不匹配帧，但运行时
把协议层不匹配标成必须关闭串口，随后 stable PTY 路径重开失败，三个从站停止推进。修复后，
只有真实 peer/read I/O 故障设置 `requires_serial_reopen`；晚帧仅被丢弃。新增
`GatewayRuntimePtyTest.DiscardsLateResponseWithoutClosingHealthySerialPort` 覆盖该路径。

### 3.2 冒烟恢复间距不足

原 60 秒加速日程给 bad CRC 后恢复只留 4 秒，无法稳定容纳真实退避。smoke 改为 90 秒并扩大
故障间距；正式 preflight/release 的 3600 秒日程和冻结门限未放宽。

### 3.3 多循环判定与日志边界

审计发现初版摘要只核对每类故障的最后一个窗口。现已改为对 release 的每个 cycle 分别检查
触发、预期分类与恢复；新增缺少第二循环必须 FAIL 的测试。累计计数必须单调，RSS 使用 5 分钟
中位数桶计算趋势。broker、subscriber、driver、gateway、PTY 和 MQTT 日志均使用有界分段
writer，摘要再次核对单段大小。

## 4. 自动化结果

| 质量门 | 结果 |
|---|---|
| Debug + warnings-as-errors + 完整 CTest | PASS，181/181 |
| ASan/UBSan + 完整 CTest | PASS，181/181 |
| Release + 完整 CTest | PASS，181/181 |
| clang-tidy 18 全目标 | 构建完成；修复 T03 driver 顶层异常逃逸诊断 |
| TSan + WSL2 workaround + 完整 CTest | PASS，181/181 |
| clang-format 18 | PASS，全项目 `--dry-run --Werror` |
| Python `py_compile` / JSON 解析 | PASS |
| soak Python 单元测试 | PASS，10/10 |
| 最终 soak CTest 子集 | PASS，4/4，含 90 秒端到端 smoke |

clang-tidy 仍会显示此前代码中的若干非 T03 基线提示，例如部分测试对 optional 的访问和配置
解码函数的异常逃逸提示；本次新增 driver 提示已清理，构建退出码为 0。这些既有提示未被描述为
“全仓零警告”。

## 5. 验收条件映射

| 条件 | 证据 | 状态 |
|---|---|---|
| 三种 profile 可校验 | profile 单测与 schema/JSON 解析 | PASS |
| 负载和七类故障冻结 | `docs/soak_protocol.md`、resolved profile | PASS |
| 每循环触发、分类、恢复 | summary oracle + 缺循环反例测试 | PASS |
| 进程组与有界关闭 | runner 单测、smoke | PASS |
| 资源、队列、时延和 MQTT 统计 | heartbeat、gateway JSONL、summary | PASS |
| 日志不覆盖且单段有界 | rotating writer 单测与 segment oracle | PASS |
| smoke 不冒充长稳 | `long_soak_pass=false` 单测 | PASS |
| 临时产物清理 | 文档完成后执行专项清理检查 | PASS |

## 6. 后续移交

T04 可以在用户确认一个提交 ID 后，使用 `software_preflight.json` 执行 3600 秒软件预跑。只有
T04 通过后，S6 才使用相同 runner 和 `software_release.json` 执行连续 28800 秒。任何 profile
或门限修改都应版本化并重新执行预跑，不得事后放宽门限来改写旧结果。

本次未执行任何 `git add`、`git commit` 或 `git push`。
