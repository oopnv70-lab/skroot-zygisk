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

- [ ] 确认能否 hook zygote 的 fork / dlopen 路径
- [ ] 确认 SELinux Enforcing 下的注入可行性
- [ ] 选型：自研 / 旧版 GPL 源码重构 / 兼容层

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
