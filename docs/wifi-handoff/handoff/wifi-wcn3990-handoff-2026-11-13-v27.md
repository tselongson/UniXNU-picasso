# WiFi (WCN3990) 交接 — 2026-11-13 18:4x（v27：DNS 发布走错层已查实并修正）

> **【历史归档】本文件已不是权威状态源，请以
> `handoff/wifi-wcn3990-handoff-2026-11-13-v28.md` 为准。**
> v28 记录 chain-607 实机判决：`6673613` 的 DNS 方向正确，但
> `wifiui_configure_service()` 每次崩掉 configd（6 次重启），
> 而 DNS 发布被放在它后面 ⇒ 一次都没执行；`857e48f` 停用该调用并将
> DNS 拆到独立 detached pthread。v27 的源码证据与全部结论仍然有效。
> 判据口径：checkpoint 明文 + 用户可操作性，不是「0 panic」。
> 本版核心：`c6725d1` 把 DNS 写到了 IPMonitor 的**输出键**；本轮用 Apple
> 一手源码 + 官方仓库原文查实因果链，改成**输入键**并补上 Apple 必需的
> `SupplementalMatchDomains`，同时修掉启动期时序猜测。**未上机，不得宣称已修好。**

---

## 0. 一句话状态

```
驱动/固件/握手/数据面/en0/内核 DHCP ✅   journal 死锁已解 ✅
注入链四道关（AMFI/env/dyld/root）✅   c0 对照成立 ✅
*TIMEOUT* 终止标记发布 ✅   wifid sets×17 原生栈恢复 ✅
零下载 boot 路径（16 KiB marker）✅   5cb8977 死锁已撤销 ✅
── v26 最后一步 c6725d1：把 DNS 写 State:/Network/Global/DNS
   ⇒ 源码查实该键是 IPMonitor 的 OUTPUT，第三方写入不触发
     dns_configuration_set()，mDNSResponder 走 dnsinfo XPC 拿不到。
── v27：改写 per-service 输入键 + SupplementalMatchDomains:[""]
   + 有界等待 resolv.conf + 保持已验证 checkpoint 时序
   ⇒ commit 6673613，本地门禁全过，CI 43410190812 在跑
── 设备物理离线（fastboot 空 / COM5 消失 / 无 USB 设备）：上机待用户接上
```

---

## 1. 本轮通读范围

- `handoff/`：v20–v26 全部（v26 逐行读完）、v19 及更早要点。
- `.workbuddy-ai/memory/`：**2026-11-13 全文**，11-12/11-11 等脉络逐份核对。
- 代码：`upstream/silenzio-picasso` HEAD `c6725d1` → `6673613`；
  `package/picasso-springboard/picasso-springboard-probe.m` 全文。
- Apple 一手源码：`work/refs/apple-oss/configd/Plugins/IPMonitor/
  {ip_plugin.c,dns-configuration.c}`、`dnsinfo/{dnsinfo_copy.c,
  dnsinfo_server.c}`、`SystemConfiguration.fproj/SCSchemaDefinitions.c`。
- 外网：Apple 官方 `apple-oss-distributions/configd` 的 `ip_plugin.c`
  （WebFetch 交叉验证输入/输出键方向）；中文引擎检索无有效结果，不采信。
- 参考项目：`work/refs/hakios/userland/src/net_test.m`
  （`waitaddr IF SECONDS` 证明「先等 DHCP、再 resolve」是参考做法）、
  `UniXNU-pong/userland/package-network.c`。

---

## 2. ★ 精准定位：`State:/Network/Global/DNS` 是输出键，不是输入键

