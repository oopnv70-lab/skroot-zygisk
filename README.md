# skroot-zygisk

> 在 SKRoot Pro 上实现 Zygisk 能力，进而支持 LSPosed / Xposed 模块的移植工程。

## ⚠️ 安全警告（必读）

本项目操作的是**内核级、能动到 zygote/fork 路径的能力**，操作不当**会导致设备无法开机（变砖）**。

- 所有改动前，先在**测试机 / 锁 BL 可救砖**的设备上验证。
- 每次改动只做**一个可控的步骤**，验证通过再下一步。
- 禁止在主力机 / 无救砖手段的设备上直接实机调试。
- 本项目当前为**空壳阶段**，尚无任何可运行代码。

## 项目状态

**阶段：0 — 空壳搭建 / 可行性验证**

尚未有任何可编译、可运行的模块代码。当前只有仓库骨架与设备实测记录。

## 目标

1. 摸清 SKRoot Pro 的模块加载机制与 SDK 接口（基于实机，非假设）。
2. 在 SKRoot Pro 上实现 Zygisk 等价能力（进程注入 + Zygisk API）。
3. 使 LSPosed / Xposed 模块可以在 SKRoot Pro 上运行。

## 目录结构

```
skroot-zygisk/
├── README.md            # 本文件
├── LICENSE              # 许可证（待定）
├── docs/
│   ├── device-probe.md  # 设备实测记录（所有结论的依据）
│   ├── architecture.md  # 架构设计与方案对比（待补）
│   └── roadmap.md       # 阶段计划
├── module/              # SKRoot 模块本体（待开发）
├── sdk/                 # SKRoot SDK 头文件（待拉取）
└── reference/           # 参考资料（旧版 GPL 源码、LSPosed 等）
```

## 当前已知（来自实机探测）

- 内核：`6.1.141-android14-11`，aarch64，Android 16（SDK 36）
- 设备品牌：OPPO / 一加（`com.oplus.pantanal.ums`）
- root 方式：`/system/bin/su`（SKRoot Pro 自带），`context=u:r:shell:s0`，SELinux Enforcing
- `/data/adb/` 为非标准结构（非 Magisk/KernelSU/APatch）

详见 `docs/device-probe.md`。

## 许可证
本项目采用 **GNU General Public License v3.0（GPL-3.0）**，全文见 [LICENSE](LICENSE)。

原因：本项目收录并分发 GPL-3.0 代码（Magisk 的 `util_functions.sh`、KernelSU 的 WebUI 桥等），
按 GPL 条款，衍生作品须以 GPL-3.0 发布。第三方代码来源与许可详见 [NOTICE](NOTICE)。
