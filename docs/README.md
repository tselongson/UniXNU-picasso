# 文档

Redmi K30 5G（picasso / SM7250）上的 XNU 移植。本目录记录状态、关键发现和调试方法。

## 从哪里读起

| 文档 | 内容 | 什么时候读 |
|---|---|---|
| **[guide.md](guide.md)** | **参考项目地图、三条技术路线、项目规划、工程方法** | **接手这个项目，先读这篇** |
| [status.md](status.md) | 阶段进度表、当前阻塞点、启动参数 | 想快速知道"到哪一步了" |
| [handoff.md](handoff.md) | 已验证/未验证清单、纪律 | 需要确认某个结论是否可靠 |
| [ufs-root.md](ufs-root.md) | UFS 两个沉默失败的完整排查过程 | 想理解存储层为什么这么难 |
| [debugging.md](debugging.md) | 抓日志的方法、每个参数的作用、补丁的四个坑 | 要动手改东西之前 |
| [usb-console-bringup.md](usb-console-bringup.md) | USB 控制台早期搭建记录 | 想改控制台本身 |
| [evidence/](evidence/) | 带日期的原始串口日志 | 核对某个结论 |

## 一句话现状

内核跑过 BSD 层，USB 控制台可用，**UFS 已发布根设备、APFS 读到容器 checkpoint**；
当前卡在根卷认证（设备树缺 `system-volume-auth-blob`）。

## 证据优先

`evidence/` 里的日志是判断依据。文档里的每条结论都标注了对应日志或源码位置；
没有证据的推测在 [handoff.md](handoff.md) 的"未验证"一节里单独列出，**不和结论混在一起**。
