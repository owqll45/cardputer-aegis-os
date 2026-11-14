# ZD 协议说明 v1.2（增量版）

状态：**冻结**。v1.2 只做加法，v1.1 的字段/语义全部保留；只认 v1.1 的老固件可继续工作
（未知 type 按 v1.1 规则忽略）。所有整数字节序、CRC、去重键与 v1.1 完全一致，见
`ZD协议说明-v1.1.md` §0。

本版改动三件事：

1. 新增 **ZT_TIME（type = 8）**：时间同步（UTC + 可信度来源），ADV 与 LoRa 网双向。
2. 新增 LoRa 侧私有 port **0x0101（ZD_TIME）**，让时间跨 LoRa 空口传播。
3. 约束 **ESP-NOW 只服务 Cardputer ADV ↔ 就近 C3**；C3 之间的帧一律不进 ZD 层，改走 LoRa。

---

## 1. ZT_TIME（type = 8）：时间同步

### 1.1 帧头

| 字段 | 值 |
|------|-----|
| type | 8 |
| flags | 0（不需要 ACK） |
| dst | `0xFFFFFFFF`（广播） |
| ttl | 3 |
| frag/frags | 0 / 1 |
| msgid | 0 |

### 1.2 payload（6 B，向后兼容：老固件按未知 type 丢弃）

```
[0..3] uint32 LE  utc     // UTC 秒（自 1970-01-01），0 = 无效
[4]    uint8      src     // 0=未同步 1=GPS 2=手动
[5]    uint8      acc     // 精度（秒）：GPS 建议 1，手动建议 5
```

- `plen` 必须 ≥ 5 才处理；第 6 字节缺失按 `acc = 5`。
- 接收方采纳规则（推荐实现，也是本工程固件的行为）：
  - 本地未同步 → 采纳任意 `src != 0` 的帧；
  - 本地为手动(2) → 只有 GPS(1) 能覆盖；
  - 本地为 GPS(1) → 不再被手动覆盖，可被另一条 GPS 帧微调。
- **谁都可以转发**：任何一台只要拿到可信时间，就每 30 s 广播一次 ZT_TIME，
  并在同一时刻向 LoRa 侧发一帧 ZD_TIME（见 §2）。广播前必须把 `utc` 按本地
  单调时钟递推（`utc + (now - 校准时刻)/1000`），不要直接转发收到的原值。
- ADV 手动设时间：直接发 `src = 2` 的 ZT_TIME；C3 收到后采纳并反向广播到 LoRa 网。

### 1.3 示例帧（ADV → 广播，UTC = 2026-11-15T04:00:00Z = 0x6918F900，src=2，acc=5）

```
5a 44 01 08 68 3f f4 7d ff ff ff ff 03 00 00 00 00 00 00 06 00 <crc16> 00 f9 18 69 02 05
```

> `from` = 发送方 NodeNum（此处示例为 ADV `0x7DF43F68`）。CRC 按 v1.1 §11 算法
> 覆盖 `[0..21] + payload`，示例中的 `<crc16>` 请按实现复算，不要照抄。

---

## 2. LoRa 侧 ZD_TIME（Meshtastic 私有 port 0x0101）

时间要跨 LoRa 空口传播，用的是 Meshtastic **Data 包**里的私有 portnum：

```
Data（protobuf）：
  field 1 (varint) portnum = 0x0101   // 257，私有应用段 0x100..0x1FF
  field 2 (bytes)  payload(6 B)：
        [0..3] uint32 LE utc
        [4]    uint8    src   // 0=未同步 1=GPS 2=手动
        [5]    uint8    acc   // 秒
```

- 帧格式同普通 Meshtastic 广播：`to=0xFFFFFFFF`、`from`=本机 NodeNum、`hop_limit=7`、
  `channel hash` 取当前频道（`awa`）；外层 AES-CTR 与文本包完全一致。
- 官方 Meshtastic 设备收到不认识的 portnum 会直接忽略，不弹消息、不影响其他功能。
- 只在 `src != 0` 时发送；30 s 一次，最多重试 1 次。
- 发射安全策略优先：`tx_allowed == false`（AA:D0 无天线）时**不发**，只接收。
- 接收侧：`portnum == 0x0101 && plen >= 5` → 走与 §1.2 相同的采纳规则。

