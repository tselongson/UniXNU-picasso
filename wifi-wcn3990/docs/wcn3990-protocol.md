# WCN3990 host-side protocol constants (ath10k master, read 2026-11-05)

Source: `torvalds/linux` `master`, `drivers/net/wireless/ath/ath10k/`.
Every number used by `source/Wcn3990Hif.cpp` is listed here with its origin.
No value was guessed; where Linux has no constant, the file says so.

## 1. Copy engines

`hw.c`: `wcn3990_values.ce_count = 12`, `.msi_assign_ce_max = 12`,
`.ce_desc_meta_data_mask = 0xFFF0`, `.ce_desc_meta_data_lsb = 4`.
WCN3990 is `target_64bit`, so descriptors are `struct ce_desc_64` (16 bytes):

```c
/* ce.h */
struct ce_desc_64 { __le64 addr; __le16 nbytes; __le16 flags;
                    __le32 toeplitz_hash_result; };
#define CE_DESC_ADDR_MASK  GENMASK_ULL(34, 0)   /* 0x7ffffffff */
#define CE_DESC_ADDR_HI_MASK GENMASK(4, 0)
```

`hw.c`: `wcn3990_regs.ceN_base_address = 0x00240000 + N * 0x1000`, relative
to the WLAN CE BAR `0x18800000`; wrapper at `0x0024C000`.

`hw.c`: `wcn3990_ce_regs`:

| register | offset |
|---|---|
| sr_base_addr_lo / hi | 0x00 / 0x04 |
| sr_size_addr | 0x08 |
| dr_base_addr_lo / hi | 0x0c / 0x10 |
| dr_size_addr | 0x14 |
| host_ie_addr | 0x2c |
| misc_ie_addr | 0x34 |
| sr_wr_index_addr | 0x3c (`WCN3990_SRC_WR_IDX_OFFSET`, snoc.c) |
| dst_wr_index_addr | 0x40 (`WCN3990_DST_WR_IDX_OFFSET`, snoc.c) |
| current_srri_addr / current_drri_addr | 0x44 / 0x48 |
| ce_rri_low / high (DDR RRI) | 0x0024C004 / 0x0024C008 |

`ce.c`: `shadow_sr_wr_ind_addr()` maps OUT rings to the TrustZone-safe shadow
window: CE0 `0x00032000`, CE3 `0x0003200C`, CE4 `0x00032010`,
CE5 `0x00032014`, CE7 `0x0003201C`. `_ath10k_ce_send_nolock_64()` writes the
shadow copy only when `hw_params.shadow_reg_support` is set; otherwise it
writes `ctrl_addr + sr_wr_index_addr` (0x3c). Our driver writes both and logs
what each reads back. Note: `shadow_reg_support` is declared in `hw.h:598`
but no `hw.c` parameter table initialises it; the WLFW shadow table
(`snoc.c: target_shadow_reg_cfg_map`, offsets 0x3C/0x40) is what tells the
firmware about the mirroring.

`snoc.c`: `target_ce_config_wlan` (12 entries) and
`target_service_to_ce_map_wlan`. Relevant subset:

| CE | direction | entries | nbytes | use |
|---|---|---|---|---|
| 0 | OUT (host→target) | 32 | 2048 | HTC control, service `0x0001` |
| 1 | IN | 32 | 2048 | HTT data |
| 2 | IN | 64 | 2048 | HTC control + WMI events |
| 3 | OUT | 32 | 2048 | WMI commands |
| 4 | OUT | 256 | 256 | HTT |
| 5 | OUT | 1024 | 64 | HTT |

`PIPEDIR_OUT` in our WLFW table is 2, `PIPEDIR_IN` is 1 (the values sent in
`WLFW_WLAN_CFG`). Service ids: `ATH10K_HTC_SVC_ID_RSVD_CTRL = 0x0001`,
`WMI_CONTROL = 0x0100`, `HTT_DATA_MSG = 0x0300` (`htc.h` `SVC(group, idx)`
= `group << 8 | idx`).

