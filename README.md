# skroot-zygisk

> 在 SKRoot Pro 上实现 Magisk 模块兼容 + 进程注入能力，进而支持 Zygisk-Next / LSPosed 模块的移植工程。

---

## 免责声明（Disclaimer）

本项目仅用于**安全研究、逆向学习与技术验证**，请勿用于任何违法违规用途。

- 本项目操作的是**内核级、能动到进程注入 / ptrace / sepolicy / SELinux 的能力**，操作不当**可能导致设备异常、启动故障，甚至永久变砖**。
- **使用者须自行承担全部风险**。作者不对因使用本项目而造成的设备损坏、数据丢失、账号封禁（含银行等敏感 App 检测风险）、法律责任承担任何责任。
- 请在**测试机 / 已解锁 Bootloader 可救砖**的设备上验证，**禁止在主力机或无救砖手段的设备上直接实机调试**。
- 涉及 `setenforce 0`、`sepolicy.rule`、`inject` 注入等操作后，务必立即恢复 Enforcing，避免设备长期处于安全降级状态。

---

## 许可声明（License）

本项目采用 **GNU General Public License v3.0（GPL-3.0）**，全文见 [LICENSE](LICENSE)。

原因：本项目收录并分发 GPL-3.0 代码（Magisk 的 `util_functions.sh`、KernelSU 的 WebUI 桥等），
按 GPL 条款，衍生作品须以 GPL-3.0 发布。第三方代码来源与许可详见 [NOTICE](NOTICE)。

---

## 目前已知（在哪个设备上）

以下信息全部来自真机实测（详见 `docs/device-probe.md`），非假设。

| 项目 | 值 |
|---|---|
| 设备品牌痕迹 | OPPO / 一加（`com.oplus.pantanal.ums` 进程存在） |
| Android 版本 | 16（SDK 36） |
| 内核版本 | `6.1.141-android14-11-o-g984c12362a16` |
| 架构 | aarch64（arm64-v8a） |
| root 方式 | SKRoot Pro 自带 `/system/bin/su` |
| root 运行上下文 | `u:r:shell:s0`（shell 域，非 magisk/su 域） |
| SELinux 状态 | Enforcing |

关键实测事实：

- **root 是 shell 域的容器 root**：`su -c id` 返回 `uid=0(root)`，但 `context=u:r:shell:s0`。root（uid=0，身份）与 shell 域（权限范围）是两个正交维度，SKRoot 只能从 shell 域入口给 root。
- **内核无 yama ptrace 模块**：`/proc/sys/kernel/yama/ptrace_scope` 不存在（该路径报 No such file）。
- **`/proc/<pid>/mem` 被整体封死**：对几乎所有进程（含 zygote、普通 App、甚至 supervisor 自身）读操作都返回 I/O error，这是厂商防注入层。
- **SKRoot 提权实质是 patch 内核内存**：运行时痕迹含 `first_patch_selinux`、`uid_control_kaddr`、`next_free_kaddr`、`tombstones_max_slot` 等文件，指向直接改写 selinux / uid 控制的内核地址。
- **SKRoot 模块机制**：模块是 `.so`，入口 `skroot_module_main()`，唯一作用是拉起 `service.sh` 后台脚本。supervisor 约在开机后 13s 启动（starttime≈1294 ticks / 100 ticks每s），而 zygote 约 9s 启动（≈903 ticks）——**SKRoot 模块永远晚于 zygote**。

---

## 目录结构

```
skroot-zygisk/
├── README.md            # 本文件
├── LICENSE              # 许可证（GPL-3.0）
├── NOTICE               # 第三方代码来源与许可
├── .github/workflows/   # CI：NDK 构建 + tag 自动发 pre-release
├── docs/
│   ├── device-probe.md  # 设备实测记录（所有结论的依据）
│   ├── architecture.md  # 架构设计与方案对比
│   └── roadmap.md       # 阶段计划
├── src/
│   └── testModule/
│       └── module_zygisk_compat/   # SKZygiskCompat 兼容层源码（C++，产物 libskzygisk_compat.so）
├── module/              # SKRoot 模块本体（待开发）
├── sdk/                 # SKRoot SDK 头文件（待拉取）
└── reference/           # 参考资料（旧版 GPL 源码、LSPosed 等）
```

核心源码在 `src/testModule/module_zygisk_compat/`，产物由 `BUILD_SHARED_LIBRARY`（模块名 `skzygisk_compat`）编译，部署到设备后以 1.5MB 独立可执行形态运行（非 .so 文件名）。

---

## 目前遇到的问题（核心问题）

### zygote 注入走不通（三条硬墙叠加）

想要让 Zygisk 以原生 Magisk 的方式「一次性注入 zygote、所有 App 自动继承 hook」，在这台设备上是死路。三层原因叠加：

1. **SELinux 域隔离**：SKRoot root 在 `u:r:shell:s0`，zygote 在 `u:r:zygote:s0`，sepolicy 无 `shell → zygote` 的 ptrace 规则。
2. **时机不可逆转**：zygote 约 9s 启动并锁定，SKRoot 模块（supervisor）约 13s 才启动，永远落后约 4s。
3. **内核封死内存口**：`/proc/<pid>/mem` 对进程整体返回 I/O error（连 supervisor 自己、连已注入成功的 App 都读不了），直接读写内存这条路被厂商封死。

实测：即便 `setenforce 0` 进 Permissive、`runcon u:r:zygote:s0` 切域，都跨不过第 3 层（读 mem 仍 I/O error）。Zygisk-Next 的 `zygiskd` 因此 `connect daemon failed` → `--suicide` 循环 → tombstone 堆积 → 前端 `Could not connect to service!`。

