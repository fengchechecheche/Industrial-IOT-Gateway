# P3-S7-T04：稳定 Release 与复现演练验收报告候选稿

> 报告状态：CP5 正式 G5 与稳定 bundle 结果已固化，等待用户审核
> 编制日期：2026-08-14
> 当前结论：`T04_FORMAL_PASS_CANDIDATE`
> T04 总体验收：`PASS`
> 稳定 `v0.1.0`：`STABLE_BUNDLE_READY_NOT_PUBLISHED`

## 1. 结论摘要

P3-S7-T04 已完成合同 RED、最小 release/manifest/systemd 判定器、必要的运行时最小修复和 WSL2
开发预检。冻结路径下最终预检为 PASS：

```text
artifacts/releases/v0.1.0/20260813T083045Z_4af2614_001
```

该结果证明早期开发内容在 WSL2 x86_64 环境中通过构建、CTest、Sanitizer、静态分析、公开 demo、
临时安装和 manifest 检查。之后候选提交 `dd671cf3c8fda523ad5cc1e8539135fceda2b112`
完成 3600 秒预跑和 28800 秒正式 G4。G5 runner 及其修复只改变 release 测试工具和合同测试，
最终安装的 `gateway_app` 与 systemd unit 哈希保持不变，因此 G4 可由最终候选
`d2bcfde93b69dd7ebcf79d3901692235b2ae4df9` 合法继承。

最终候选已在 VMware Ubuntu Server 24.04 x86_64 原生 systemd 环境完成 G5 十场景，结果为
10/10 PASS；随后生成稳定 bundle，两个独立安装目录产生相同归档 SHA-256，并完成清洁解包、
三从站轮询、SIGTERM 停止和卸载复演。G6 按冻结合同记为 `NOT_RUN_OPTIONAL`。因此 T04 工程验收
可以判定 PASS，但本结论不表示已经创建 Git tag、GitHub Release 或完成真实硬件验证。

## 2. 输入和证据边界

| 项目 | 当前值 |
|---|---|
| T02 起点提交 | `4af261498a6c2561c6085a2f1ca57ef68924ea17` |
| 正式 G4 运行提交 | `dd671cf3c8fda523ad5cc1e8539135fceda2b112` |
| T04 最终候选提交 | `d2bcfde93b69dd7ebcf79d3901692235b2ae4df9` |
| 候选提交预检 run | `20260813T195811Z_dd671cf_001` |
| 正式 G4 run | `20260813T213614Z_soak_release_dd671cf_001` |
| 正式 G5 run | `20260814T111107Z_systemd_d2bcfde9_001` |
| 稳定 bundle run | `20260814T112347Z_d2bcfde9_001` |
| WSL2 预检环境 | Linux x86_64，内核 `6.18.33.2-microsoft-standard-WSL2`，`DEVELOPMENT_ONLY` |
| G5/打包环境 | VMware Ubuntu 24.04.4 LTS，x86_64，内核 `6.8.0-137-generic`，systemd 255 |
| `release_candidate_bound` | `true`（G4 继承、G5、manifest 与 bundle 均绑定最终候选） |
| `hardware_validated` | `false` |
| `arm64_validated` | `false` |

原 CP2 开发 run 仍只属于 WSL2 开发预检。正式 G4 绑定 `dd671cf...`；后续提交只修改 G5 runner
及其测试，最终安装的 `gateway_app` SHA-256 继续为
`d928295c426e52606a522fb178b1d43e71705a9d60730f1c368ff4d6336f1324`，systemd unit SHA-256
继续为 `b17cb84ad251bbf450d920bc6ce87e164a1acbc8eeab916e4c7665c6270f89dd`。正式 G5 和稳定包
均绑定最终候选 `d2bcfde...`，没有把 WSL2 预检冒充为原生 G5。

## 3. 实施产物

### 3.1 Release 工具

