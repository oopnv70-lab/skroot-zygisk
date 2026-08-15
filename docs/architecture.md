# 架构设计（Architecture）

> 占位。待阶段 1 实测完成 SKRoot Pro 的加载机制后补充。
> 当前禁止填充基于假设的架构内容。

## 待明确的核心问题

1. SKRoot 模块如何被装载（路径、格式、入口）。
2. SKRoot 内核 hook 接口能 hook 到哪些函数（fork / clone / dlopen / prctl？）。
3. SELinux Enforcing 下，注入 zygote 需要额外处理什么。

（以下为空白，等待实测数据）
