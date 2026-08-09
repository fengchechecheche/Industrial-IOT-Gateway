# P3-S2-T05 可公开证据摘要

- 基线提交：`b74bd2468f3769668ebd69bc5ed8a97453be8c46`
- 环境：`Ubuntu-24.04-Gateway`，x86_64
- 模拟器：`build/<config>/bin/pty_slave`
- 寄存器表 SHA256：`b7410b4e1399007d7744714fdbf1f4e5ae7a51d81651057d95641c97327dc323`
- 场景：normal、exception、delay、silent、bad-crc、truncated
- Debug / Sanitizer / Release / clang-tidy：均为 44/44
- `ctest -L integration`：6/6
- `ctest -L pty`：1/1 聚合门，内部执行 6 个真实 PTY 场景
- ASan/UBSan：无报告
- clang-tidy：零告警

本目录只保存可公开、可复现的摘要。构建目录和临时 `/dev/pts/*` 路径不作为稳定证据，
PTY 结果不代表 UART、USB-RS485、总线偏置、终端电阻、方向控制或其他电气层已经验证。
