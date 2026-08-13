# P3-S6 软件 MVP 候选 Release 验收报告

> 验收日期：2026-08-13  
> 冻结源码提交：`ecb4cacdf56e187940b341d647ff1529ca2f0b0c`  
> 当前结论：**P3-S6 软件范围 PASS；树莓派 ARM64、真实 RS485 和硬件长稳仍未验证**

## 1. 总结

P3-S6-T01 至 T05 已完成。绑定冻结提交的 `software_release_v2` 连续运行
28800.055 秒，`summary.status=PASS`、`long_soak_pass=true`、未关闭失败为 0，
212/212 个自动 oracle 全部通过。随后完成 systemd 非 root 部署合同、异常重启、错误配置、
串口缺失、broker 不可达和重复启停验收，并从干净构建生成 x86_64 候选 Release。

本结论只覆盖 WSL2 Ubuntu 24.04 x86_64 上的 PTY + 回环 Mosquitto 软件 MVP。没有可用硬件时，
不能把它表述为树莓派部署 PASS、RS485 电气层 PASS、STM32/传感器联调 PASS 或生产 MQTT
TLS/凭据 PASS。

## 2. 工作块验收矩阵

| 工作块 | 关键产物或证据 | 结果 |
|---|---|---|
| P3-S6-T01 | 冻结 profile、提交和 Release driver；启动 8 小时长稳 | PASS |
| P3-S6-T02 | run ID `20260812T184831Z_soak_release_ecb4cac_001`、摘要、失败清单、SHA-256 | PASS |
| P3-S6-T03 | systemd unit、环境模板、合同测试和场景矩阵 v3 | PASS（WSL2 开发证据） |
| P3-S6-T04 | 性能与可靠性报告、资源趋势图 | PASS |
| P3-S6-T05 | Release/Debug/ASan/TSan、clang-tidy、格式、安装暂存和 RC bundle | PASS（x86_64） |

## 3. 8 小时长稳结论

正式证据位于本地忽略目录：

`artifacts/soak/release/20260812T184831Z_soak_release_ecb4cac_001/`

- 开始：2026-08-12T18:48:31.285Z；
- 结束：2026-08-13T02:48:31.977Z；
- runner monotonic 时长：28800.692 秒；有效 profile 时长：28800.055 秒；
- 七类故障 × 八个循环共 56 个故障窗口，全部触发并恢复；
- 晚响应隔离累计 302 次，丢弃旧事务字节 692 bytes；
- SHA256SUMS 复核 PASS，`failures.json=[]`；
- 证据目录 511,325,274 bytes，小于 5 GiB；最大日志段小于 64 MiB。

正常窗口成功率 99.9685%，吞吐 26.2005 requests/s；P95/P99/最大成功延迟分别为
0/0/20 ms。非目标从站最大成功间隔 2.998 秒，继续满足冻结的 10 秒门限。RSS 峰值
9.902 MiB、斜率 0.0404 MiB/hour、首末稳态差 0.430 MiB；fd 和线程稳态漂移均为 0。

## 4. systemd 验收

最终正式软件证据：

`artifacts/systemd/s6/20260813T033000Z_systemd_acceptance_ecb4cac_003/`

| 场景 | 观察 | 结果 |
|---|---|---|
| 静态校验 | `systemd-analyze verify` 返回 0 | PASS |
| 身份与目录 | `User/Group=iot-gw`，工作目录与环境文件为绝对路径 | PASS |
| SIGTERM | broker 不可达场景 277 ms 完成排空并停止，小于 5000 ms | PASS |
| 异常退出 | SIGKILL 后 PID 改变，`NRestarts=1`，约 2 秒后重启 | PASS |
| 错误配置 | 寄存器表错误退出 4，`NRestarts=0` | PASS |
| 串口缺失 | 服务保持 active、按有界退避重试、`NRestarts=0`，可正常停止 | PASS |
| broker 不可达 | 出现 `mqtt_connect_failed`，同时串口成功采集继续 | PASS |
| 重复启停 | 三轮停止为 54/55/54 ms，每轮残留 `gateway_app` 为 0 | PASS |

测试先行新增了五项 unit 合同，冻结非 root、Restart、停止预算、显式路径、安全加固和安装产物。
`RestartPreventExitStatus=2 3 4 5 6 7` 防止已分类的参数/配置/启动错误形成重启风暴，同时保留
信号崩溃的 `Restart=on-failure` 自动拉起。

## 5. 候选 Release 与质量门

候选包保存在本地忽略目录：

`artifacts/release/s6/industrial_iot_gateway-0.1.0-linux-x86_64-rc1.tar.gz`

| 门 | 结果 |
|---|---|
| Release + MQTT + PTY + warnings-as-errors | 182/182 PASS |
| Debug + MQTT + PTY + warnings-as-errors | 182/182 PASS |
| ASan/UBSan | 182/182 PASS |
| TSan + WSL2 address-layout workaround | 182/182 PASS |
| clang-tidy 18 | 全目标构建 PASS；仅保留既有 optional/异常逃逸提示 |
| clang-format 18 | 82 个 C++ 文件 PASS |
| Python `py_compile` | 21 个文件 PASS |
| JSON 解析 | 8 个配置文件 PASS |
| systemd 合同 | 5/5 PASS |
| CMake 安装暂存 | executable、unit、公开环境模板均存在并有 SHA-256 |

安装暂存哈希：

- `gateway_app`：`8169611044070caf151512bcd019b5d0adc8bfad2318923837728320ba86b609`；
- unit：`b17cb84ad251bbf450d920bc6ce87e164a1acbc8eeab916e4c7665c6270f89dd`；
- 环境模板：`1ffa6749a4ca8ba94e6c58c88bfce40e1a3493723178b5d89aee0a4362d10a0d`。

## 6. 首轮失败与最终证据选择

systemd 场景矩阵的前两轮分别暴露了错误预言机和 PTY 夹具复用问题，均保留在本地证据目录；
它们没有被改写为 PASS。最终 v3 每轮自行启动全新 PTY、动态取得节点并在结束时回收，
`summary.status=PASS` 且 `failures.json=[]`，因此 v3 是本报告引用的正式 systemd 证据。

## 7. 未关闭边界与 S7 移交

- 在 Raspberry Pi ARM64 上重新构建或安装 RC，并复跑 G5 systemd 场景；
- 硬件到货后执行 USB-RS485、STM32 和真实传感器联调，软件证据不得替代电气证据；
- 生产部署前补充 MQTT TLS、凭据管理、日志保留策略和时间同步；
- 项目许可证仍待所有者最终确认，当前 RC 不宣称已完成公开发行授权。

本阶段没有检查 Git 工作区状态，也没有执行 `git add`、`git commit` 或 `git push`。
