# P3-S7-T06：v0.1.0 本地双架构 Release 验收报告

> 文档状态：`USER_APPROVED_AND_FINALIZED`
> 资产验收状态：`PASS`
> 仓库文档固化状态：`PASS`
> 产品候选：`2a0c961bb9677fb3cc54b1e57be88a96cd9db82b`
> T06 工具提交：`db309472b76d0f64d30d8d001db4cf40ace8380a`
> 版本：`0.1.0`
> 日期：2026-08-18

## 1. 验收结论

同一产品候选已在 VMware Ubuntu Server 24.04 x86_64 和 Raspberry Pi 4B Ubuntu
Server 24.04 ARM64 上分别完成原生 Release 构建、完整 CTest、确定性归档、清洁解包、
PTY/Mosquitto 演示、SIGTERM 和卸载复演。两份 final bundle 已组成精确五文件的本地双架构
Release 集，资产与仓库文档固化验收结论均为 PASS。

本结论只证明本地 Linux x86_64 与 Linux ARM64 软件 Release 就绪，不等于 GitHub Release、
Git tag、生产发布或真实 RS485/STM32/CAN 硬件验证。

## 2. 冻结身份

| 身份 | 值 | 作用 |
|---|---|---|
| 产品源码提交 | `2a0c961bb9677fb3cc54b1e57be88a96cd9db82b` | 两个正式软件包的唯一源码候选 |
| T06 汇总工具提交 | `db309472b76d0f64d30d8d001db4cf40ace8380a` | 验证并组装五文件 Release 集 |
| T04 正式重启证据 | `20260818T063221Z_reboot_2a0c961_001` | 证明 ARM64 长稳、systemd 与重启恢复门 |
| Release 版本 | `0.1.0` | 两个包与 release index 的统一版本 |

工具提交和产品提交保持分离；后提交的验收工具没有改变已冻结产品源码。

## 3. 汇总工具质量门

| 检查项 | 结果 |
|---|---|
| 定向 T06 合同测试 | `4/4` PASS |
| 全部显式 Python 测试 | `132/132` PASS |
| Debug CTest | `195/195` PASS |
| ASan/UBSan CTest | `195/195` PASS |
| Release CTest | `195/195` PASS |
| Clang 18 TSan 条件门 | `195/195` PASS |
| clang-tidy | PASS；仅既有 C++ 告警 |
| clang-format dry-run | PASS |
| Python AST 与 `git diff --check` | PASS |

## 4. Linux x86_64 final bundle

- 主机：VMware Ubuntu Server 24.04 x86_64；
- run：`20260818T073354Z_2a0c961_001`；
- 包：`industrial_iot_gateway-0.1.0-linux-x86_64.tar.gz`；
- 大小：217586 字节；
- SHA-256：`2b820fbed97ccd4192288cbc1a1eb8cbde45e6b90cf2686895934b681932e309`；
- 完整 CTest：PASS，耗时 172056 ms；
- clean smoke：27 次成功请求、30 次 MQTT 发布、203 ms 有界关闭；
- 两个独立安装根归档一致，安装树精确 9 文件；
- x86-64 ELF 与动态依赖检查通过，无 `ldd not found`；
- 敏感扫描、清洁解包、SIGTERM 和卸载复演通过。

## 5. Linux ARM64 final bundle

- 主机：Raspberry Pi 4B / Ubuntu Server 24.04 ARM64；
- run：`20260818T073403Z_2a0c961_001`；
- 包：`industrial_iot_gateway-0.1.0-linux-arm64.tar.gz`；
- 大小：200393 字节；
- SHA-256：`14d70f0aaa62bbc5aacf75a687e5493b6f91454c9af707abe8403e716aee7ab6`；
- 原生构建耗时 203349 ms，完整 CTest 耗时 181219 ms；
- clean smoke：27 次成功请求、31 次 MQTT 发布、103 ms 有界关闭；
- 两个独立安装根归档一致，安装树精确 9 文件；
- AArch64 ELF 与动态依赖检查通过，无 `ldd not found`；
- 敏感扫描、清洁解包、SIGTERM 和卸载复演通过；
- 结束温度 52.5°C，`get_throttled=0x0`。

## 6. 五文件本地 Release 集

正式组装 run：`20260818T074233Z_2a0c961_001`。

本地 Release 集精确包含：

| 文件 | 字节数 | 内容边界 |
|---|---:|---|
| `industrial_iot_gateway-0.1.0-linux-x86_64.tar.gz` | 217586 | Linux x86_64 软件包 |
| `industrial_iot_gateway-0.1.0-linux-arm64.tar.gz` | 200393 | Linux ARM64 软件包 |
| `SHA256SUMS` | 397 | 四项内容文件的 SHA-256 |
| `release_index.json` | 1254 | 平台、证据、能力与发布边界索引 |
| `RELEASE_NOTES.md` | 610 | 本地 Release 范围和限制 |

核验结果：

- 目录精确为 5 个文件；
- `SHA256SUMS` 全部有效；
- 两个归档各包含 9 个正式安装文件；
- `release_index.json` 的 `release_status=LOCAL_RELEASE_READY`；
- `source_revision` 精确为冻结产品提交；
- `x86_64_validated=true`；
- `arm64_native_build_validated=true`；
- `arm64_systemd_validated=true`；
- `arm64_long_soak_validated=true`；
- `arm64_release_bundle_ready=true`；
- `published=false`、`tag=null`、`hardware_validated=false`；
- 项目自有代码为 MIT，第三方边界指向包内 `THIRD_PARTY_NOTICES.md`。

## 7. 临时资源清理

- VMware x86_64 与树莓派 ARM64 的本轮 build 目录已删除；
- 树莓派临时 x86_64 传输输入和临时 T06 工具检出已删除；
- Windows 临时传输目录和 Git bundle 已删除；
- 正式原生 bundle、T04 及 Release 组装 evidence 保留；
- 没有执行 Git tag、GitHub Release、push 或其他远程发布动作。

## 8. 能力声明边界

允许声明：

```text
v0.1.0 已形成 Linux x86_64 与 Linux ARM64 的本地双架构软件 Release 集。
两个软件包来自同一候选提交，并分别通过原生构建、测试、确定性归档和 clean smoke。
Raspberry Pi 4B ARM64 已通过 8 小时软件长稳与受控重启自启演练。
```

禁止声明：

```text
已发布 GitHub Release
已创建 v0.1.0 Git tag
已通过真实 USB-RS485、商用 Modbus 从站、STM32 或 CAN 硬件验收
已达到工业级、生产可用或现场高可靠
```

## 9. 最终状态

```text
PASS_P3_S7_T06_LOCAL_DUAL_ARCH_RELEASE_READY
PUBLISHED=false
HARDWARE_VALIDATED=false
TAG=null
```
