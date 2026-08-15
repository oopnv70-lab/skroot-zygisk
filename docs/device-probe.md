# 设备实测记录（Device Probe）

> 本文件所有内容来自实机命令输出，禁止凭假设补充。后续所有架构决策以此为准。
> 探测时间：2026-08-16

## 1. 系统与内核

| 项目 | 值 |
|---|---|
| 内核版本 | `6.1.141-android14-11-o-g984c12362a16` |
| 架构 | aarch64 |
| Android 版本 | 16 |
| SDK | 36 |
| CPU ABI | arm64-v8a |
| 品牌痕迹 | OPPO / 一加（`com.oplus.pantanal.ums` 进程存在） |

## 2. SELinux 状态

```
getenforce  -> Enforcing
/sys/fs/selinux/enforce -> 1
```

SELinux 处于 **Enforcing** 状态。

## 3. root / su 状态

```
su -c id
  uid=0(root) gid=0(root)
  context=u:r:shell:s0
```

| 项目 | 值 |
|---|---|
| su 路径 | `/system/bin/su` |
| su 大小 | 741784 字节 |
| su 权限 | `-rwxrwxrwx root root` |
| su 落盘时间 | 2026-08-16 04:49 |
| su 运行上下文 | `u:r:shell:s0`（shell 域，非 magisk/su 域） |

**关键结论**：此 su 是 SKRoot Pro 自带，在 shell 域内授予 root，SELinux 仍为 Enforcing。

## 4. 进程列表（关键进程）

```
PID   PPID  NAME
1682  1     zygote64
1742  1     zygote64
4021  1682  system_server
4753  1682  webview_zygote
```

存在两个 `zygote64`（PID 1682 / 1742），架构正常。

## 5. `/data/adb/` 目录结构（非标准）

```
/data/adb/
├── enenimei         (25 字节，普通文件)
├── modules_update/  (目录)
├── oo/              (目录)
└── usa/             (目录)
```

- **不存在** `/data/adb/modules`、`/data/adb/magisk`、`/data/adb/ksu`、`/data/adb/ap`。
- 这是 **SKRoot Pro 自定义的目录结构**，非 Magisk/KernelSU/APatch 标准布局。
- `oo/`、`usa/`、`modules_update/`、`enenimei` 的具体内容**尚未探测**（待补）。

## 6. 待探测项（TODO，后续实测补充）

- [ ] `/data/adb/oo/` 内容
- [ ] `/data/adb/usa/` 内容
- [ ] `/data/adb/modules_update/` 内容
- [ ] `/data/adb/enenimei` 内容
- [ ] SKRoot Pro APP 包名
- [ ] SKRoot 模块装载 `.so` 的真实路径与机制
