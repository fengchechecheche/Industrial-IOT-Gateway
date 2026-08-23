# Industrial-IOT-Gateway 证据保留清单

> 状态：`ACTIVE_RETENTION`
>
> 日期：2026-08-23

## 1. 保留原则

- Git 只保存公共安全报告、证据索引、合同和校验值；
- 大体积 JSONL、journal、MQTT 捕获和资源样本继续保存在树莓派私有证据根；
- 原始层是事实来源，派生 CSV 和图表可以重建；
- 没有完成独立备份和哈希复核前，不删除树莓派正式证据；
- 失败证据和已解释异常同样保留，不能只保留 PASS 摘要；
- `build/`、缓存、临时补丁和可再生预览不属于长期证据。

## 2. 必须长期保留的 Release 资产

逻辑位置：`planning-output:local_release/v0.1.0/`

| 文件 | SHA-256 | 状态 |
|---|---|---|
| `industrial_iot_gateway-0.1.0-linux-x86_64.tar.gz` | `2b820fbed97ccd4192288cbc1a1eb8cbde45e6b90cf2686895934b681932e309` | RETAIN |
| `industrial_iot_gateway-0.1.0-linux-arm64.tar.gz` | `14d70f0aaa62bbc5aacf75a687e5493b6f91454c9af707abe8403e716aee7ab6` | RETAIN |
| `release_index.json` | `0b009cd817d3797f056632f50264450fc7f1bd1f7c8f753f8fee5c2f8e062ee3` | RETAIN |
| `RELEASE_NOTES.md` | `4fce78b9f28883ff5eb1418f9428d11bcf976601db1ea57a7faa8075837caa76` | RETAIN |
| `SHA256SUMS` | 负责校验上述四项 | RETAIN |

不得因 G6 PASS 修改这些资产或原 `release_index.json`。

## 3. 必须长期保留的 G6 原始证据

下表使用公共安全逻辑定位符；具体主机路径不进入公共仓库。

| 范围 | 逻辑定位符 | 字节数 | SHA256SUMS 文件 SHA-256 | 状态 |
|---|---|---:|---|---|
| T02 正常基线 | `raspberry-pi:g6/t02/b35a01e/evidence/normal_baseline` | 472558 | `605aaa96295cf15f1dc45602a334c7bbd2682cb777d6353aee59c3a4e41c7dc9` | RETAIN |
| T02 TAS-A 故障 | `raspberry-pi:g6/t02/b35a01e/evidence/fault_tas_a_final` | 1921276 | `adda7a52236f09edc184b0869b87154f3fd7b3b382a53544c81d0eaa8e787211` | RETAIN |
| T02 TAS-B 故障 | `raspberry-pi:g6/t02/b35a01e/evidence/fault_tas_b_final` | 1894230 | `16881edf85c3c96f12c1f53429a2a9d6c4b7acd72ca7c0bcd73c2c6da5f72142` | RETAIN |
| T03 17 场景 | `raspberry-pi:g6/t03/e873b5e/20260822T214326Z_g6_t03_e873b5e_001` | 139500939 | `07f7fefa117b53d45982b2cd6d172ed322b3ff5acc1cd85c5908b551ddf74f02` | RETAIN |
| T04 一小时预跑 | `raspberry-pi:g6/t04/c5ddc36/20260823T032544Z_g6_t04_preflight_c5ddc36_001` | 107495038 | `a899a024cdfcb756b916339aed8dd837f0a880cd65b90b679b935cb8509528f8` | RETAIN |
| T04 八小时正式运行 | `raspberry-pi:g6/t04/c5ddc36/20260823T043408Z_g6_t04_release_c5ddc36_001` | 860494667 | `d02be15e7ed275fc55c4b0f3c180c33b915d4cc821a17704ebe2c7e6effaeb86` | RETAIN |

树莓派 T03 全工作根当前约 143508405 字节，T04 全工作根当前约 1032800859 字节。全工作根还包含
构建/runner 退出码、smoke 和恢复记录；在单独审核前不按目录名批量删除。

## 4. Git 中长期保留的公共安全投影

- `docs/p3_s7_g6_t00_evidence_reuse.md`；
- `docs/p3_s7_g6_t01_validation.md`；
- `docs/p3_s7_g6_t02_validation.md`；
- `docs/p3_s7_g6_t03_validation.md`；
- `docs/p3_s7_g6_t04_validation.md`；
- `docs/p3_s7_g6_t05_validation.md`；
- `docs/p3_s7_g6_evidence_matrix.md`；
- `docs/p3_s7_t05_claim_ledger.yaml`；
- `docs/p3_s7_t05_claim_source_map.csv`；
- `docs/releases/v0.1.0_hardware_validation_supplement/`。

## 5. 备份状态与风险

当前已核验树莓派主副本仍存在，正式目录大小和 SHA256SUMS 文件哈希已经记录。大体积原始证据
没有复制进 Git，也没有在本次轻量封装中创建第二份完整镜像。因此当前状态为：

```text
PRIMARY_EVIDENCE_COPY=VERIFIED
SECOND_FULL_RAW_BACKUP=NOT_CREATED_BY_THIS_TASK
DELETE_AUTHORIZATION=NONE
```

这不阻塞项目工程状态冻结，但意味着树莓派存储仍是原始大体积证据的主要副本。后续如果迁移或
清理设备，应先复制到独立存储，逐项校验 SHA-256，再授权删除源目录。

## 6. 可清理对象

以下对象在确认不属于正式证据后可以重新生成，不要求长期保留：

- 仓库根 `build/`；
- `build-debug/`、`build-release/`、`build-asan/`、`build-tidy/` 等构建目录；
- `__pycache__/`、`*.pyc`、编辑器缓存和 `*.orig`；
- 本轮文档预览、校验脚本和临时复制目录；
- 已被正式报告和 SHA 清单取代的重复日志副本。

本清单不授权删除。任何批量清理仍需先确认精确路径和内容，再由用户单独授权。