CE send (`ce.c: _ath10k_ce_send_nolock_64`, `__ath10k_ce_rx_post_buf_64`):
write `addr` (masked), `nbytes`, `flags = 0`, then advance the write index.
On the destination side a non-zero `nbytes` at `sw_index` means the CE has
filled that buffer (`_ath10k_ce_completed_recv_next_nolock_64`); host clears
it and re-posts. `ath10k_ce_rx_update_write_idx()` deliberately keeps one
ring slot free to avoid a full-ring deadlock.

## 2. HTC

`htc.h`: `struct ath10k_htc_hdr` is 8 bytes:

```
u8 eid; u8 flags; __le16 len;  /* len = payload + trailer */
u8 trailer_len; u8 seq_no; u8 pad0; u8 pad1;   /* rx: byte0 = trailer_len */
```
`ATH10K_HTC_FLAG_TRAILER_PRESENT = 0x02` (rx flags). payload-only length is
`len - trailer_len`.

Message ids (`enum ath10k_ath10k_htc_msg_id`): READY 1, CONNECT_SERVICE 2,
CONNECT_SERVICE_RESP 3, SETUP_COMPLETE 4, SETUP_COMPLETE_EX 5.
Control messages are `<htc_hdr eid=0><__le16 message_id><payload>` and travel
on CE0 (host→target) / CE2 (target→host).

READY payload (`struct ath10k_htc_ready`): `credit_count u16`, `credit_size
u16`, `max_endpoints u8`, `pad u8`; extended form adds htc_version,
max_msgs_per_htc_bundle and a u16 reserved.

CONNECT_SERVICE payload (`struct ath10k_htc_conn_svc`): `service_id u16`,
`flags u16`, `pad u8[2]`. `htc.c` sets
`ATH10K_HTC_CONN_FLAGS_DISABLE_CREDIT_FLOW_CTRL = 1<<3` for every service
except WMI_CONTROL, and only enables credit flow control for WMI_CONTROL.

CONNECT_SERVICE_RESPONSE (`struct ath10k_htc_conn_svc_response`):
`service_id u16`, `status u8` (0 = success), `eid u8`, `max_msg_size u16`.
The returned eid is the WMI endpoint used in every WMI HTC header.

`htc.c: ath10k_htc_send_setup_complete()` emits SETUP_COMPLETE_EX (5) with
an all-zero `struct ath10k_htc_setup_complete_extended` on non-SDIO buses.

## 3. WMI-TLV

`wmi.h`: `struct wmi_cmd_hdr { __le32 cmd_id; }` prefixes every command and
event. `WMI_CMD_GRP(grp) = (grp << 12) | 1`,
`WMI_EVT_GRP_START_ID(grp) = (grp << 12) | 1`, `WMI_GRP_START = 0x3`.
Group order: SCAN 3, PDEV 4, VDEV 5, PEER 6, MGMT 7, BA_NEG 8, …

| id | value |
|---|---|
| `WMI_INIT_CMDID` | 0x0001 |
| `WMI_SERVICE_READY_EVENTID` | 0x0001 |
| `WMI_READY_EVENTID` | 0x0002 |
| `WMI_START_SCAN_CMDID` | 0x3001 |
| `WMI_SCAN_EVENTID` | 0x3001 |
| `WMI_MGMT_RX_EVENTID` | 0x7001 |

`wmi-tlv.h`: `struct wmi_tlv { __le16 len; __le16 tag; u8 value[]; }` —
**length comes first**. Tag values computed from `enum wmi_tlv_tag`
(`LAST_RESERVED = 15`, `FIRST_ARRAY_ENUM = 16`, `LAST_ARRAY_ENUM = 31`):

| tag | value |
|---|---|
| ARRAY_UINT32 | 16 |
| ARRAY_BYTE | 17 |
| ARRAY_STRUCT | 18 |
| ARRAY_FIXED_STRUCT | 19 |
| STRUCT_MGMT_RX_HDR | 44 (0x2c) |
| STRUCT_INIT_CMD | 74 (0x4a) |
| STRUCT_RESOURCE_CONFIG | 75 (0x4b) |
| STRUCT_START_SCAN_CMD | 77 (0x4d) |