| 证据 | 结论 |
|---|---|
| `ip_plugin.c:6245/6262` `update_dns_global_resolver()`：`keyChangeListRemoveValue(keys, S_state_global_dns)` / `keyChangeListSetValue(keys, S_state_global_dns, new_dict)` | IPMonitor **自己写**这个键 |
| `ip_plugin.c:9103` `ip_plugin_init()`：`SCDynamicStoreRemoveValue(S_session, S_state_global_dns)` | 启动时还主动删它 |
| `ip_plugin_copy_keys()` `:8953-8965`、`ip_plugin_copy_patterns()` `:8968-9004` | 监听 `Setup:/Network/Global/IPv4`、`S_private_resolvers`、`S_multicast_resolvers`，以及 `add_service_keys(kSCCompAnyRegex, …)` 生成的 **per-service** `Setup:/`+`State:/Network/Service/<id>/<entity>` 正则；`entityTypeNames[2] = &kSCEntNetDNS` (`:572`) ⇒ **包含 DNS**。列表里**没有** `State:/Network/Global/DNS` |
| `dns-configuration.c:1758` `_dns_configuration_store(&dns_create_config)` | 真正下发到 mDNSResponder 的调用，只在 `dns_configuration_set()` 内 |
| `dnsinfo_copy.c:50` `dns_configuration_copy()` | 是 XPC 客户端（`DNSINFO_SERVICE_NAME`），从 `dnsinfo_server` 的 store 读，**不读 dynamic store 的 Global/DNS 键** |
| `ip_plugin.c:8673` `update_dnsinfo(services_info, S_primary_dns, …)` → `:6300` `dns_configuration_set(dict, primary, S_service_state_dict, …)` | 唯一生产链 |
| Apple 官方仓库 `ip_plugin.c`（WebFetch） | 服务级键变化 → `parseNetworkServiceString()` → `get_dns_changes()` → `S_service_state_dict` → `dns_configuration_set()`；**「第三方只写 Global 键不会触发 IPMonitor 重算」** |

⇒ `c6725d1` 注释里「`ip_plugin.c:6262` publishes exactly this key;
`dns_configuration_set()` consumes it」**因果写反**：6262 是
`dns_configuration_set()` 的**结果**写出去，不是它的输入。
即便 `dns publish=1`，mDNSResponder 也拿不到任何 DNS 服务器。

### 2.2 第二个必需条件：`SupplementalMatchDomains`

`dns-configuration.c:270-276` `add_supplemental()`：

```c
domains = CFDictionaryGetValue(dns, kSCPropNetDNSSupplementalMatchDomains);
n_domains = isA_CFArray(domains) ? CFArrayGetCount(domains) : 0;
if (n_domains == 0) return;    /* 直接丢弃 */
```

`:311-316`：数组中**空字符串 `""`** ⇒ 移除 `DomainName` ⇒
「匹配所有查询」的默认 resolver。`SCSchemaDefinitions.c:126/130`
确认字面量是 `"ServerAddresses"` / `"SupplementalMatchDomains"`。
⇒ 只有 `ServerAddresses` 的 DNS 字典（c6725d1 的写法）会被静默丢弃。

### 2.3 第三个缺陷：启动期时序靠猜

`wifiui_thread()` 只等 `if_nametoindex("en0")`（BSD 接口 attach 很早），
而内核 DHCP 在 WCN3990 关联完成后（约 t+168 s）才写
`/private/var/run/resolv.conf`。`wifiui_publish_dns()` 原先一次性
`open()` 失败即返回 ⇒ 极可能永远扑空。**没有实机 cap 可判**
（lite4 已删 stale c0/c1 cap，随后 USB stall 没采集到新的），
因此按「不猜」原则改成**有界等待**，而不是假定某个时序。

---

## 3. 修复（commit `6673613`，1 file +98/−23）

文件 `package/picasso-springboard/picasso-springboard-probe.m`
（按 v26 铁律 15，用 Edit 通道逐处替换，源码写入保护会拒绝脚本写盘）。

1. 新增 `WIFIUI_DNS_WAIT_SECONDS = 240`；`wifiui_publish_dns()`
   有界轮询 resolv.conf，打印 `dns wait=%us`；超时打印
   `dns resolver file absent after %us`。等待在 detached pthread，
   **不占 configd 主 RunLoop**（`5cb8977` 的死锁正是阻塞主 RunLoop）。
2. DNS 字典 `{ ServerAddresses:[…], SupplementalMatchDomains:[""] }`。
3. 依次写三个键，每个都打印 `dns set=<0|1> key=…`：
   ```
   Setup:/Network/Service/picasso-wifi/DNS     （持久配置）
   State:/Network/Service/picasso-wifi/DNS     （触发 IPMonitor 通知）
   State:/Network/Global/DNS                   （仅状态，OUTPUT 键）
   ```
