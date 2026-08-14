# P3-S7-ARM64-T01：树莓派 ARM64 环境预检报告

> 报告状态：CP0～CP8 与 CP-D 审核已完成，正式维护版本已同步  
> 检查日期：2026-08-14  
> 检查方式：从 Windows 控制端通过 SSH 执行只读命令  
> 当前结论：`PASS_P3_S7_ARM64_T01_ENVIRONMENT_AND_NATIVE_BUILD`  
> 系统修改：Ubuntu 官方系统更新、授权重启及冻结依赖安装  
> 依赖安装：已完成并复核  
> Git 写操作：未执行

## 1. 结论摘要

目标机确认是 Raspberry Pi 4 Model B Rev 1.5，运行 Ubuntu 24.04.4 LTS ARM64，内核、用户空间
和 Debian 包架构分别为 `aarch64`、64 位和 `arm64`，PID 1 为 systemd，且不是 WSL、容器或
虚拟机。

系统首次初始化已完成，systemd 状态为 `running`，失败 unit 数量为 0，NTP 已同步。目标机具有
约 7.6 GiB 内存和约 110 GiB 可用根分区空间；当前没有 swap，但对本机 8 GiB 内存配置不是
阻塞项。前两道 Debug 构建门按原方案使用 `-j2`，后续构建经用户确认改用 `-j4`。

预检时 SoC 温度为 53.5°C，`vcgencmd get_throttled` 返回 `0x0`，本次启动的内核日志没有发现
欠压、节流、过热、OOM、I/O error 或只读文件系统记录。当前不存在同名网关服务、配置、二进制
或精确进程，也没有 Mosquitto 服务和 1883/18884 监听冲突。

全部构建、测试、MQTT 和静态分析依赖均能从 Ubuntu Noble 官方 ARM64 软件源获得，软件源 HTTP
探测返回 200。CP-A 据此允许进入 CP-B；当前 CP-B 也已安装并复核完成。

## 2. 身份与系统准入

| 检查项 | 结果 | 判定 |
|---|---|---|
| 设备型号 | Raspberry Pi 4 Model B Rev 1.5 | PASS |
| 内核架构 | `aarch64` | PASS |
| 用户空间 | 64 位 | PASS |
| Debian 包架构 | `arm64` | PASS |
| 操作系统 | Ubuntu 24.04.4 LTS Noble | PASS |
| 内核 | Linux `6.8.0-1047-raspi` | PASS |
| PID 1 | systemd | PASS |
| 虚拟化分类 | `none`，物理目标机 | PASS |
| SSH | 公钥批处理连接成功 | PASS |

公开报告未记录 IP、IPv6、MAC、主机名、用户名、机器 ID、Boot ID 或 SSH 指纹。

## 3. 系统健康和时间

| 检查项 | 结果 | 判定 |
|---|---|---|
| cloud-init | `done`，errors 为空 | PASS |
| systemd 整体状态 | `running` | PASS |
| failed unit | 0 | PASS |
| 时区 | Asia/Shanghai | PASS |
| NTP | 已同步，Leap normal，Stratum 2 | PASS |
| 本地 RTC | 关闭，符合树莓派常见配置 | INFO |

系统刚启动约 28 分钟。启动早期没有持久 RTC 时钟导致的初始日志时间不作为阻塞项；当前 NTP
已经同步，后续证据以 UTC run ID 和同步后的系统时间为准。

## 4. 计算与存储资源

| 项目 | 检查值 | 方案门限 | 判定 |
|---|---:|---:|---|
| CPU | 4 核 Cortex-A72，AArch64 | AArch64 | PASS |
| 内存 | 7.6 GiB 总量，约 7.2 GiB available | 至少 2 GiB | PASS |
| Swap | 0 | 8 GiB 机型不强制 | INFO |
| 根分区 | 117 GiB，总可用约 110 GiB | 至少 8 GiB | PASS |
| inode | 约 99% 可用 | 至少 10% 可用 | PASS |
| 存储介质 | 约 119 GiB microSD，ext4 根分区 | 可读写且无 I/O error | PASS |

前两道 Debug 构建门实际采用 `cmake --build ... --parallel 2` 和 `ctest --parallel 2`。用户确认
树莓派没有其他项目负载后，后续编译在门前健康检查通过时采用 `cmake --build ... --parallel 4`；
CTest 和 Sanitizer 测试并行度继续单独控制，不能直接等同于构建并行度。

