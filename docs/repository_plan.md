# industrial_iot_gateway 仓库结构与构建/测试方案

## 1. 文档状态

| 字段             | 内容                                           |
| ---------------- | ---------------------------------------------- |
| 项目             | 项目三：工业通信网关                           |
| 任务编号         | `P3-S1-T01`                                    |
| 文档状态         | 已固化（S1 验收通过）                          |
| 文档更新时间     | 2026-08-08                                     |
| 远程仓库名称     | `Industrial-IOT-Gateway`                       |
| 本地仓库目录     | `industrial_iot_gateway`                       |
| 仓库建立状态     | 已建立并完成只读准入核验                       |
| 仓库内容核验状态 | 有效空 Git 仓库，仅含 `.git/`                  |
| 构建状态         | 未开始，属于 `P3-S2-T01`                       |
| 测试状态         | 未开始，属于 `P3-S2-T01`                       |
| 当前阶段产物     | `2026-07-25至2026-07-28_工作任务产物/repository_plan.md` |
| 后续实施入口     | `P3-S2-T01`：增量建立 CMake/测试骨架           |

GitHub 远程仓库固定为 `Industrial-IOT-Gateway`；本地目录、CMake 工程标识和 C++ 顶层命名空间固定为 `industrial_iot_gateway`。本地工程标识采用下划线形式，与项目一 `nav2_apf_benchmark` 的命名方式保持一致。

“仓库已经建立”只证明仓库载体存在，不自动证明以下事项已经完成：

- CMake 工程已经建立；
- 依赖已经安装；
- 源码、测试或 CI 已经实现；
- 构建、测试或部署已经通过。

2026-08-08 的只读核验已确认本地 Git 根目录、`origin`、当前 `main` 分支和空仓库状态；远程可见性尚未核验，不影响本工作块的规划验收。构建、测试和部署状态仍必须在后续阶段通过实际文件、命令输出和测试证据分别确认。

## 2. 本文档的位置与生命周期

### 2.1 S1 规划基线

审核通过后，本文档首先固化在项目三规划目录：

```text
F:\5.GitHub_Code\autumn_recruitment\0.秋招资料\
└── 4.个人项目规划/
    └── Codex/
        └── 3.项目三：工业通信网关/
            └── 2026-07-25至2026-07-28_工作任务产物/
                └── repository_plan.md
```

`2026-07-25至2026-07-28_工作任务产物/` 统一保存 S1 的五项合同工件。该文件作为 `P3-S1-T01` 的规划基线，保留当时批准的仓库、构建和测试设计。

### 2.2 独立仓库内的维护版本

现有独立仓库完成准入核验后，将批准版本复制到：

```text
industrial_iot_gateway/
└── docs/
    └── repository_plan.md
```

两份文件的职责不同：

- 规划目录版本：记录 S1 审核批准的基线，不随日常实现任意改写；
- 独立仓库版本：用于后续实现维护，变更时记录实际原因和验证结果。

复制文档不代表构建或测试完成。

## 3. 本工作块的范围

### 3.1 本阶段执行内容

1. 确认仓库及工程命名规则。
2. 记录仓库已经建立这一当前事实。
3. 冻结仓库目录结构及目录职责。
4. 冻结主要 CMake 构建目标及依赖方向。
5. 冻结 Debug、Sanitizer 和 Release 构建方案。
6. 冻结单元测试、PTY 集成测试、系统测试和硬件测试边界。
7. 冻结格式、静态检查和 sanitizer 质量门。
8. 冻结第三方依赖的用途、版本及许可证管理规则。
9. 冻结源码、配置、测试、原始证据和公开报告的隔离规则。
10. 为 `P3-S2-T01` 提供可直接实施的输入。

### 3.2 本阶段不执行内容

本工作块不重复执行或擅自执行：

- `git init`；
- 重新创建远程仓库；
- 删除或覆盖现有仓库内容；
- 修改 `origin`；
- 修改仓库可见性；
- 切换默认分支；
- 安装编译器或第三方依赖；
- 创建实际 CMake 骨架；
- 编译或运行测试；
- 创建 Git 提交、标签或 Release；
- 推送到远程仓库；
- 接入 USB-RS485、商用从站或 STM32。