---

## 工作原理（关键细节）

### 1. SKRoot 的提权模型

SKRoot Pro **不是**普通的 su，而是**内核级提权**——它直接 patch 内核内存改写 SELinux 与 uid 控制逻辑（运行时痕迹 `first_patch_selinux`、`uid_control_kaddr` 等文件）。提权核 `supervisor` 由 init 直接 fork（ppid=1），但它**不在 init.rc 里**，是运行时被拉起（动态注入 init 的 fork 路径）。

### 2. 模块装载链

```
init (t≈0)
  └─ ... 等 /data 分区就绪、找到 modules 里的 .so ...
     └─ supervisor (t≈13s，shell 域)
          └─ fork 出 TEESimulator（Java 守护，app_process + classes.dex）
               └─ 加载模块 .so → 调用 skroot_module_main()
                    └─ system("nohup sh service.sh &")
                         └─ service.sh 启动 inject 注入 / sepolicy 等
```

模块 `.so` 的入口（`module.cpp`）：

```cpp
SKROOT_MODULE_NAME("TEESimulator-RS")
int skroot_module_main(const char* root_key, const char* module_private_dir) {
    // 找到 service.sh，chmod 0755 后 nohup 后台执行
    system("nohup sh service.sh > /dev/null 2>&1 &");
}
```

**关键**：SKRoot 模块没有 init 阶段、没有 post-fs-data 阶段，只有「加载时执行一次 `skroot_module_main()`」的机会，而这次执行就是 supervisor 被拉起的时刻（t≈13s）。

### 3. 进程注入原理（为什么单 App 能通，zygote 不能）

SKRoot 自带注入工具 `inject <pid> <lib_path> <entry_name>`，其注入链路：

```
ptrace ATTACH（attach 目标进程）
  └─ process_vm_readv / process_vm_writev   ← 直接跨进程读写内存，绕开被封的 /proc/<pid>/mem
       └─ 在目标进程里远程调用 dlopen / android_dlopen_ext
            └─ 装载 libxxx.so，执行入口函数（如 zn_entry / zygisk_entry）
```

**为什么单 App 能注入成功**：
- 目标 App 进程在 `untrusted_app` 域，SKRoot 用 `sepolicy.rule` 里的 `allow crash_dump <domain> process *` 通道获得 ptrace 权限；
- `crash_dump` 是 Android 自带、**天生有 ptrace 权限**的进程（用于抓崩溃现场），作者把 inject 挂到这个域上，绕开 shell 域的限制；
- `process_vm_writev` 走内核 syscall，**不依赖被厂商封死的 `/proc/<pid>/mem`**。

实测：将 `/data/adb/modules/zygisksu/lib64/libzn_loader.so`（入口 `zn_entry`）注入天气 App（`com.coloros.weather2`，pid 3137），`inject` 返回 exit=0，`/proc/3137/maps` 出现三段完整映射（r-xp 代码段 / r--p 只读段 / rw-p 读写段），证明装载成功。

**为什么 zygote 注入不可行**：
- zygote 是 init 直接 fork、`u:r:zygote:s0` 域，shell 域无 ptrace 权限；
- `process_vm_writev` 同样需要 ptrace 权限，shell 域→zygote 域被 SELinux 拒绝；
- 即便 `runcon` 切到 zygote 域，`process_vm_writev` 依然被 zygote 的 `PR_SET_DUMPABLE=0`（不可被 attach）挡住。

### 4. SKRoot 作者自己的持久注入方案（现成参考）

作者在 `target.txt` 列出要注入的具体 App 包名（`com.android.vending`、`com.google.android.gms`、`io.github.vvb2060.keyattestation` 等），在 `sepolicy.rule` 里开 `allow crash_dump keystore process *`、`allow crash_dump platform_app process *` 通道，对 **soterserver / keystore 等具体进程**做持久注入——**从不碰 zygote**。这正是单 App 注入路线的现成验证。

---

## 项目状态

**阶段：模块管理兼容层已闭环；LSPosed 可安装、可打开；单 App 进程注入已实测打通。**

已完成可编译、可运行的模块代码（`src/testModule/module_zygisk_compat/`），真机验证闭环：

- **安装**：上传 zip → 解压 → 落盘 `/data/adb/modules/<id>/` → 权限 → source 官方 util_functions.sh → customize.sh。
- **列表**：扫描 `/data/adb/modules/` 展示 id/name/version/author/description，标记 `hasWebui`。
- **卸载**：支持任意目录名（含括号等特殊字符），修复 query URL decode 与校验过严两个缺陷。
- **通用模块 WebUI 代理**：任意带 `webroot/` 的模块可点开界面；注入 `window.ksu.exec` 兼容桥，重写根绝对路径到模块子路径。
- **ksu.exec 兼容桥**：后端 `/ksuExec` + 前端 `ksu-bridge.js`，对齐 KernelSU 三参回调契约。
- **Magisk 身份伪装**：`MAGISK_VER_CODE=28101`，LSPosed 可正常安装。

**当前结论**：让 Zygisk/LSPosed 的 hook 生效，正路是「注入具体 App 进程 + 常驻 watcher 自动触发」，而非注入 zygote。详见 `docs/device-probe.md` 与 `docs/roadmap.md`。

---

## 目标

1. 摸清 SKRoot Pro 的模块加载机制与 SDK 接口（基于实机，非假设）。
2. 在 SKRoot Pro 上实现进程注入能力（单 App 注入已通，zygote 注入不可行）。
3. 使 LSPosed / Xposed 模块可以在 SKRoot Pro 上运行（安装/打开已通，hook 生效走单 App 注入路线）。