4. **把 `wifiui_publish_dns()` 移到 `wifiui_bootstrap()` 最后**：
   它的 240 s 等待不得推迟已实机验证的 `*TIMEOUT*` 发布、
   `service begin`、`wifid sets`×17 等 checkpoint。
   `SCDynamicStore` session 保持到发布之后才 `CFRelease`。
5. `wifiui=1` 门控不动 ⇒ c0 完全 inert，单变量成立。

### 门禁（全过）

| 检查 | 结果 |
|---|---|
| WSL clang `-fsyntax-only -Wall -Wextra -Werror`（`work/tmp-wifiui-syntax.py` CF/IOKit stub） | **RC 0 / 0 errors** |
| `work/abi-check.py` | **0 HARD**（4 个既有 soft） |
| 花括号 / 圆括号 / 方括号平衡 | 0 / 0 / 0 |
| `.m` sha256 | `624a4e4e…`（变了 ⇒ recipe 变了） |
| 用户 6 处未提交改动 | 原样保留（5 evidence 删除 + charter.md） |

CI `43410190812`：`fast=true create_data_seed=true`（**不带
build_userdata**，lite 路径已证明无需 5.87 GB userdata）。

---

## 4. 设备状态（当前硬阻塞）

`resume-606-lite4.sh`（18:16）本是为恢复 lite4 的 USB stall 写的：
`fastboot flash userdata marker-clear.sparse.img` 卡在
`Sending 'userdata' (0 KB)` → exit 9；同一命令在 lite3 只用 0.037 s，
故判断是主机/USB 侧掉线，不是镜像或分区几何问题。

18:2x 复核：

```
fastboot devices            → 空
COM5                        → 不存在（系统只剩 COM3/COM4 蓝牙串口）
Get-Process python/fastboot → 无残留
USB fastboot/picasso 设备   → 无
```

⇒ **设备物理离线，需要重新接 USB 并进 fastboot**，这一步只能用户做。
CI 在云端不受影响；产物已安排后台下载。

### 恢复后执行顺序（不可换）

1. `fastboot devices` 确认；`fastboot getvar partition-size:userdata`
   须为 `0x1AC2BFB000`，否则停下重测 marker LBA。
2. `fastboot flash userdata out/full-flash/marker-clear.sparse.img`
   （16 KiB journal marker 清零；新 seedHash 必须这一步，v26 铁律 12）。
3. 用 **本次 CI 43410190812 的 boot.img** 重新生成 c0/c1
   （`mk-autojoin-args.py` → `mk-args.py wifiui=0/1`，995 B、
   全镜像仅 1 字节不同）。
   ⚠ 旧 `boot-exp-c0/c1.img` 是 `c6725d1` 的镜像，**不能复用**。
4. c1（判决）400 s，c0（对照）400 s；采集后**不重启**，留给用户实测。

### 判据（checkpoint 明文，不是 0 PANIC）

| 判据 | 期望 c1 | c0 |
|---|---|---|
| `PICASSO_WIFIUI dns wait=` | 1（实际等待秒数） | 0 |
| `PICASSO_WIFIUI dns servers=` | ≥1 | 0 |
| `dns set=1 key=Setup:/Network/Service/picasso-wifi/DNS` | 1 | 0 |
| `dns set=1 key=State:/Network/Service/picasso-wifi/DNS` | 1 | 0 |
| `configd started with` | **1**（8 = 死锁回归） | 0 |
| `quiet KERN_DENIED` / `service begin` / `truthful=1 publish=1` | 各 1 | 0 |
| `wifid sets` / `DHCP: bound` / `join=5` / 0 PANIC | ✓ | ✓ |
| **用户实测** | `http://192.168.101.1`（IP 层）→ 域名 → 设置 Wi-Fi 详情 | — |

⚠ DNS 最终判据**只能用户实测 Safari 输域名**：日志里没有
`State:/Network/Global/DNS` 的打印代码（该字符串只在源码注释），
且 `getaddrinfo()` 结果不进串口日志。
⚠ 判据字符串必须整行精确匹配；`'publish=1' in log` 会误匹配
`arrival_publish=1`（v26 已修过一次）。

---