### 2.1 与 v1.1 的边界

- 不占用原有 type/flags；ZT_TEXT/ZF_LORA/ZF_WANT_ACK 行为不变。
- 时间包不走 ZD 桥的回传（不会产生 ZT_LTEXT），避免把时间当聊天文本显示。
- ADV 不需要实现 LoRa 时间：它只需要 ZT_TIME；LoRa 侧时间由 C3 桥自行收发。

---

## 3. ESP-NOW 作用域收缩（v1.2 硬约束）

用户要求：**LoRa 设备之间只用 LoRa 通信；ESP-NOW 只用于 Cardputer ADV 与就近 C3。**

实现约束（C3 侧）：

- C3 收到 ESP-NOW 帧后，若 `header.from` 的**高字节为 0x7D**（C3 系 NodeNum，由
  `50:78:7D:xx:xx:xx` MAC 推导），整帧丢弃，不解析、不中继、不回 ACK。
- ADV 的 NodeNum 高字节为 `0xAC`（Cardputer MAC 推导），不受影响。
- 因此 ZD 的 ttl 中继在 C3 之间**不再生效**（原来靠 ESP-NOW 多跳）；跨 C3 的文本改走
  LoRa 桥（`ZT_TEXT + ZF_LORA` 单播到就近 C3 → LoRa 广播 → 另一台 C3 用 ZT_LTEXT 回给
  它身边的 ADV）。

### 3.1 受影响能力清单

| 原能力 | v1.2 状态 | 替代路径 |
|--------|-----------|----------|
| C3↔C3 ESP-NOW 文本 | 关闭 | LoRa 文本（`ZF_LORA` → 空口 → `ZT_LTEXT`） |
| C3↔C3 节点表（ZT_NODETAB）跨机同步 | 关闭 | LoRa NodeInfo（官方格式，5 min / 30 s 抖动） |
| ZD 多跳中继（ttl 3） | 仅 ADV↔C3 单跳有效 | LoRa 自带中继（hop_limit 7） |
| ADV 经 C3 访问远端 C3 | 需单播给就近 C3 | 桥把文本发到 LoRa，空口覆盖全组 |
| 跨 C3 的 ACK（ZT_ACK） | 不再跨机 | LoRa TX-DONE 语义（`ZT_LNACK` 只对本机桥有效） |
| ADV↔就近 C3 的心跳/电量/节点/时间 | 不变 | — |

---

## 4. 空口档位（不属于帧格式，但必须全网一致）

v1.2 起默认档位从 **LONG_SLOW** 改为 **LONG_FAST**，三台设备与官方 Meshtastic 节点
必须一致，否则互相解不出来（帧能收到但 CRC/同步失败）。

| 档位 | SF | BW | CR | LDRO | 预测空口（100 B） |
|------|----|----|----|------|------------------|
| LONG_FAST（默认） | 11 | 250 kHz | 4/5 | 0 | ≈ 1.4 s |
| LONG_SLOW（兼容） | 12 | 125 kHz | 4/8 | 1 | ≈ 5.4 s |

- 频道名、PSK(32 B)、hash(`0x17`)、频率(433.000 MHz) 全部不变，所以**协议层零改动**。
- C3 设置页可运行期切换（BOOT 键：超长按进设置 → 短按切项 → 长按改值），不写 NVS。
- 切换后必须整组一起改；单台改动表现为“对端静默”。

---

## 5. 兼容性检查清单（ADV 侧新增项）

- [ ] 解析 `ZT_TIME`：plen ≥ 5，按 §1.2 采纳；未同步时不要把 0 当时刻。
- [ ] 手动对时：发 `src=2` 的 ZT_TIME，等 C3 的下一帧 ZT_TIME 回显（可视为已扩散）。
- [ ] 不要因为 `ZT_TIME` 而回 ACK，也不要把它塞进聊天列表。
- [ ] 若 ADV 自己能拿到 GPS/手机时间，可以直接当时间源（`src=1`），C3 会转发到 LoRa。
- [ ] v1.1 的全部检查项继续有效。