## 5. 电源与散热

| 检查项 | 结果 | 门限 | 判定 |
|---|---|---|---|
| thermal sysfs | 约 53.6°C | 低于 70°C 推荐值 | PASS |
| `vcgencmd measure_temp` | 53.5°C | 低于 70°C推荐值 | PASS |
| `vcgencmd get_throttled` | `0x0` | 当前位和历史位均为 0 | PASS |
| ARM 当前频率 | 约 1.2 GHz，动态调频 | 仅记录 | INFO |
| 内核告警 | 未发现欠压、节流、过热、OOM 或存储错误 | 不允许阻塞告警 | PASS |

后续每个构建门开始和结束时仍需重新采样温度与 `get_throttled`。达到 80°C、出现当前欠压或当前
节流时，必须停止启动新的构建任务。

## 6. 网络与软件源

| 检查项 | 结果 | 判定 |
|---|---|---|
| 当前连接 | Wi-Fi，默认路由存在 | PASS |
| Ubuntu Ports DNS | 可以解析 | PASS |
| Ubuntu Noble InRelease | HTTP 200，仅探测未下载 | PASS |
| 网络隐私 | 报告未保存地址、SSID 或 MAC | PASS |

有线网络仍然是长稳和正式 systemd 验收的优先选择，但 Wi-Fi 不阻塞当前依赖安装和原生构建。

## 7. 现有部署冲突

| 检查项 | 结果 | 判定 |
|---|---|---|
| `industrial_iot_gateway.service` | 不存在 | PASS |
| `/etc/industrial_iot_gateway` | 不存在 | PASS |
| `/usr/local/bin/gateway_app` | 不存在 | PASS |
| `gateway_app` 精确进程 | 不存在 | PASS |
| `gateway_pty_bus` 精确进程 | 不存在 | PASS |
| `mosquitto` 精确进程 | 不存在 | PASS |
| Mosquitto systemd 服务 | inactive/未安装 | PASS |
| 1883/18884 端口 | 无监听 | PASS |

首次宽松进程查询曾匹配到本次 SSH 命令行自身；随后使用精确进程名复核，三类目标进程均不存在。
该自匹配不属于部署冲突。

## 8. ARM64 依赖候选

以下 Candidate 均来自 `ports.ubuntu.com` Noble 官方 `arm64` 仓库：

| 包 | ARM64 Candidate | 当前状态 |
|---|---|---|
| `build-essential` | `12.10ubuntu1` | 未安装 |
| `cmake` | `3.28.3-1build7` | 未安装 |
| `ninja-build` | `1.11.1-2` | 未安装 |
| `pkg-config` | `1.8.1-2build1` | 未安装 |
| `git` | `1:2.43.0-1ubuntu7.3` | 已安装，同 Candidate |
| `python3` | `3.12.3-0ubuntu2.1` | 已安装，同 Candidate |
| `clang-18` | `1:18.1.3-1ubuntu1` | 未安装 |
| `clang-format-18` | `1:18.1.3-1ubuntu1` | 未安装 |
| `clang-tidy-18` | `1:18.1.3-1ubuntu1` | 未安装 |
| `libgtest-dev` | `1.14.0-1` | 未安装 |
| `libyaml-cpp-dev` | `0.8.0+dfsg-6build1` | 未安装 |
| `libpaho-mqttpp-dev` | `1.2.0-2` | 未安装 |
| `libpaho-mqtt-dev` | `1.3.13-1build2` | 未安装 |
| `nlohmann-json3-dev` | `3.11.3-1` | 未安装 |
| `mosquitto` | `2.0.18-1build3` | 未安装 |
| `mosquitto-clients` | `2.0.18-1build3` | 未安装 |

Paho、nlohmann/json、Mosquitto、GoogleTest、yaml-cpp、CMake 与 Clang 版本与既有 Ubuntu 24.04
x86_64 项目基线一致。当前不存在需要添加 PPA、源码编译第三方依赖或使用非官方 `.deb` 的情况。

## 9. CP-A 验收矩阵

| 门 | 状态 |
|---|---|
| 环境准入 | PASS |
| ARM64 身份 | PASS |
| Ubuntu 24.04/systemd | PASS |
| 系统健康 | PASS |
| 时间同步 | PASS |
| CPU/内存/磁盘/inode | PASS |
| 电源与散热 | PASS |
| 网络与官方软件源 | PASS |
| 部署冲突 | NONE |
| ARM64 依赖可用性 | PASS |

