# 路线图（Roadmap）

> 原则：**慢，稳，每步实测验证，禁止假设**。

## 阶段 0：空壳搭建（当前）

- [x] 建立仓库骨架
- [x] 记录设备实测数据
- [ ] 确定许可证
- [ ] 确定仓库定位与命名

## 阶段 1：摸清 SKRoot Pro（实机）

- [ ] 探测 `/data/adb/` 各目录真实内容
- [ ] 找到 SKRoot Pro APP 包名与模块加载接口
- [ ] 拉取 SKRoot SDK 头文件（`abcz316/SKRoot-linuxKernelRoot`）
- [ ] 定位 SDK 的内核 hook 接口（`module_base_kernel_func_hook.h` 等）

## 阶段 2：Zygisk 能力可行性

- [x] 确认能否 hook zygote 的 fork / dlopen 路径 → **不可行（见下方结论）**
- [x] 确认 SELinux Enforcing 下的注入可行性 → **Enforcing 拦截；Permissive 下仅推进到「注入最后一步失败」**
- [x] 选型：自研 / 旧版 GPL 源码重构 / 兼容层 → **兼容层（模块管理框架）已落地**

### ⚠️ 阶段 2 结论（已定论，2026-08）

经真机逐层排查（含 ZygiskNext bugreport 分析 + SELinux 实测）：

1. **SKRoot Pro 的 root 是 `u:r:shell:s0` 域的容器 root**：`id` 显示 `uid=0(root)`，但 `context=u:r:shell:s0`；提权核是 `supervisor`（`/data/.../TEESimulatorRS_Core/supervisor`，由 init 拉起，同 shell 域）。
2. **zygote 是 `u:r:zygote:s0` 域**，与 shell 域之间无 ptrace 授权规则。
3. Zygisk-Next 的 `zygiskd` 注入 zygote 必然失败：`connect daemon failed with 22` → 随机名 `--suicide` 进程 SIGABRT → tombstone 堆积 → 前端 `Could not connect to service!`。
4. `setenforce 0` 可进 Permissive（证明能降级 SELinux），但仅把错误推进到 `.magic` 已写出、报 `❌ Last injection failed!`——根因（容器够不到宿主机 zygote）不变。
5. `znctl` 的 `dump-zn`/`status`/`znmod` 均依赖 zygiskd daemon，daemon 起不来则全部空转，无法「薅」到注入外的价值。

> 最终判断：**本工程定位为「SKRoot 容器内的 Magisk 模块管理兼容层」**（安装/列表/卸载/WebUI/ksu 命令桥），
> hook 注入能力需真机 Magisk/KernelSU/APatch，非本容器可达成。

## 阶段 3：最小可运行件（MVP）

- [ ] 在测试机上验证注入 zygote
- [ ] 提供一个最小 Zygisk API 实现
- [ ] 验证 LSPosed / Xposed 模块能加载

## 阶段 4：完善与加固

- [ ] 反检测、隐藏痕迹
- [ ] 兼容多内核 / 多机型
- [ ] 文档补全

---

## 三方案备忘（待最终选型）

1. **从零写一个 Zygisk 等价模块**（工作量大，完全自主）
2. **找旧版 GPL 源码重构分支**（起点高，授权干净）
3. **写兼容 Magisk 模块格式的 SKRoot 模块**（兼容层）

> 三方案不互斥，倾向「方案二 + 方案三」组合，方案一兜底。最终以阶段 1 实测结果为准。