现有仓库的一切内容均应先只读盘点，再决定如何实施 S2。

## 4. 命名规则

### 4.1 固定名称

| 对象             | 固定名称                             |
| ---------------- | ------------------------------------ |
| GitHub 仓库      | `Industrial-IOT-Gateway`             |
| 本地仓库目录     | `industrial_iot_gateway/`            |
| WSL2 建议路径    | `~/projects/industrial_iot_gateway/` |
| 树莓派建议路径   | `~/projects/industrial_iot_gateway/` |
| CMake 工程标识   | `industrial_iot_gateway`             |
| C++ 顶层命名空间 | `industrial_iot_gateway`             |
| 核心库目标       | `gateway_core`                       |
| MQTT 适配目标    | `gateway_mqtt`                       |
| 主程序目标       | `gateway_app`                        |
| 主程序输出文件   | `industrial_iot_gateway`             |
| PTY 模拟器目标   | `pty_slave`                          |
| systemd 服务     | `industrial_iot_gateway.service`     |

### 4.2 命名边界

远程仓库名称与本地工程标识按对象分别固定，不要求字符形式完全相同：

```text
GitHub 远程仓库：Industrial-IOT-Gateway
本地/CMake/C++：industrial_iot_gateway
作品集 URL slug：industrial-iot-gateway
```

其中：

- `industrial_comm_gateway` 是旧规划名称，应被替换；
- `Industrial-IOT-Gateway` 是已确认的 GitHub 远程仓库名称；
- `industrial_iot_gateway` 是本地目录、CMake 工程和 C++ 命名空间；
- `industrial-iot-gateway` 仅可作为作品集网页等面向浏览器的 slug，例如 `/industrial-iot-gateway/`；
- 不得因为仓库改名而机械修改所有网页路由或自然语言项目名称。

## 5. 现有仓库准入核验

由于仓库已经建立，S2 不再从“创建空仓库”开始，而应先完成只读准入核验。

### 5.1 需要确认的仓库身份

| 项目           | 当前状态                                                                       |
| -------------- | ------------------------------------------------------------------------------ |
| 远程仓库名称   | `Industrial-IOT-Gateway`                                                       |
| GitHub 所有者  | `fengchechecheche`                                                             |
| 远程仓库 URL   | `git@github.com:fengchechecheche/Industrial-IOT-Gateway.git`                  |
| 远程可访问性   | 已通过只读 `git ls-remote` 验证；仓库为空，尚无远程 `HEAD`                    |
| 仓库可见性     | 本轮未核验，不作为 S1 规划验收门                                              |
| 本地克隆路径   | `/home/iot-gw/projects/industrial_iot_gateway`                                |
| 当前分支       | `main`，尚无提交                                                              |
| 现有文件       | 仅 `.git/`，尚无项目文件                                                      |
| 工作区是否干净 | 是；空工作区，无未跟踪项目文件                                                |
| 是否已有提交   | 否                                                                            |
| 是否已有 CI    | 否                                                                            |

### 5.2 已执行的只读核验命令

2026-08-08 已在实际仓库中执行：

```bash
git rev-parse --show-toplevel
git remote -v
git branch --show-current
git status --short
git log -1 --oneline
find . -maxdepth 3 -type f | sort
git ls-remote --symref origin HEAD
```

其中 `git log -1` 因 `main` 尚无提交而返回预期失败，其余身份、状态和远程访问检查与“有效空仓库”一致。这些命令只用于确认状态，不代表允许修改远程仓库、提交或推送。

如果仓库已有用户文件，后续骨架必须在盘点基础上增量建立，不得覆盖或重建。

## 6. 规划的仓库结构

