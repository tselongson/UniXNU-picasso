# picasso (SM7250) WiFi 迁移方案 — 对照 Trogdor/pong

目标:把 NativeWlan 从 bring-up 探针升级为真实 WiFi 栈。硬件:SM7250 集成
WCN3980,SNOC + copy engines + QMI/WLFW(WLAN 固件服务挂在 MODEM 上),无 PCIe。

## 三个参照的定位

| 参照 | 架构 | 借什么 |
|---|---|---|
| Trogdor (SC7180, WCN3990 SNOC) | 与我们同构 | 整条链路的实现顺序与协议事实:`platform/coachz-prelink/pending/wifi/` 的 SMEM/SMP2P 解码器与 QMI/TLV 线格式;`docs/plans/wifi.md` 的分层验收表 |
| pong (SM8475, QCA6490 PCIe+MHI) | 异构(PCIe,不是我们) | 组织方式:驱动独立子仓库、`proprietary-files.txt` 固件清单(从真机提取不入库)、Platform/Net/native 三层分离、`configs/*.json` 从真机 REC DT/sysfs 捕获板级输入 |
| ios-flame (Pixel, SM8150) | 同为 iOS 侧 SNOC/QMI | `phone/wifi/{qmi,wlfw,link,glink}.c` 的 iOS 侧 GLINK/QRTR/QMI 实现参考 |

## 阶段阶梯(照 Trogdor 已验证顺序)

0. **板级输入捕获**(不改内核):从本机 stock 固件提取
   - modem 分区:`wlanmdsp.mbn`(WLAN 固件服务在 modem 上)、`mpss` 全套(MBA/qdsp6sw)
   - vendor 分区:WLAN 固件/BDF(`bdwlan.bin`、`regdb.bin`、qca-wlan 的 `wlanmac.bin` 等)
   - DTS:SMEM/SMP2P item 号、GLINK FIFO、WLAN reg/SID、MSA 区、rail 清单
   - 产出:`proprietary-files.txt` + `configs/picasso-wifi-sm7250.json`(仿 pong)
1. **MPSS(MODEM)启动** —— SNOC WLAN 的硬门槛。SM7250 由 Xiaomi XBL 链启动,
   XNU 侧需要 PIL/SMC 路径(对照 Trogdor 的 TF-A SMC64 SIP 经验:他们机器 PAS
   未实现,走 MBA 自认证;我们要先探本机 SMC 能力)
2. **SMEM/SMP2P/GLINK/QRTR**(先只读后读写):Trogdor 的
   `coachz_wifi_smem.h`/`coachz_wifi_proto.h` 可直接移植(BSD-2)
3. **WLFW QMI 握手**:indication 注册 → CAP → MSA-info → mem-ops → FW ready
   (参照 Linux ath10k `qmi.c`/`qmi_wlfw_v01.h` 的线格式)
4. **BDF + CE/HTC/WMI/HTT**:copy engines(我们已有 WlanCe/WlanBmi 骨架)+
   WMI_INIT → 扫描 → BSS 表
5. **en0/Apple80211**:ifnet + ioctl + DHCP(WlanNet/WlanApple80211 骨架已在,
   最终形态参照 pong `PongWifiNet.cpp` 与 sheng `WifiNet.cpp`)

## 现有 NativeWlan 骨架的去留

- 保留:WlanFirmware.c(MBN 解析)、WlanQmi(QMI/TLV)、WlanCe/WlanBmi(CE/BMI)、
  WlanNet/WlanApple80211(en0 雏形)、WlanBoard.h(板级常量,需按第 0 步复核)
- 缺失待建:MODEM 启动(第 1 阶)、SMEM/SMP2P/GLINK/QRTR 传输层(第 2 阶)、
  WMI/HTC/HTT 数据面(第 4 阶)
- WlanProbe 探针:降级为诊断工具,默认关闭(v269 起已验证它卡死启动——stage1
  的 WCSS MMIO 读在 IP 块未上电时会挂总线;重开前必须先做 rail 检查)

## 教训记录

- v269 前的实测:探针在未上电 WCSS 上做 MMIO 读 → IRQ-off 硬锁,启动页卡死。
  任何 IP 块访问前必须先确认电源轨/时钟,这是探针代码的缺陷,真栈的第 2/4 阶
  也要遵守"先 rails 后 MMIO"。
- 探针从未上过硬件就随构建启用,违反了 Trogdor"pending 不进构建链,硬件验收
  分开做"的原则。后续新模块一律先 pending 后接线。