最终判定：

```text
CP_A_ARM64_ENVIRONMENT_PREFLIGHT_PASS
arm64_native_build_pass=false
arm64_software_integration_validated=false
arm64_systemd_validated=false
hardware_validated=false
```

该 CP-A 快照中的 `arm64_native_build_pass` 为 false；当前虽然已经完成依赖安装，但仍未传输
冻结源码或执行任何 ARM64 构建和 CTest，所以该字段在 CP-B 结束时仍保持 false。

## 10. CP-B 执行结果

### 10.1 首次系统自动更新

执行 `apt-get update` 后，树莓派首次启动的 `unattended-upgrade` 正在持有 dpkg 锁。实施过程没有
终止自动更新，也没有删除锁文件，而是等待它自然完成。该更新升级了 `libc6`、systemd、OpenSSH
和 Raspberry Pi 内核等基础组件，并要求重启。

经用户明确授权后执行一次 `sudo reboot`。重启后：

- 运行内核从 `6.8.0-1047-raspi` 更新为 `6.8.0-1060-raspi`；
- systemd 恢复为 `running`，失败 unit 为 0；
- `dpkg --audit` 为空，`apt-get -s check` 无错误；
- `/var/run/reboot-required` 不再存在；
- 自动更新期间曾失败的 `fwupd-refresh.service` 重启后为正常 inactive，未继续造成 degraded；
- 温度 53.0°C，`get_throttled=0x0`。

### 10.2 冻结依赖安装

全部依赖使用 `--no-install-recommends` 和冻结版本安装。最终状态：

| 类别 | 结果 |
|---|---|
| GCC/G++ | 13.3.0，ARM64 |
| CMake | 3.28.3 |
| Ninja | 1.11.1 |
| pkg-config | 1.8.1 |
| Clang/clang-format/clang-tidy | 18.1.3 |
| Python | 3.12.3 |
| Git | 2.43.0 |
| GoogleTest | 1.14.0，ARM64 |
| yaml-cpp | 0.8.0，ARM64 |
| Paho MQTT C++ | 1.2.0，ARM64 |
| Paho MQTT C | 1.3.13，ARM64 |
| nlohmann/json | 3.11.3，架构无关 `all` |
| Mosquitto/clients | 2.0.18，ARM64 |

Paho 和 yaml-cpp 动态库均由 linker 识别为 AArch64，并从 `/lib/aarch64-linux-gnu/` 加载。没有
发现 `amd64`、`i386` 或 x86_64 动态库污染。

Ubuntu 包安装自动把 Mosquitto 设置为 enabled/active，默认只监听 `127.0.0.1:1883` 和
`[::1]:1883`。实施过程没有修改 `/etc/mosquitto/`、匿名访问、监听地址或服务启停策略；后续项目
验收仍使用独立配置和非默认端口，不能把全局 broker 状态写入测试合同。

安装后 systemd 为 `running`，失败 unit 为 0，温度 54.0°C，`get_throttled=0x0`，可用空间约
109 GiB，无再次重启要求。

CP-B 判定：

```text
CP_B_ARM64_DEPENDENCIES_INSTALLED=PASS
ARM64_TOOLCHAIN_READY=true
SOURCE_TRANSFERRED=false
arm64_native_build_pass=false
arm64_software_integration_validated=false
arm64_systemd_validated=false
hardware_validated=false
```

## 11. CP5 执行结果

冻结提交 `8e0e909a8b7576ab80b6f2ade186631910226b47` 已通过 `git archive` 导出并传输到
树莓派隔离目录。归档大小为 384861 bytes，SHA-256 为
`426ccb871551f0a96f459c4e49aced2f0071e921c333dd4256528e744e4f1655`，树莓派接收后校验
为 `OK`。

解压结果包含 213 个普通文件、0 个符号链接，与冻结 Git 树一致；未发现 `.git`、构建目录、
Python 字节码、`.orig` 或 Codex 临时文件。归档默认模式产生的组写权限已经在不改变 Git
可执行位的前提下移除，最终为 169 个 `0644` 文件和 44 个 `0755` 文件，组或其他用户可写
对象为 0。