## 5. 未通清单（均未宣称已修）

1. **DNS / 域名解析**：`6673613` 已按源码修方向，待上机 + 用户实测；
2. **设置 App 点 Wi-Fi 详情卡死**：`*TIMEOUT*` 已发布、`wifid sets`×17，
   但用户 11-12 实测仍卡；属原生控制面/Preferences 集成，待复测；
3. **状态栏 / SystemConfiguration 显示已连接**：与原生 service 同源；
4. **凭据持久化**：`wpass=` 仍是 boot-arg 明文；
5. **Safari HTTPS 端到端实测**：从未在「关联稳定 + IP 对照」下做完；
6. 5G MGMT_TX `status=3`（非阻塞）。

---

## 6. 铁律（v26 §8 全部沿用，新增 2 条）

16. **写 dynamic store 前先确认键的方向**：`State:/Network/Global/DNS`
    是 IPMonitor 的**输出**；输入是 per-service
    `Setup:/|State:/Network/Service/<id>/DNS`。别把「源码里出现这个键」
    当成「写它就能驱动该逻辑」。
17. **supplemental DNS 字典必须带 `SupplementalMatchDomains`**
    （含 `""` 表全部查询），否则 `add_supplemental()` 直接丢弃；
    只有 `ServerAddresses` 是无效配置。

---

## 7. 关键路径

| 用途 | 路径 |
|---|---|
| 唯一权威 | 本文 v27 |
| 本轮修复 | `6673613`（`picasso-springboard-probe.m`），CI `43410190812` |
| 上机链 | `work/chain-606-lite4.sh` / `work/resume-606-lite4.sh`（需换成本次 boot.img） |
| 恢复脚本 | `work/resume-606-lite4.sh`（镜像已在盘：`boot-exp-c0/c1.img`、`marker-clear.sparse.img`） |
| 语法检查 | `work/tmp-wifiui-syntax.py`（WSL clang stub） |
| marker 清零 | `work/make-marker-clear-sparse.py` |
| 源码证据 | `work/refs/apple-oss/configd/Plugins/IPMonitor/{ip_plugin.c,dns-configuration.c}`、`dnsinfo/dnsinfo_copy.c` |
| 参考项目 | `work/refs/hakios/userland/src/net_test.m`、`UniXNU-pong/userland/wifi-bootstrap.c` |
| 中断现场 | `work/chain-606-lite4.out`（USB stall exit 9）、`work/run-606d.out` |
| 本轮产物 | `out/run-43410190812-v607-1794567908/picasso-bootimg-609/boot/boot.img`（65,105,920 B） |
| 上机链（新） | `work/chain-607.sh [boot.img]`（`bash -n` 通过、离线单变量预演通过） |

---

## 8. CI `43410190812` 结果与离线验证（19:0x）

**CI success（30m19s，`fast=true create_data_seed=true`）**，产物已下载：

```
out/run-43410190812-v607-1794567908/picasso-bootimg-609/boot/boot.img  65,105,920 B
```

离线核查（全部 PASS）：

| 检查 | 结果 |
|---|---|
| `boot.img` 内含 `dns wait=` / `dns servers=` / `dns set=` / `SupplementalMatchDomains` / 两条 `.../Service/picasso-wifi/DNS` / `State:/Network/Global/DNS` | 各 ×1 |
| `linux-inputs/libpicasso-sb.dylib` | **94,864 B / sha `977f484d…`**（c6725d1 版是 94,208 B / `2210d06c…`），含全部新 checkpoint ⇒ 修复后的库确实进了 boot |
| seed-609 identity | `volume_uuid 0733E5C5…`、`seed_identity_sha256 acb2d23b…`、`raw_sha256 9a88494c…`，与 seed-603/605 全不同 ⇒ 新建 seed 生效（铁律 14） |
| `mk-autojoin-args.py` → `mk-args.py wifiui=0/1`（用**新** boot.img） | c0/c1 各 **995 B**、显示参数与 `wifiauto` 全在、**全镜像仅 1 字节不同**（offset `60233858`） |

⚠ 盘上 `boot-exp-c0/c1.img` 仍是 **c6725d1** 的旧镜像，`chain-607.sh`
会删掉并用新 boot.img 重建；**不可复用**。