- `tools/release/release_runner.py`：唯一 run、路径边界、命令超时、失败保留和 WSL2 预检编排；
- `tools/release/manifest.py`：安装树 manifest、文件模式/大小/SHA-256、敏感信息和 symlink 逃逸
  检查；
- `tools/release/systemd_runner.py`：环境分类、unit 属性合同和十场景 G5 机器判定；
- `tools/release/__init__.py`：release 工具包入口；
- `.gitignore`：忽略 `/artifacts/releases/` 正式原始证据根。

### 3.2 合同测试

- `tests/release/test_release_manifest.py`；
- `tests/release/test_release_runner.py`；
- `tests/release/test_systemd_runner.py`；
- `tests/CMakeLists.txt` 中注册 release 三组测试，并在 MQTT ON 时定位 `mosquitto_sub`。

当前 `tests/release` 下包含既有 T02 测试在内共 42 项，全部通过。

### 3.3 稳定性修复

- `tests/integration/mqtt_broker_integration_test.cpp`：broker 观察端改为独立
  `mosquitto_sub` 子进程，保留真实 topic/payload 端到端验证，消除测试观察端的 Paho 并发问题；
- `src/runtime/gateway_runtime.cpp`：停止请求打断在途串口事务时记录
  `shutdown_cancelled`，不再误计为串口 I/O 错误；
- `tests/integration/gateway_runtime_pty_test.cpp`：新增关闭取消分类回归测试；
- `tools/soak/summary.py`：把 `shutdown_cancelled` 识别为已知结果，并从正常性能样本排除；
- `tests/soak/test_soak_summary.py`：新增关闭取消汇总合同；
- `tests/soak/run_smoke_ctest.py`：失败时保留并打印证据路径，成功时清理 smoke 证据。

上述变更触及运行时和 soak 判定器，所以 G4 继承条件不成立。

## 4. 测试先行证据

### 4.1 初始 RED

在新增实现之前，release 合同测试因 `tools.release` 不存在产生 3 个导入错误，证明 runner、
manifest 和 systemd 判定器尚未实现。

### 4.2 systemd staging 校验

预检最初直接校验源码 unit，`systemd-analyze` 报告：

```text
Command /usr/local/bin/gateway_app is not executable: No such file or directory
```

根因是程序只安装在临时 staging，而不是 WSL2 宿主机 `/usr/local/bin`。修复为对 staging 根执行：

```text
systemd-analyze verify --recursive-errors=no --root=<stage> <stage内unit>
```

最终静态校验返回 0。该证据只覆盖 unit 解析和 staging 目标存在性，不覆盖正式 G5 服务场景。

### 4.3 关闭取消汇总合同

新增测试先在旧汇总逻辑下失败：

```text
requests.unclassified_errors: actual 1, expected 0
```

加入 `shutdown_cancelled` 已知分类并从正常性能样本排除后，soak summary 5/5 PASS；TSan 下
`soak_smoke_integration` 定点复验 1/1 PASS，耗时 90.92 秒。

### 4.4 证据路径合同

新增测试证明旧默认路径为 `artifacts/releases/0.1.0`，不符合批准方案中的 `v0.1.0`。修复为
`artifacts/releases/v0.1.0` 后，release 合同测试 42/42 PASS，最终预检也在冻结路径重新执行。

## 5. 最终 WSL2 预检结果

最终 run：

```text
artifacts/releases/v0.1.0/20260813T083045Z_4af2614_001
```