完整记录见同目录下的 `P3-S7-ARM64-T01_CP5_source_transfer.md`。完成校验后已删除 Windows
中转目录和树莓派传输归档副本，只保留树莓派 `source/` 冻结源码基线。

```text
CP5_FROZEN_SOURCE_TRANSFER_AND_VALIDATION=PASS
SOURCE_TRANSFERRED=true
arm64_native_build_pass=false
```

## 12. CP6 MQTT OFF 初次执行结果

MQTT OFF 的 CMake 配置和 ARM64 原生编译通过，`gateway_app`、`gateway_pty_bus` 和
`pty_slave` 均为 AArch64 ELF。CTest 注册 179 项，其中 178 项通过，唯一失败项为
`demo_cli_unit`。

失败根因不是 C++/PTY 的 ARM64 编译问题，而是 Linux x86_64 专用 demo 的单元测试没有为
模拟的 x86_64 场景显式注入 `machine="x86_64"`，导致当前 AArch64 宿主架构泄漏进逻辑测试，
7 个断言被 `unsupported_architecture:aarch64` 提前截断。完整证据和修复边界见同目录下的
`P3-S7-ARM64-T01_CP6_mqtt_off_validation.md`。

```text
CP6_MQTT_OFF_CONFIGURE=PASS
CP6_MQTT_OFF_BUILD=PASS
CP6_MQTT_OFF_CTEST=FAIL_178_OF_179_PASS
CP6_MQTT_OFF=BLOCKED
arm64_native_build_pass=false
```

## 13. CP6 MQTT OFF 修复后复验

用户提交 `e62de0e946820c163c25fd7c82ebc3c4bc07afb5` 只修改
`tests/release/test_demo.py`，显式隔离模拟的 x86_64 架构，`tools/demo.py` 未修改。该提交先在
x86_64 上通过定向 8/8、Debug/Release/ASan/TSan 完整 191/191、MQTT OFF 179/179、
clang-tidy 和 clang-format。

随后从该精确提交生成 SHA-256 为
`cffcdae741d2b29583a10ff272ccdc818c5468f3afa458fb035561a4d9c8c517` 的归档，在树莓派
隔离目录重新校验为 `OK`。ARM64 定向 unittest 8/8 PASS，AArch64 正式 demo 仍返回退出码 3，
MQTT OFF 原生构建和完整 CTest 为 179/179 PASS。

```text
PASS_ARM64_DEMO_TEST_PORTABILITY_MINIMAL_FIX=true
CP6_MQTT_OFF=PASS
arm64_native_build_pass=false
```

## 14. CP6 MQTT ON + PTY Debug 执行结果

同一提交 `e62de0e946820c163c25fd7c82ebc3c4bc07afb5` 已在独立
`debug-full-e62de0e94682/` 目录完成 MQTT ON + PTY Debug 配置和原生构建。三个主要产物均为
AArch64 ELF，`gateway_app` 的动态依赖不存在 `not found`。

CTest 清单与 x86_64 完整基线一致，共 191 项；最终 191/191 PASS。测试结束后无 Python 字节码和
遗留项目进程。树莓派温度 50.1°C、`get_throttled=0x0`、可用内存约 7.2 GiB、可用磁盘约
109 GiB。完整证据见同目录下的 `P3-S7-ARM64-T01_CP6_mqtt_on_debug_validation.md`。

```text
CP6_MQTT_ON_DEBUG=PASS
CP6_MQTT_ON_DEBUG_CTEST=PASS_191_OF_191
arm64_native_build_pass=false
```

## 15. CP6 MQTT ON + PTY Release 执行结果

同一候选提交已经在独立 `release-full-e62de0e94682/` 目录完成 MQTT ON + PTY Release 配置、
ARM64 原生编译和完整 CTest。构建使用 `--parallel 4`，三个主要产物均为 AArch64 ELF，动态依赖
不存在 `not found`，最终 Ninja 构建图没有剩余动作。

首次构建的外层 SSH 执行器在 180 秒超时，但远端编译没有代码错误；在相同构建目录和相同参数下
完成剩余 7 个增量动作后，构建真实退出码为 0。CTest 注册 191 项并最终 191/191 PASS，退出码 0，
真实用时 144.32 秒。测试后温度 51.6°C、`get_throttled=0x0`、可用内存约 7.2 GiB、可用磁盘
约 109 GiB。完整证据见同目录下的 `P3-S7-ARM64-T01_CP6_release_validation.md`。