**设备仍物理离线**（`fastboot devices` 空）。接上后直接
`bash work/chain-607.sh out/run-43410190812-v607-1794567908/picasso-bootimg-609/boot/boot.img`。

---

## 9. 源码闭环复核：per-service DNS 键不会被 service 门控挡掉

`6673613` 写的 serviceID `picasso-wifi` 没有对应的 IPv4/IPv6 实体，
因此 `get_dns_changes()`（`ip_plugin.c:4832`）里
`active_protos == kProtocolFlagsNone`。必须确认这条分支不会丢弃我们的
DNS 字典：

| 检查点 | 源码 | 结论 |
|---|---|---|
| `state_dict == NULL && setup_dict == NULL` → `goto done` | `:4864-4867` | 我们写了 `State:/.../DNS`，`state_dict` 非空 ⇒ 不触发 |
| `active_protos == None` 时 `setup_dict = NULL`（只读 State:） | `:4891-4895` | 我们同时写了 State: 键 ⇒ 数据仍在 |
| `merge_list[]` 是否包含 `SupplementalMatchDomains` | **`:4849`： `{ kSCPropNetDNSSupplementalMatchDomains, ALLOW_EMPTY_STRING, TRUE }`** | 含，且 flag 正好允许我们的 `""` 空字符串 |
| `merge_array_prop()` 合并 state/setup 数组成 `new_dict` | `:4223-4258` | `[""]` 会进入 `new_dict` |
| `active_protos == None` 时只有 `new_dict` 含 `SupplementalMatchDomains` 才不 `goto done` | `:4958-4980` | 满足 ⇒ 保留为 "State: supplemental" |
| `service_dict_set(serviceID, kSCEntNetDNS, new_dict)` → `changed` | `:5021` | 触发 `dns_changed` → `update_dnsinfo()` (`:8673`) |
| 通知键前缀匹配 | `:8356-8384`：`S_state_service_prefix` / `S_setup_service_prefix` → `parseNetworkServiceString()` → `service_changes` | 两个键都会触发 |

⇒ 完整链成立：`State:/Network/Service/picasso-wifi/DNS` →
`S_service_state_dict` → `dns_configuration_set()` →
`add_supplemental()`（有 `[""]` 不会被丢弃）→ `_dns_configuration_store()`
→ mDNSResponder。**无需再改代码**。

### 9.1 预案：若 DNS 判决仍失败（参考项目，未执行）

v26 §4.8 的硬结论仍成立：**不要从 configd 进程内建 SCPreferences
service**（自持 `preferences.plist-lock`）。唯一剩余通路是 pong 的
**独立 root 进程**：

- `UniXNU-pong/userland/wifi-bootstrap.c:169-176` `--configure-only`：
  `publish_interface(); (void)publish_namer_result(); return configure_interface();`
  `configure_interface()` 用真实 en0 IORegistry 对象 +
  `SCPreferencesCreate/Lock(false)/Commit/Apply`，**独立进程**才拿得到锁；
  主入口 `:198-201` 用 `posix_spawn` 起这个 worker，`alarm(20)` 有界超时，
  不拖住启动。
- 本仓库 CI 已有 `xcrun -sdk iphoneos` 工具链与
  `scripts/build-picasso-sb-inject.py`（`.github/workflows/compose-bootimg.yml:99-103`），
  可复用同一编译/签名链产出 arm64 可执行；seed 生成器
  `scripts/picasso_linux_inputs.py:125/253` 已支持把产物放进
  `picasso/usr-lib/` 并做 sha256 校验，扩展一个 job plist + 二进制即可。
- 成本：需新建 seed（recipe 变）、注入 launchd job、`create_data_seed=true`，
  且**必须先有本轮 DNS 的实机判决**，否则又变成串行试错。**暂不执行。**

### 9.2 19:20 设备接回

`fastboot devices` → `64993333 fastboot`，
`partition-size:userdata = 0x1AC2BFB000`（几何闸门通过）。
后台等待任务 `71Xohy` 于 19:20:45 自动触发 `work/chain-607.sh`，
产物 `work/chain-607-auto.out`；c1（判决）+ c0（对照）各 400 s。