`wmi-tlv.c: ath10k_wmi_tlv_op_gen_init()` emits INIT(74) with
`struct wmi_tlv_init_cmd` (`abi_version` = 6 u32 + `num_host_mem_chunks`)
and `struct wmi_tlv_resource_config` (**44 u32** — count the fields, it is
not 41), then an empty ARRAY_STRUCT(18). ABI: `VER0 = 0x01000000`,
`VER1 = 53`, `VER_NS0 = 0x5F414351`. Only `abi_ver0` and `abi_ver_ns0`
are validated by the driver (`wmi-tlv.c:1348`).

`wmi-tlv.c: ath10k_wmi_tlv_op_gen_start_scan()` emits START_SCAN(77) with
`struct wmi_tlv_start_scan_cmd` = `struct wmi_start_scan_common` (16 u32) +
6 u32 + two 6-byte MACs = 100 bytes, then ARRAY_UINT32(channel frequencies),
ARRAY_FIXED_STRUCT(ssids), ARRAY_FIXED_STRUCT(bssids), ARRAY_BYTE(ie).
`wmi.h` scan flags: `WMI_SCAN_FLAG_PASSIVE 0x1`,
`ADD_BCAST_PROBE_REQ 0x2`, `FILTER_PROBE_REQ 0x20`. The TLV path XORs
`FILTER_PROBE_REQ` into the flags, so a host that emits the post-XOR value
0x20 asks for beacons/probe responses only.
Scan event types: `STARTED 1`, `COMPLETED 2`, `BSS_CHANNEL 4`,
`FOREIGN_CHANNEL 8`, `DEQUEUED 0x10`, `PREEMPTED 0x20`.

`wmi-tlv.c: ath10k_wmi_tlv_op_pull_mgmt_rx_ev()` takes
`WMI_TLV_TAG_STRUCT_MGMT_RX_HDR` (44) → `struct wmi_tlv_mgmt_rx_ev`
(channel, snr, rate, phy_mode, buf_len, status, rssi[4] = 10 u32 = 40 bytes)
and `WMI_TLV_TAG_ARRAY_BYTE` (17) → the raw 802.11 frame. `buf_len` is the
frame length; `rssi[0]` is the signal. Our beacon parser walks the standard
802.11 information elements for SSID (tag 0), DS parameter set (tag 3) and
RSN/WPA (tags 48 / 221).

## 4. WLFW / firmware side (already implemented in PicassoWlan.cpp)

`snoc.c: ath10k_qmi_wlan_cfg_send()` sends `target_ce_config_wlan`,
`target_service_to_ce_map_wlan` and `target_shadow_reg_cfg_map` only **after**
`WLFW_FW_READY_IND (0x21)`; our `wlfw_step()` cases 7→8→9 follow that order.
`WLFW_WLAN_CFG = 0x0023`, `WLFW_WLAN_MODE_REQ = 0x0022` with
`QMI_WLFW_MISSION_V01 = 0`. Once mission mode is acknowledged the firmware
owns its CE configuration and the host may program the registers above.

## 5. Not yet sourced / known gaps

* Association and the data path: `WMI_VDEV_CREATE`/`VDEV_START`/`PEER_CREATE`/
  `PEER_ASSOC` and the HTT data pipes (CE4/CE5/CE1) are **not** implemented.
  `PicassoWlan.cpp::picasso_wlan_transmit()` still forges DHCP/ARP locally.
* MSI setup from WLFW (`wlfw_msi_cfg`) is not used: this HIF is polled.
* `struct ath10k_hw_params.shadow_reg_support` is declared but never
  initialised in `hw.c`; whether a given kernel writes 0x3c or the shadow
  window must be confirmed on hardware. Our driver writes both.
* The `rri_on_ddr` path (`ce_rri_low/high` at 0x24C004/8) is not used; we
  detect completion from `desc->nbytes` exactly like
  `_ath10k_ce_completed_recv_next_nolock_64()`.