```text
CP6_RELEASE=PASS
CP6_RELEASE_CTEST=PASS_191_OF_191
arm64_native_build_pass=false
```

## 16. CP7 ARM64 ASan/UBSan 执行结果

同一候选提交已在独立 `asan-ubsan-e62de0e94682/` 目录完成 ASan/UBSan 配置、`-j4` 原生编译和
单并行完整回归。85/85 个 Ninja 动作完成，产物实际链接 ARM64 `libasan.so.8` 和
`libubsan.so.1`，CTest 191/191 PASS，退出码 0，真实用时 204.94 秒。

除 CTest 数字外，已扫描 CTest 后台输出和逐项 `LastTest.log`；ASan、LeakSanitizer、UBSan、
`runtime error`、Sanitizer `ERROR/SUMMARY` 诊断数均为 0。构建观测最高温度 67.2°C，全程
`get_throttled=0x0`。结束后无 Python 字节码和遗留项目进程。完整证据见同目录下的
`P3-S7-ARM64-T01_CP7_asan_ubsan_validation.md`。

```text
CP7_ASAN_UBSAN=PASS
CP7_ASAN_UBSAN_CTEST=PASS_191_OF_191
CP7_ASAN_UBSAN_DIAGNOSTICS=PASS_ZERO_FINDINGS
arm64_native_build_pass=false
```

## 17. CP7 ARM64 clang-tidy 执行结果

同一候选提交已在独立 `clang-tidy-e62de0e94682/` 目录完成 clang++-18 和 clang-tidy-18 静态
分析构建。系统只提供版本化工具名，因此通过项目已有缓存变量显式设置
`GATEWAY_CLANG_TIDY_EXECUTABLE=/usr/bin/clang-tidy-18`，没有修改源码或系统全局环境。

构建共输出 17 条 warning、0 条 error；warning 分布在三个未由候选提交修改的既有 C++ 文件中，
归类为既有非阻塞技术债。构建产物完整 CTest 为 191/191 PASS，退出码 0，用时 180.17 秒。

本门最初以 `-j4` 运行，温度连续超过 70°C 后按当时有效规则受控中断 Ninja，并以 `-j2` 完成
剩余 18 个增量动作。用户随后更新后续约束：70～79°C 只记录 WARN 并保持 `-j4`，仅在 80°C、
当前欠压/降频、OOM 或其他硬性条件出现时中止新构建。完整证据见同目录下的
`P3-S7-ARM64-T01_CP7_clang_tidy_validation.md`。

```text
CP7_CLANG_TIDY=PASS_WITH_EXISTING_WARNINGS
CP7_CLANG_TIDY_ERRORS=0
CP7_CLANG_TIDY_WARNINGS=17_EXISTING_NON_BLOCKING
CP7_CLANG_TIDY_CTEST=PASS_191_OF_191
arm64_native_build_pass=false
```

## 18. CP7 clang-format、Python 与数据文件执行结果

已对冻结源码中的 83 个 `.hpp`/`.cpp` 文件执行 `clang-format-18 --dry-run --Werror`，全部
通过；对 35 个 Python 文件执行 `compileall -q`，全部通过。

JSON、YAML 和 profile 检查复用仓库已有 Release CTest，覆盖 RuntimeConfig、PTY 配置、MQTT
payload、故障配置、release contract/manifest 和 soak profile/summary，共 20/20 PASS，用时
2.05 秒。compileall 生成的 8 个 `__pycache__` 目录和 35 个 `.pyc` 已在逐个验证路径后删除，
最终缓存数为 0。完整证据见同目录下的
`P3-S7-ARM64-T01_CP7_format_python_data_validation.md`。

```text
CP7_CLANG_FORMAT=PASS_83_OF_83
CP7_PYTHON_COMPILEALL=PASS_35_OF_35
CP7_DATA_CONTRACT_TESTS=PASS_20_OF_20
CP7_FORMAT_PYTHON_DATA=PASS
arm64_native_build_pass=false
```

## 19. CP7 ARM64 TSan 条件门执行结果

首次执行时，Clang 18 resource directory 中没有 AArch64 TSan runtime；最小原子线程探针在链接
阶段明确缺少 `libclang_rt.tsan-aarch64.a` 和 `libclang_rt.tsan_cxx-aarch64.a`，因此按合同记录
`NOT_RUN_TOOLCHAIN_BLOCKED`，没有伪造 PASS，也没有把工具链缺失解释为项目数据竞争。