| 门 | 结果 | 证据摘要 |
|---|---|---|
| Debug | PASS | configure/build 返回 0，CTest 188/188 |
| ASan/UBSan | PASS | configure/build 返回 0，CTest 188/188 |
| Release | PASS | configure/build 返回 0，CTest 188/188 |
| TSan | PASS | configure/build 返回 0，CTest 188/188 |
| MQTT OFF | PASS | configure/build 返回 0，CTest 176/176 |
| clang-tidy | PASS | Clang 18 全量构建返回 0 |
| clang-format | PASS | 全部 C++ 文件 `--dry-run --Werror` 返回 0 |
| Python | PASS | 全部工具和测试 `py_compile` 返回 0 |
| PTY+MQTT demo | PASS | `tools/demo.py --profile pty-mqtt` 返回 0 |
| CMake install | PASS | staging 安装返回 0 |
| systemd 静态检查 | PASS | staging root 下 `systemd-analyze verify` 返回 0 |
| 安装 manifest | PASS | 9 个必需文件，排序、权限、大小和 SHA-256 完整 |
| 敏感信息扫描 | PASS | `[]`，0 个 blocker |
| runner 总状态 | PASS | `failures.json=[]`，存在 `PASS` 标记 |

最终输出：

```text
T04_PREFLIGHT=PASS
G5=DEVELOPMENT_ONLY
HARDWARE_VALIDATED=false
```

## 6. 失败运行及修复追踪

失败证据没有被删除或改写，主要问题和处理如下：

| 问题 | 发现方式 | 修复 |
|---|---|---|
| Paho 观察端 TSan 数据竞争/锁问题 | 完整 TSan broker 集成测试 | 外部 `mosquitto_sub` 观察器 |
| 关闭时串口错误误计数 | ASan PTY 晚响应/关闭测试 | 新增 `shutdown_cancelled` 运行时分类 |
| smoke 失败证据被 wrapper 删除 | ASan smoke 偶发失败 | 失败保留证据，成功才清理 |
| `shutdown_cancelled` 被当成未知错误 | TSan 90 秒 smoke | 同步 soak 汇总分类和性能分母 |
| systemd 静态检查找不到程序 | 安装后 verify | 对 staging 根做非递归校验 |
| 证据根缺少版本前缀 `v` | 路径合同审查 | 默认根冻结为 `releases/v0.1.0` |

最终 run 是修复全部问题后重新执行的单次完整 PASS，不是把多个部分运行拼接成 PASS。

## 7. G1～G6 当前状态

| Gate | 当前状态 | 是否可作为正式结论 |
|---|---|---|
| G1 | `PASS` | 协议合同与当前 Release 回归通过，产品二进制哈希未改变 |
| G2 | `PASS` | 最终候选 Release 构建 85/85、CTest 191/191 |
| G3 | `PASS` | 故障矩阵语义继承，当前测试与 broker 恢复场景通过 |
| G4 | `FORMAL_PASS_INHERITED` | 28800 秒正式 run PASS；最终产品二进制与 unit 未改变 |
| G5 | `FORMAL_PASS` | 原生 Linux/systemd 十场景 10/10 PASS |
| G6 | `NOT_RUN_OPTIONAL` | 未执行真实硬件，不阻塞 Linux x86_64 软件 MVP |

早期开发 summary 中的 `G4=INHERITANCE_NOT_YET_FROZEN` 和本报告历史 CP2 中的
`RERUN_REQUIRED` 已由新的完整 28800 秒正式 run 取代。G4 当前唯一有效状态为 `FORMAL_PASS`。

## 8. Release blocker 状态

| Blocker | 当前状态 | 剩余关闭条件 |
|---|---|---|
| `S7-RB-04` | CLOSED | 最终候选、G4 继承、G5、bundle 和本报告已形成闭环 |
| `S7-RB-05` | CLOSED | 原生 Linux x86_64/systemd 十场景 10/10 PASS |
| `S7-RB-06` | CLOSED | 双目录安装与归档一致，manifest、内外层 SHA-256 全部通过 |

`S7-RB-04`～`S7-RB-06` 已全部关闭；开放 P0/P1 数量为 0。

## 9. 当前未完成项和下一检查点

T04 工程工作已经完成，当前剩余的是文档审核与后续移交：