```text
industrial_iot_gateway/
├── CMakeLists.txt
├── cmake/
│   ├── CompilerWarnings.cmake
│   ├── Sanitizers.cmake
│   └── StaticAnalysis.cmake
├── src/
│   ├── protocol/                 # CRC、RTU 编解码和流式组帧
│   ├── transport/                # termios、非阻塞串口和 fd 生命周期
│   ├── scheduler/                # 多从站轮询、超时、重试和退避
│   ├── gateway/                  # 数据转换、质量和新鲜度
│   ├── mqtt/                     # MQTT 发布、重连和有界缓存
│   └── app/                      # 配置装配、信号处理和程序入口
├── include/
│   └── industrial_iot_gateway/
│       ├── protocol/
│       ├── transport/
│       ├── scheduler/
│       ├── gateway/
│       └── mqtt/
├── config/
│   ├── examples/                 # 可公开的虚构设备配置
│   └── schemas/                  # 配置结构及字段约束
├── tests/
│   ├── unit/                     # 不依赖串口、网络和硬件
│   ├── integration/              # PTY、本地 broker 和进程级测试
│   └── data/                     # 黄金帧、坏帧和固定测试向量
├── tools/
│   └── pty_slave/                # PTY 从站模拟器和故障开关
├── packaging/
│   └── systemd/
│       └── industrial_iot_gateway.service
├── docs/
│   ├── repository_plan.md
│   ├── environment.md
│   └── third_party_inventory.md
├── artifacts/
│   ├── README.md                 # 证据结构和公开边界
│   ├── baseline/                 # 原始运行证据，默认忽略
│   └── reports/                  # 经整理和去敏的报告
├── .github/
│   └── workflows/
│       └── ci.yml
├── .clang-format
├── .clang-tidy
├── .gitignore
├── LICENSE
└── README.md
```

此目录树是目标结构，不证明当前仓库已经具备这些文件。

S2 应先把现有文件与目标结构进行差异对照，再输出实际创建清单。

## 7. 目录职责与依赖边界

| 模块        | 主要职责                                           | 禁止承担的职责                  |
| ----------- | -------------------------------------------------- | ------------------------------- |
| `protocol`  | CRC、RTU ADU/PDU、功能码编解码、异常响应和流式组帧 | 不打开串口，不重试，不发布 MQTT |
| `transport` | `termios`、非阻塞读写、等待和 fd 生命周期          | 不解释工程量，不决定轮询策略    |
| `scheduler` | 请求排队、多从站轮询、deadline、有限重试和退避     | 不直接持有 MQTT 连接            |
| `gateway`   | 寄存器转换、质量标志、新鲜度和设备快照             | 不直接读写串口                  |
| `mqtt`      | topic/payload、异步发布、重连和有界缓存            | 不阻塞串口轮询，不修改 RTU 帧   |
| `app`       | 加载配置、装配模块、处理信号和控制生命周期         | 不放置可复用协议算法            |
| `pty_slave` | 软件从站、寄存器表和故障注入                       | 不作为真实 RS485 硬件证据       |

计划依赖方向：

```text
protocol
   ↑
transport
   ↑
scheduler
   ↑
gateway
   ↑
mqtt

app 负责组装上述模块
tests 只依赖对应被测目标
pty_slave 与主程序保持独立
```

应避免循环依赖。第三方 MQTT、YAML、JSON 和日志类型不得向 `protocol` 的公开接口泄漏。

## 8. 构建方案

### 8.1 基础工具链

- 语言标准：C++17；
- 构建系统：CMake；
- 测试入口：CTest；
- 测试框架：GoogleTest；
- 默认开发环境：`Ubuntu-24.04-Gateway` WSL2，x86_64；
- 目标部署环境：Ubuntu Server 24.04 LTS ARM64，树莓派 4B；
- 首选编译器：GCC；
- 补充质量检查编译器：Clang；
- 首选生成器：Ninja；
- Ninja 不可用时允许使用 Unix Makefiles；
- 不在 `/mnt/c/` 或 `/mnt/f/` 内执行高频构建；
- CMake 最低版本候选：`3.22`。

最低版本必须在 S2 使用实际工具链验证，不能只依据候选文档宣称兼容。

### 8.2 构建目标

