# skroot-zygisk

> 在 SKRoot Pro 上实现 Zygisk 能力，进而支持 LSPosed / Xposed 模块的移植工程。

## ⚠️ 安全警告（必读）

本项目操作的是**内核级、能动到 zygote/fork 路径的能力**，操作不当**会导致设备无法开机（变砖）**。

- 所有改动前，先在**测试机 / 锁 BL 可救砖**的设备上验证。
- 每次改动只做**一个可控的步骤**，验证通过再下一步。
- 禁止在主力机 / 无救砖手段的设备上直接实机调试。
- 当前已有可安装、可运行 Magisk 模块的代码，且 run service.sh 会触碰 zygote 路径，务必谨慎。

## 项目状态

**阶段：模块管理兼容层已基本闭环；zygote 注入层结论为「SKRoot 容器不支持」。**

已完成可编译、可运行的模块代码（`src/testModule/module_zygisk_compat/`），真机验证闭环：

- **安装**：上传 zip → 解压 → 落盘 `/data/adb/modules/<id>/` → 权限 → source 官方 util_functions.sh → customize.sh，完整跑通。
- **列表**：扫描 `/data/adb/modules/` 展示 id/name/version/author/description，标记 `hasWebui`。
- **卸载**：支持任意目录名（含括号等特殊字符），修复了 query URL decode 与校验过严两个缺陷。
- **通用模块 WebUI 代理**：任意带 `webroot/` 的模块可点开自己的第二个界面；对 `.html` 注入 `window.ksu.exec` 兼容桥，并重写模块 index.html 里的根绝对路径到模块子路径。
- **ksu.exec 兼容桥**：后端 `/ksuExec` + 前端 `ksu-bridge.js`，对齐 KernelSU 三参回调契约。

### ⚠️ zygote 注入的结论（重要）

经真机逐层排查（含 bugreport 分析），确认 **SKRoot Pro 的 root 是 `u:r:shell:s0` 域的容器 root（提权核 `supervisor` 同域），无法 ptrace 到宿主机的 `u:r:zygote:s0` 域 zygote 进程**。因此：

- Zygisk-Next 的 `zygiskd` 注入 zygote **必然失败**（`connect daemon failed` → `--suicide` 循环 → tombstone 堆积 → 前端 `Could not connect to service!`）。
- 即便 `setenforce 0` 进 Permissive，也仅推进到「注入最后一步失败」（`.magic` 已写出、报 `Last injection failed`），根因不变。

结论：**本工程可作为 SKRoot 容器内的「Magisk 模块管理框架」交付（安装/列表/卸载/WebUI/命令桥），但 hook 注入能力需真机 Magisk/KernelSU/APatch。** 详见 `docs/device-probe.md` 与 `docs/roadmap.md`。
## 目标

1. 摸清 SKRoot Pro 的模块加载机制与 SDK 接口（基于实机，非假设）。
2. 在 SKRoot Pro 上实现 Zygisk 等价能力（进程注入 + Zygisk API）。
3. 使 LSPosed / Xposed 模块可以在 SKRoot Pro 上运行。

## 目录结构

```
skroot-zygisk/
├── README.md            # 本文件
├── LICENSE              # 许可证（GPL-3.0 已确定）
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