1. 用户审核本报告与同步教程；
2. 审核通过后同步到独立仓库 `docs/`；
3. 如需创建 Git tag、上传 GitHub Release 或发布二进制，必须另行授权；
4. 移交 P3-S7-T05；真实硬件到货后作为独立增强工作块重新开启 G6。

当前不需要也不允许由本工作块代理执行 `git add`、`git commit`、`git push`、tag、GitHub
Release 或宿主机 `sudo` 改动。

## 10. 能力声明边界

当前允许表述：

> 最终候选 `d2bcfde...` 继承了未改变产品执行路径的正式 8 小时 G4，并在 VMware Ubuntu
> Server 24.04 x86_64 原生 systemd 环境完成 G5 十场景 10/10 PASS；稳定 bundle 已完成双目录
> 可复现打包和清洁复演。该结论仅适用于 Linux x86_64 软件 MVP。

当前禁止表述：

- “v0.1.0 已上传或公开发布”；
- “树莓派、ARM64、真实 RS485/CAN 或硬件台架已验证”。

## 11. 正式 G5 与稳定 bundle 证据

### 11.1 G5 十场景

| 场景 | 结果 | 关键观察 |
|---|---|---|
| unit 静态校验 | PASS | unit 属性与停止/重启合同一致 |
| 正常启动 | PASS | 请求成功 105 次，MQTT 发布成功 108 次 |
| SIGTERM 停止 | PASS | 215 ms，退出 0，无主进程残留 |
| SIGINT 直接进程 | PASS | 112 ms，退出 0 |
| 异常重启 | PASS | 新 PID，约 2267 ms 恢复 |
| 非法配置 | PASS | 退出码 4，无重启风暴 |
| 串口缺失 | PASS | 串口打开成功 0，不忙循环 |
| broker 不可用 | PASS | MQTT 失败 3 次；串口仍成功 255 次；恢复后重连并发布 |
| journal 可观测性 | PASS | 启动、请求、MQTT、停止和摘要事件可关联 |
| 重复启停 | PASS | 10/10；fd/thread 漂移 0，无残留 PID |

G5 `summary.status=PASS`、`cleanup_ok=true`、`failures.json=[]`，全部 `SHA256SUMS` 有效；测试结束后
服务用户、组、unit、配置、二进制、进程和 18884 监听端口均无残留。

### 11.2 稳定 bundle

| 项目 | 结果 |
|---|---|
| 包名 | `industrial_iot_gateway-0.1.0-linux-x86_64.tar.gz` |
| 字节数 | 217125 |
| SHA-256 | `fced2ddf55c6fcaa0f7de1c9554905152eaf7b887cacccc71da671d21313b412` |
| 独立安装树 | 2 份，manifest 完全一致 |
| 规范化归档 | 两次 SHA-256 完全一致 |
| 正式安装文件 | 9 个，不含临时 `/etc` 副本或测试工具 |
| 敏感信息扫描 | `[]` |
| 清洁解包复演 | PASS；三个 PTY 从站分别处理 25、56、24 次请求 |
| 关闭与卸载 | SIGTERM 退出 0；PTY 退出 0；临时安装根已删除 |
| 发布状态 | bundle 已就绪，但未创建 tag 或上传远程 Release |

## 12. CP5 判定

```text
CP5_T04_FORMAL_PASS_CANDIDATE
T04_RELEASE_CANDIDATE_COMMIT=d2bcfde93b69dd7ebcf79d3901692235b2ae4df9
G1=PASS
G2=PASS
G3=PASS
G4=FORMAL_PASS_INHERITED
G5=FORMAL_PASS
G6=NOT_RUN_OPTIONAL
T04=PASS
STABLE_V0.1.0_BUNDLE=READY
PUBLISHED=false
HARDWARE_VALIDATED=false
ARM64_VALIDATED=false
```

下一检查点是用户审核两份 T04 候选文档。审核通过后同步正式维护版本并移交 P3-S7-T05。