| CMake 目标     | 类型              | 内容                                              |
| -------------- | ----------------- | ------------------------------------------------- |
| `gateway_core` | 静态库            | `protocol`、`transport`、`scheduler` 和 `gateway` |
| `gateway_mqtt` | 静态库            | MQTT 发布、重连和缓存                             |
| `gateway_app`  | 可执行程序        | 配置装配、信号处理和主入口                        |
| `pty_slave`    | 可执行程序        | PTY Modbus 从站模拟器                             |
| 单元测试目标   | 测试程序          | 按模块注册到 CTest                                |
| 集成测试目标   | 测试程序或 runner | PTY、进程、故障和恢复测试                         |

`gateway_core` 不直接依赖 Paho MQTT，使协议和串口核心能够独立构建和测试。

### 8.3 CMake 选项

后续骨架至少提供：

```text
GATEWAY_BUILD_TESTS
GATEWAY_BUILD_PTY_SLAVE
GATEWAY_ENABLE_MQTT
GATEWAY_ENABLE_WARNINGS_AS_ERRORS
GATEWAY_ENABLE_CLANG_TIDY
GATEWAY_ENABLE_SANITIZERS
GATEWAY_ENABLE_TSAN
```

规则：

- 测试默认在开发构建中启用；
- ASan 与 UBSan 可组合启用；
- TSan 只在线程组件和第三方依赖兼容时启用；
- sanitizer 构建不得作为发布二进制；
- 警告视为错误只应用于项目自有目标；
- 所有构建均使用源码外构建目录。

### 8.4 规划的构建配置

| 配置      | 用途             | 必须检查                    |
| --------- | ---------------- | --------------------------- |
| Debug     | 日常开发         | 编译警告、单元测试          |
| Sanitizer | 缺陷检查         | ASan、UBSan、CTest          |
| Release   | 发布和树莓派部署 | 完整测试、无 sanitizer 依赖 |
| TSan      | 条件执行         | 仅在线程兼容性确认后启用    |

### 8.5 后续标准命令

以下命令是 S2 的预定入口，本阶段不执行：

```bash
cmake -S . -B build/debug \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGATEWAY_BUILD_TESTS=ON

cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

Sanitizer 入口：

```bash
cmake -S . -B build/sanitizer \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGATEWAY_BUILD_TESTS=ON \
  -DGATEWAY_ENABLE_SANITIZERS=ON

cmake --build build/sanitizer
ctest --test-dir build/sanitizer --output-on-failure
```

Release 入口：

```bash
cmake -S . -B build/release \
  -DCMAKE_BUILD_TYPE=Release \
  -DGATEWAY_BUILD_TESTS=ON

