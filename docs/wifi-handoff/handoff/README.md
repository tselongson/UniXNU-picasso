# 交接文档索引（29 份）

> ⚠ **只有 [`wifi-wcn3990-handoff-2026-11-14-v35.md`](wifi-wcn3990-handoff-2026-11-14-v35.md)
> 是当前唯一权威状态源。** 其余全部是历史归档。
> 早期文档里写着「本文件是当前唯一权威状态源」「vXX 及更早为历史归档」，
> 那是**当时的口径**，现在都已过时，不要据此判断现状。
> 状态冲突一律以 v35 为准；逐级演进脉络看 [`../README.md`](../README.md) §1 与 §5。

## 阅读建议

- **只想接手 / 求助**：读 `../README.md` → v35 → `../logs/2026-11-14.md`，三份足够。
- **想查某个结论是怎么来的**：按下表定位到对应版本，再翻 `../logs/` 同日日志看逐轮实验。
- **想避免重复踩坑**：`../README.md` §5「已证伪清单」和 v35 §6「铁律」优先于任何单份旧文档。

## 按时间索引

| 版本 | 文件 | 当时主题 |
|---|---|---|
| 摘要 | [`2026-11-10-project-history-digest.md`](2026-11-10-project-history-digest.md) | 10-26～11-10 项目历史脉络浓缩 |
| v4 | [`wifi-wcn3990-handoff-2026-11-09.md`](wifi-wcn3990-handoff-2026-11-09.md) | join 里程碑、首次 panic 定位 |
| v9 | [`wifi-wcn3990-handoff-2026-11-10.md`](wifi-wcn3990-handoff-2026-11-10.md) | WiFi 可开机进系统；设置侧待刷 Data |
| v10 | [`wifi-wcn3990-handoff-2026-11-11.md`](wifi-wcn3990-handoff-2026-11-11.md) | 阶段划分、wifid 放行前状态 |
| v11 | [`wifi-wcn3990-handoff-2026-11-11-v11.md`](wifi-wcn3990-handoff-2026-11-11-v11.md) | Data seed 身份 / `runtime_enabled_jobs` |
| v12 | [`wifi-wcn3990-handoff-2026-11-11-v12.md`](wifi-wcn3990-handoff-2026-11-11-v12.md) | HIF 忙等自旋把 UI 饿死 |
| v13 | [`wifi-wcn3990-handoff-2026-11-11-v13.md`](wifi-wcn3990-handoff-2026-11-11-v13.md) | 链路事件 / KEV / `SIOCSIFADDR` |
| v14 | [`wifi-wcn3990-handoff-2026-11-11-v14.md`](wifi-wcn3990-handoff-2026-11-11-v14.md) | IONetworkStack user client 与 InterfaceNamer |
| v15 | [`wifi-wcn3990-handoff-2026-11-11-v15.md`](wifi-wcn3990-handoff-2026-11-11-v15.md) | configd 原生链首次判决 |
| v16 | [`wifi-wcn3990-handoff-2026-11-12-v16.md`](wifi-wcn3990-handoff-2026-11-12-v16.md) | netsvc plist 写入、SIOCSIFADDR 仍 0 |
| v17 | [`wifi-wcn3990-handoff-2026-11-12-v17.md`](wifi-wcn3990-handoff-2026-11-12-v17.md) | 换思路：内核侧 KPI 直达 |
| v18 | [`wifi-wcn3990-handoff-2026-11-12-v18.md`](wifi-wcn3990-handoff-2026-11-12-v18.md) | 内核 DHCP 客户端设计与本地预检 |
| v19 | [`wifi-wcn3990-handoff-2026-11-12-v19.md`](wifi-wcn3990-handoff-2026-11-12-v19.md) | `IPConfiguration.bundle` 缺失、resolv.conf 路径 |
| v20 | [`wifi-wcn3990-handoff-2026-11-12-v20.md`](wifi-wcn3990-handoff-2026-11-12-v20.md) | DNS 发布方向修正 |
| v21 | [`wifi-wcn3990-handoff-2026-11-12-v21.md`](wifi-wcn3990-handoff-2026-11-12-v21.md) | 进度总表与「换个思维方式」 |
| v22 | [`wifi-wcn3990-handoff-2026-11-12-v22.md`](wifi-wcn3990-handoff-2026-11-12-v22.md) | InterfaceNamer / pong 对照 |
| v23 | [`wifi-wcn3990-handoff-2026-11-12-v23.md`](wifi-wcn3990-handoff-2026-11-12-v23.md) | `*TIMEOUT*` 解卡注入链 |
| v24 | [`wifi-wcn3990-handoff-2026-11-12-v24.md`](wifi-wcn3990-handoff-2026-11-12-v24.md) | `IOKitWaitQuiet` / 全机型同源根因 |
| v25 | [`wifi-wcn3990-handoff-2026-11-13-v25.md`](wifi-wcn3990-handoff-2026-11-13-v25.md) | journal 死锁实机解出；注入链全通，卡在 `IOKitWaitQuiet=53` |
| v26 | [`wifi-wcn3990-handoff-2026-11-13-v26.md`](wifi-wcn3990-handoff-2026-11-13-v26.md) | `53=KERN_DENIED` 根因查实并修复 |
| v27 | [`wifi-wcn3990-handoff-2026-11-13-v27.md`](wifi-wcn3990-handoff-2026-11-13-v27.md) | DNS 发布走错层（Global/DNS 是输出键）已查实并修正 |
| v28 | [`wifi-wcn3990-handoff-2026-11-13-v28.md`](wifi-wcn3990-handoff-2026-11-13-v28.md) | chain-607 判决：修掉 configd 崩溃循环 |
| v29 | [`wifi-wcn3990-handoff-2026-11-13-v29.md`](wifi-wcn3990-handoff-2026-11-13-v29.md) | DNS 发布实机闭环（verify-608） |
| v30 | [`wifi-wcn3990-handoff-2026-11-14-v30.md`](wifi-wcn3990-handoff-2026-11-14-v30.md) | 真实 TX 触发整机 SError 的定位与 `txguard` 修复 |
| v31 | [`wifi-wcn3990-handoff-2026-11-14-v31.md`](wifi-wcn3990-handoff-2026-11-14-v31.md) | 键盘不可输入（320 vs 360 键面）与不整体 rebase 决策 |
| v32 | [`wifi-wcn3990-handoff-2026-11-14-v32.md`](wifi-wcn3990-handoff-2026-11-14-v32.md) | 键盘已修好；WiFi 只剩「收」一个方向 |
| v33 | [`wifi-wcn3990-handoff-2026-11-14-v33.md`](wifi-wcn3990-handoff-2026-11-14-v33.md) | `rekey` / `keyfix` / `pspoke` 假设与 PTK 离线复算 |
| v34 | [`wifi-wcn3990-handoff-2026-11-14-v34.md`](wifi-wcn3990-handoff-2026-11-14-v34.md) | `if_input` 路线探到底、`rc=0` 探针（部分结论后被 v35 更正） |
| **v35** | **[`wifi-wcn3990-handoff-2026-11-14-v35.md`](wifi-wcn3990-handoff-2026-11-14-v35.md)** | **★ 权威：协议栈入向因果反转、MAC 不一致根因、wifix 修复与 A/B 闭环、chain-619/621 判决、CE1 静默定位** |

> 注：v34 提出「协议栈不回 ARP」和「`rc=0` 证明帧已进栈」两条结论，
> 均被 v35 用实机证据更正为**判据错误**。引用 v34 时务必连同 v35 §1.5 一起看。