用户随后自行安装 `libclang-rt-18-dev:arm64=1:18.1.3-1ubuntu1`。复验确认运行库文件存在，安全
原子线程探针正常退出 0；故意数据竞争探针被 TSan 捕获，输出 warning/summary 并按约定退出 66，
说明正、反测试预言机均有效。

项目随后在独立 `tsan-e62de0e94682/` 目录完成 clang++-18、MQTT ON、PTY ON、TSan ON、
WSL workaround OFF、warnings-as-errors ON 的 Debug 配置和 `-j4` 原生构建，85/85 个 Ninja 动作
成功。并发相关定向测试 24/24 PASS；单并行完整 CTest 191/191 PASS，退出码 0，用时 200.07 秒。
完整输出和 `LastTest.log` 的项目 TSan 诊断均为 0，后者含 191 个 `Test Passed.` 且没有
`LastTestsFailed.log`。完整证据见同目录下的
`P3-S7-ARM64-T01_CP7_tsan_conditional_validation.md`。

```text
CP7_TSAN_INITIAL_RESULT=NOT_RUN_TOOLCHAIN_BLOCKED
CP7_TSAN_RUNTIME_PRESENT=true
CP7_TSAN_SAFE_PROBE=PASS
CP7_TSAN_INTENTIONAL_RACE_ORACLE=PASS
CP7_TSAN_PROJECT_BUILD=PASS
CP7_TSAN_TARGETED_CTEST=PASS_24_OF_24
CP7_TSAN_FULL_CTEST=PASS_191_OF_191
CP7_TSAN_DIAGNOSTICS=PASS_ZERO_PROJECT_FINDINGS
CP7_TSAN=PASS
TSAN_GATE_EQUIVALENT_TO_X86_64=true
arm64_native_build_pass=false
```

等价范围仅限 TSan 条件门；ARM64 原生构建总门仍须由 CP8 的二进制、动态依赖、安装暂存和证据
汇总共同关闭，systemd 与真实 RS485/CAN 硬件也仍未在本工作块验证。

## 20. CP8 AArch64 二进制、安装暂存、证据与清理结果

正式 run `20260814T171411Z_arm64_native_e62de0e_003` 已完成 AArch64 ELF、动态依赖、唯一
`DESTDIR` 安装暂存和 systemd unit 静态解析。四个主要二进制均为 AArch64 ELF，动态依赖无
`not found`；安装树含 9 个文件，路径和权限全部符合合同，综合哈希为
`7403eecc6db54a80e4eb0c6e77b7e18b0c919180712980cb5aec04b4b5f6dbf1`，unit 静态解析退出码为 0。

正式 `failures.json` 为 `[]`，31 个 evidence 文件全部通过 `sha256sum -c`。Release handoff 共
150 个文件、24,855,918 字节，源构建树与 handoff 树的综合哈希均为
`a26972652e2567536978b3da8bfae7c29c1d1ec74212e72c966250c9062ae7fd`。

已按精确绝对路径删除六个可重建质量门目录、两个探索 run、三个唯一 stage 和本轮临时文件；保留
正式 evidence、冻结候选源码、Release 原构建和 Release handoff。没有遗留项目进程或 Python
字节码。真实 `/usr/local`、`/etc` 和 systemd 状态未写入。最终温度 47.2°C、
`get_throttled=0x0`。

```text
PASS_P3_S7_ARM64_T01_ENVIRONMENT_AND_NATIVE_BUILD
arm64_environment_eligible=true
arm64_native_build_pass=true
arm64_software_integration_validated=false
arm64_systemd_validated=false
arm64_release_bundle_ready=false
hardware_validated=false
```

完整验收报告、去敏构建矩阵和教程分别见：

- `p3_s7_arm64_t01_validation.md`；
- `p3_s7_arm64_t01_build_matrix.json`；
- `p3_s7_arm64_t01_树莓派原生构建与平台验证.md`。

## 21. 下一步

CP-D 已审核通过，正式维护版本已同步到独立仓库。下一工作块需单独制定并审核实施方案；当前不执行
Git 写操作，也不自动开始 ARM64 systemd、长稳或真实 RS485/CAN 硬件验收。