cmake --build build/release
ctest --test-dir build/release --output-on-failure
```

## 9. 测试方案

### 9.1 测试分层

| 层级              | 环境               | 主要内容                            | 软件 MVP 门  |
| ----------------- | ------------------ | ----------------------------------- | ------------ |
| 单元测试          | WSL2/ARM64         | CRC、编解码、parser、边界和数据语义 | 是           |
| PTY 集成测试      | WSL2/ARM64         | 多从站、超时、重试、坏帧和恢复      | 是           |
| MQTT 集成测试     | 本地 broker        | 发布、断线、缓存、重连和旧数据      | 是           |
| 进程/systemd 测试 | Linux              | 启停、SIGTERM、重启和错误配置       | 是           |
| 8 小时软件长稳    | Linux + PTY        | 资源、延迟、队列和恢复              | 是           |
| 商用从站测试      | 树莓派 + USB-RS485 | 真实串口、拔插、断电和恢复          | 否，硬件增强 |
| STM32 从站测试    | 树莓派 + STM32     | 自研从站、故障注入和复位            | 否，条件执行 |

PTY 结果只能证明用户态软件协议和故障逻辑，不得作为真实 RS485 电气验证结果。

### 9.2 单元测试范围

至少覆盖：

- CRC 标准向量；
- 空输入、单字节、最大允许帧和一位翻转；
- `0x03`、`0x04`、`0x06` 请求和响应；
- Modbus 异常响应；
- 非法从站地址、功能码、长度、寄存器范围和值；
- 半帧、粘连帧、噪声前缀、坏 CRC 和超长帧；
- 工程量缩放、偏移、单位和访问属性；
- fresh、stale、offline 和 invalid 等质量状态；
- 队列边界、有限重试和 deadline。

单元测试不得依赖真实串口、外部网络或测试执行顺序。

### 9.3 集成测试范围

PTY 集成测试至少覆盖：

- 一个主站轮询至少三个模拟从站；
- 正常、延迟、静默、坏 CRC 和截断响应；
- Modbus 异常码；
- 从站离线和恢复；
- 串口断开和重新连接；
- 请求队列或发布队列满；
- `SIGINT`、`SIGTERM` 和有界退出。

MQTT 集成测试使用本地 broker 和虚构设备 ID，不依赖公网或生产凭据。

### 9.4 CTest 标签

```text
unit
integration
pty
mqtt
system
hardware
soak
```

默认 CI 只执行不依赖硬件的测试。

`hardware` 和 `soak` 必须显式启动，不能混入普通单元测试。每项集成测试必须设置超时并返回明确退出状态。

## 10. 代码质量门

计划采用：

- `clang-format`；
- `clang-tidy`；
- `-Wall -Wextra -Wpedantic`；
- ASan；
- UBSan；
- 条件执行的 TSan。

进入 S3 前，S2 至少应满足：

1. Debug 构建成功；
2. 所有已实现单元测试通过；
3. ASan/UBSan 无未解释报告；
4. PTY 基础集成测试通过；
5. 格式检查无差异；
6. 无被静默忽略的编译警告；
7. 失败测试返回非零退出状态；
8. 测试日志和摘要能够定位到具体版本。

PTY 不可用时，集成验收保持未通过，不能使用内存流测试替代。

## 11. 第三方依赖方案

| 依赖                  | 计划用途                      | 引入边界                 |
| --------------------- | ----------------------------- | ------------------------ |
| GoogleTest            | 单元和集成测试                | 仅测试目标               |
| `yaml-cpp`            | 从站、寄存器和轮询配置        | 不进入协议层公开接口     |
| `nlohmann/json`       | JSON 事件、摘要和网络 payload | 不承担日志持久化         |
| `spdlog`              | 运行日志                      | 不记录凭据和私人地址     |
| Eclipse Paho MQTT C++ | MQTT/TLS、重连和发布          | 只由 `gateway_mqtt` 使用 |
| `socat`               | PTY 链路辅助                  | 测试环境工具             |
| PyModbus              | 软件从站和互操作对照          | 项目专用 Python 虚拟环境 |
| `libmodbus`           | RTU 行为测试预言机            | 不替代自行实现的协议核心 |

版本和许可证规则：

1. 不跟随未固定的 `main`、`master` 或 latest。
2. 每项依赖记录明确版本、tag 或 commit。
3. 在 `docs/third_party_inventory.md` 中记录来源、许可证、用途、链接方式和修改内容。
4. 测试依赖如使用 `FetchContent`，必须固定版本。
5. 许可证检查完成前，不复制第三方源码。
6. 项目核心证据必须来自自行实现的协议、串口、调度、数据质量和网络适配。
7. 项目自有源码许可证候选为 MIT，公开前必须由用户确认。

## 12. 配置、安全与证据隔离

### 12.1 配置规则

- `config/examples/` 只保存虚构设备和本地 broker 示例；
- 真实设备 ID、串口序列号、私人地址和凭据不得进入公开示例；
- MQTT 密码、TLS 私钥和 Wi-Fi 密码不得提交；
- 本地敏感配置通过独立文件或环境变量提供；
- 配置结构变化必须同步更新 schema 和示例。

### 12.2 构建产物

以下内容不得提交：

```text
build/
compile_commands.json
CMakeCache.txt
CMakeFiles/
Testing/
```

### 12.3 证据分层

计划采用：

```text
artifacts/
├── baseline/
│   └── <run_id>/                # 原始配置、日志和结构化结果
└── reports/
    └── <stage_or_release>/       # 去敏后的公开候选报告
