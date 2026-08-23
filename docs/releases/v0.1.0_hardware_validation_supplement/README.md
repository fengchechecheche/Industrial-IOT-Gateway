# Industrial-IOT-Gateway v0.1.0 硬件验证补充证据包

本目录是既有 v0.1.0 本地双架构软件 Release 的轻量补充证据包。它封装已经通过的 G6 真实
硬件验收结论，不重新构建软件包、不重复执行故障矩阵或长稳，也不改写既有五文件 Release。

## 状态

```text
SUPPLEMENT_STATUS=COMPLETE
POST_RELEASE_G6_HARDWARE_SUPPLEMENT=PASS
V0_1_0_RELEASE_MANIFEST_HARDWARE_VALIDATED=false
PUBLISHED=false
TAG=null
```

这里的 `SUPPLEMENT_STATUS=COMPLETE` 表示证据封装完成；补充包的冻结 Git 身份由首次包含
`docs/project_freeze.md` 和本目录的提交确定。

## 与原 Release 的关系

- 原版本：`0.1.0`；
- 原产品源码提交：`2a0c961bb9677fb3cc54b1e57be88a96cd9db82b`；
- G6 证据固化提交：`18e0af7c57e916af641e1e868995cb1a4855a631`；
- 原 x86_64 包 SHA-256：`2b820fbed97ccd4192288cbc1a1eb8cbde45e6b90cf2686895934b681932e309`；
- 原 ARM64 包 SHA-256：`14d70f0aaa62bbc5aacf75a687e5493b6f91454c9af707abe8403e716aee7ab6`。

原 `release_index.json` 仍保持 `hardware_validated=false`。G6 的正确语义是“v0.1.0 发布后的
真实硬件补充验证”，不能写成原 Release 已被修改或重新发布。

## 已封装报告

| 文件 | 作用 |
|---|---|
| `reports/g6_evidence_matrix.md` | G6 T00～T05 总验收矩阵 |
| `reports/g6_t03_validation.md` | 17/17 真实故障与 systemd 恢复验收 |
| `reports/g6_t04_validation.md` | 3600 秒预跑和 28800 秒硬件长稳验收 |
| `reports/g6_t05_validation.md` | evidence、claim 和 Release 边界固化验收 |
| `evidence_index.json` | 正式运行、原始证据清单哈希和保留位置的公共安全索引 |
| `supplement_manifest.json` | Release 身份、G6 身份、能力与限制合同 |
| `SHA256SUMS` | 本目录全部内容文件的 SHA-256 |

原始 JSONL、journal、MQTT 捕获和资源样本没有复制到 Git。它们保留在树莓派的私有证据根中，
由 `evidence_index.json` 的逻辑定位符和 SHA256SUMS 哈希追踪。

## 验证方式

在本目录执行：

```bash
sha256sum -c SHA256SUMS
python3 -m json.tool supplement_manifest.json >/dev/null
python3 -m json.tool evidence_index.json >/dev/null
```

## 声明边界

可以声明当前 Raspberry Pi 4B、桌面短线 RS485、两只 TAS-WS-R00020、STM32 地址 4 和本地
Mosquitto 组合完成真实三从站集成、17 个受控故障场景及一次八小时正式硬件长稳。

不能外推为 CAN 已验证、传感器计量认证、长距离/EMC/浪涌验证、工业级、生产可用或无限期可靠；
也不能声明已创建 Git tag 或 GitHub Release。
