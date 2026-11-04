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

## 追加参照:unixnu-wcn7860-wifidriver(u0er,小米17 Ultra/SM8850+WCN7860)

已联网验证的完整实现(GPT 6.1 Sol 制作,扫描/连接/访问百度正常),8636 行,
来源 https://github.com/u0er/unixnu-wcn7860-wifidriver 。层级图谱(我们的
栈照此组织,把 PCIe/MHI 换成 SNOC/CE,ath12k-QMI 换成 ath10k-WLFW v1):

| Peach 层 | 行数 | 对应我们的 |
|---|---|---|
| pandora_wifi.c(入口/生命周期) | 1618 | WlanService/WlanProbe |
| PeachWmi.c(WMI 命令) | 1280 | 待建(第 4 阶核心) |
| PeachWlfw.c(QMI/WLFW) | 556 | WlanQmi + 已移植的 WlanWlfwProto.h |
| PeachNet.c(ifnet/en0) | 413 | WlanNet |
| PeachApple80211.c(ioctl) | 330 | WlanApple80211 |
| PeachStation/Rsn/Htc/Htt/Rx/Tx | ~1900 | 待建 |
| PeachCe.c(copy engines) | 239 | WlanCe(直接可比对) |
| PeachMhiHost/Firmware + PcieData | ~600 | 不适用(SNOC 替代) |
| pandora_files.c(从磁盘卷读固件) | 381 | **照抄**:固件放数据卷,不入库 |
| ArmSmmu4K.h(复用 picasso 的 SMMU 头!) | - | 我们自己的 |

关键收获:固件供给用"数据卷文件加载"(pandora_files.c 模式),彻底绕开
"blob 不入库"与构建内嵌的两难;固件从本机 vendor/modem 分区提取后放到
ESP/数据卷固定路径,驱动启动时读。

## 阶段 B 详细计划:MODEM(MPSS)冷启动(2026-11-04 补充)

真机已验证:SMEM v12 表可解析(14 条目),SMP2P 435 / GLINK 478-480 因 MPSS 未启动而缺失。
WLFW/GLINK 的宿主是 MODEM,不启它没有对象可谈。三条并行的子任务:

### B1. 固件读取通道
- modem 分区(mba.mbn + modem.mdt + 分段)在 UFS 上;先查 NativeUfs 是否已把 modem 分区
  作为块设备发布(看 DT 的 partitions 节点与 UfsService 的 publish 列表)。
- 读取器:按块读 + MBN 解析(复用 WlanFirmware.c 的 ELF/MBN 解析)。固件不入库,运行时读分区。

### B2. PIL/SMC 路径探查
- 查本机 bootloader(Xiaomi XBL/ABL)暴露的 SCM 接口:PAS_AUTH_AND_RESET / PAS_SHUTDOWN / mem_setup。
  参考 Trogdor 的 TF-A SMC64 经验(他们机器 PAS 未实现,走 MBA 自认证;我们要先探本机能力)。
- 若 SMC PAS 不可用:评估"由 UEFI 侧启动 MPSS"的可行性(UEFI 有自己的 SMC/SCM 通道)。

### B3. 启动后验证
- SMP2P 435 出现(slave state)+ GLINK 478/479/480 出现 → stage2 的 SMMU 读应该也能看到
  SID 0xc0 的 SMR/S2CR 变化(当前 matches=0,说明 WLFW 的流映射由固件侧建立)。
- 之后进阶段 C:WLFW QMI 握手(传输层已就绪:WlanSmem.h/WlanWlfwProto.h)。

## 阶段 B2 设计(2026-11-04): MPSS/WCSS 的 PIL 启动

### 已确认的现成设施(树内,已在硬件跑通)
- `drivers/NativeAdrenoPicasso/Adreno620Cp.cpp` 有完整的 SMC/PAS 实现:
  - SMC 约定: `kScmSipStd = 0x42000000` (STD_CALL|SMC_64|OWNER_SIP),x0..x3 参数、x6 会话 id;
    x0==1 = QCOM_SCM_INTERRUPTED(必须带 x6 重试,否则 PIL 会话卡死——wf106-109 的教训)。
  - PAS 命令: INIT_IMAGE(ARGS(2,VAL,RW)=0x82) / MEM_SETUP(ARGS(3)=0x03) / AUTH_AND_RESET(ARGS(1)=0x01)。
  - **必须先 rpmhVoteCryptoPath()**(wf115:crypto 引擎 NoC 无投票时,读寄存器 SError 死机)。
  - **本平台 modem PAS ID = 1**(代码里 `pas-supported(1)` 作对照探针)。
  - metadata 格式: ELF 头+phdrs+hash 段无 padding;region 只算 PT_LOAD 段。

### 固件传输的硬约束
- modem 固件 82MB(modem.mdt+33 段),boot.img 已 62MB(分区~64MB)→ **不能内嵌**。
- 固件本来就在设备的 modem 分区(NON-HLOS.bin 那个 FAT 镜像),**缺的是运行时 UFS 分区读取**。
- 两条路:
  a. 扩展 NativeUfs 加 GPT 枚举+分区读取(silenzio 的文件,需批准);
  b. UEFI 侧(uefi_picasso 仓库)预读 modem 分区到内存,经 boot_args/ADT 传给 XNU
     (UEFI 有完整的 SMC/文件读取能力,且不碰 silenzio 的内核代码)。

### 待定问题
- SM7250 的 WLAN 固件到底跑在 MPSS 还是独立 WCSS 处理器?(本板 DT 有 WCSS 0x18800000 节点;
  WCN3980 可能是独立 WPSS 类子系统,pas-id 待查)。先做 PAS_IS_SUPPORTED 全 pas-id 扫描(现成 SMC 代码可复用),
  再决定加载目标。

## 阶段 B3 实现路线(2026-11-04 最终确定,纯软件,无需人工按键)

前提:fastboot fetch/oem dump 均不支持;UEFI Mass Storage 需要按按钮(不可用)。
modem 分区(FAT16,含 modem.mdt + b00..b33)就在设备 UFS 上,**内核自己读**是最短路径,
且 PAS 加载本来就在内核完成,固件无需出机器。

### B3.1 NativeUfs 分区读取(需改 silenzio 的文件,新增只读 API)
- GPT: 读 LBA1 头 + 分区项数组,按 label 找 "modem"。
- 新增 `native_ufs_partition_find(name)` / `native_ufs_partition_read(name, off, buf, len)`,
  SCSI READ(10) 复用现有 command() 通路;限长(≤2MB/次),只读,配 boot arg 门控。

### B3.2 WlanMpss 加载器(我们的新文件)
1) 用 B3.1 读 modem 分区 → FAT16 解析(参考已写的 Python 版:FAT 偏移表、LFN、簇链)。
2) 取 modem.mdt(ELF32 hash 表)+ 需要的 bXX 段(按 ELF phdr 的 p_paddr 落位)。
3) MPSS 目标内存:DT 的 mpss 区域(需从 ADT/picasso.dts 取 base/size;staged 的 modem.mdt 与
   "modem_wlan_mem" 提示该板已预留)。
4) PAS 序列(照 Adreno620Cp): 先 rpmhVoteCryptoPath → INIT_IMAGE(pas=1, mdt_paddr)
   → MEM_SETUP(pas=1, mba_addr, mba_size) → AUTH_AND_RESET(pas=1);x0==1 带 x6 重试。
5) 成功后轮询 SMEM SMP2P item 435(slave state)确认 MPSS 起来。

### B3.3 之后
- stage2 应能看到 SID 0xc0 的 SMR/S2CR 变化;接着 WLFW QMI(传输层已就绪)。