```

原始运行证据默认位于被 `.gitignore` 排除的 `artifacts/baseline/`。

经审核允许公开的派生报告进入 `artifacts/reports/`。公开报告应：

- 不包含凭据、私人地址和设备唯一标识；
- 能追溯对应测试版本和配置；
- 不把计划值写成实测结果；
- 不用短时或单帧结果替代长期验收；
- 内容改变后重新生成并验证校验和清单。

失败实验与成功实验采用相同证据结构，不得只保留成功结果。

## 13. 开发、CI 与部署环境

| 环境                        | 职责                                            |
| --------------------------- | ----------------------------------------------- |
| `Ubuntu-24.04-Gateway` WSL2 | 日常开发、单元测试、PTY、故障注入和网络接口测试 |
| Ubuntu Server 24.04 ARM64   | 树莓派原生构建、systemd、USB-RS485 和硬件验收   |
| Windows                     | Git 管理、VS Code Remote 和第三方 GUI 对照工具  |
| `Ubuntu-24.04-ROS2`         | 项目一专用，不安装项目三依赖                    |
| 干净 `Ubuntu-24.04`         | 可重复环境基线，不直接开展项目                  |

WSL2 和树莓派必须分别原生构建，不能把 x86_64 二进制文件复制到树莓派并宣称 ARM64 构建成功。

未来 CI 至少计划包含：

1. GCC Debug 构建和单元测试；
2. Clang ASan/UBSan 构建和测试；
3. `clang-format` 检查；
4. `clang-tidy` 检查；
5. PTY 软件集成测试；
6. 失败时保存去敏后的测试摘要。

普通云端 CI 不承担 USB-RS485、STM32、systemd 开机启动或 8 小时长稳验收。

## 14. 向 S2 的移交条件

审核固化后，向 `P3-S2-T01` 移交：

- 已确认的远程仓库名称 `Industrial-IOT-Gateway` 及本地目录 `industrial_iot_gateway`；
- 现有仓库准入核验清单；
- 目标目录结构；
- CMake 构建目标列表；
- CMake 选项和构建配置；
- 单元与集成测试目录计划；
- 质量工具和阻塞规则；
- 第三方依赖与许可证规则；
- 原始证据和公开报告的隔离规则。

S2 的执行顺序固定为：

```text
只读核验现有仓库
        ↓
盘点现有文件和 Git 状态
        ↓
对照目标结构生成增量实施清单
        ↓
建立最小 CMake/CTest 骨架
        ↓
执行 Debug 和 Sanitizer 验证
        ↓
保存实际结果并更新状态
```

仓库已建立不等于 S2 已完成；只有实际骨架、命令和测试证据满足门槛后，才能更新对应任务状态。

## 15. 本文档验收清单

- [x] 已固定远程仓库 `Industrial-IOT-Gateway` 与本地工程 `industrial_iot_gateway` 的命名映射
- [x] 已记录仓库建立事实及其证据边界
- [x] 未把仓库存在误写成构建或测试完成
- [x] CMake 工程和 C++ 命名空间统一为 `industrial_iot_gateway`
- [x] 目录职责明确且无明显重叠
- [x] 核心库、MQTT、主程序和 PTY 目标边界明确
- [x] Debug、Sanitizer 和 Release 方案明确
- [x] 单元、PTY、MQTT、系统和硬件测试边界明确
- [x] 第三方依赖及许可证规则明确
- [x] 原始证据与公开报告相互隔离
- [x] WSL2 与树莓派验收边界明确
- [x] 本次固化未修改现有仓库、远程或 Git 历史
- [x] 审核者已确认远程仓库最终名称并授权修复阻塞项

以上内容已通过审核，本文档自 2026-08-08 起固化为 `P3-S1-T01` 基线。该结论只表示“仓库结构与构建/测试方案”工作块通过，不表示 `P3-S2-T01` 的 CMake、实现或测试已经开始或通过。
