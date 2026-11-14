# ZD 协议说明 v1.1（最小可用版）

适用：C3 侧（本工程 `c3msg`）与 Cardputer ADV 面板对接。
状态：**冻结**。v1.1 只新增内容，不改动 v1 任何既有字段/语义，老固件可继续工作。
所有多字节整数均为 **小端（LE）**；有符号数为二进制补码；文本为 **UTF-8，不带结尾 NUL**。

---

## 0. 帧结构总览

```
偏移  长度  字段
0     2     magic 'Z' 'D' (0x5A 0x44)
2     1     ver = 1（v1.1 仍为 1，不升版本号）
3     1     type
4     4     from    —— 本帧的 ZD 发送方 NodeNum
8     4     dst     —— 目标 NodeNum，见 §7
12    1     ttl     —— 发送方填 3，每次中继减 1
13    1     flags
14    2     seq     —— 发送方递增序号，去重键
16    2     msgid   —— 应用层消息 id
18    1     frag
19    1     frags
20    2     plen    —— payload 字节数，≤ 226
22    2     crc16
24    plen  payload
```

- 整帧长度必须等于 `24 + plen`，否则丢弃。
- CRC：CRC-16/CCITT，多项式 `0x1021`，初值 `0xFFFF`，先算 `[0..21]` 再算 payload。
- 最大帧长 250 字节（24 + 226），正好等于 ESP-NOW 上限。
- 去重：接收方按 `(header.from, seq)` 去重；每来源 32 条窗口。**重传必须换新 seq**。
- **seq 起点**：开机后必须用**随机 16 位**起点（如 `esp_random() & 0xFFFF`），之后严格递增。固定从 0 开始时，设备重启后的前若干帧会撞进对端残留的去重窗口，被当成重复帧丢弃（表现为“链路通了但对方没反应”）。三台设备同一套固件，统一遵守。
- 中继：`ttl>1` 时 `ttl-1`、置 `ZF_FORWARDED`、重算 CRC、原样广播；`ttl<=1` 停止（原帧之外最多再转发 2 次）。收到 `from == 自己` 的帧直接丢（广播回环）。

## 1. type 表

| 值 | 名称 | 方向 | 说明 |
|----|------|------|------|
| 1 | ZT_GAME | 双向 | 旧版游戏帧，v1.1 不涉及 |
| 2 | ZT_TEXT | 双向 | 文本；v1.1 增加 `ZF_LORA` 用法，payload 不变 |
| 3 | ZT_BEAT | C3→广播 | 心跳，见 §6 |
| 4 | ZT_NODETAB | C3→广播 | 节点表，每 15 s |
| 5 | ZT_ACK | 双向 | 确认，payload 固定 4 B |
| **6** | **ZT_LTEXT** | **C3→广播** | **LoRa 收到的文本回传给 ADV**，见 §3 |
| **7** | **ZT_LNACK** | **C3→广播** | **LoRa 转发失败通知**，见 §5.3 |

未知 type 一律忽略，不得报错、不得断开链路。

## 2. flags 表

| 位 | 名称 | 说明 |
|----|------|------|
| 0x01 | ZF_WANT_ACK | 发送方要求接收方回 `ZT_ACK` |
| 0x02 | ZF_FORWARDED | 已被至少一次中继置位（由中继方写，原发送方不得置位） |
| **0x04** | **ZF_LORA** | **仅用于 ZT_TEXT：请求 C3 把这条文本转发到 LoRa**，见 §4 |
| 0x08–0x80 | 保留 | 必须填 0；接收方忽略 |

未知 flag 位必须忽略（v1 行为已是如此）。

## 3. ZT_LTEXT（type = 6）：LoRa → ADV

```c
payload:
  [0..3]  uint32 LE  lora_from   // LoRa 原始发送方 Meshtastic NodeNum
  [4]     int8       rssi        // dBm，如 -65 => 0xBF
  [5]     int8       snr4        // SNR × 4，如 4.25 dB => 17
  [6]     uint8      hops        // Meshtastic hops_away，255 = 未知
  [7]     uint8      lflags      // 保留，v1.1 必须为 0
  [8..]   bytes      text        // UTF-8 原文，≤ 96 B，无 NUL
```

- `plen = 8 + strlen(text)`，上限 104。
- 帧头：`from` = **C3 桥自己的 NodeNum**（header.from 永远表示“谁发的这一帧 ZD”，不是 LoRa 的来源）；`dst = 0xFFFFFFFF`；`flags = 0`；`frag = 0`、`frags = 1`（本版不分片）。
- `seq` 由 C3 自己的递增序号产生；`msgid` = 该 LoRa 包 id 的低 16 位（仅用于关联/调试）。
- **不收 ACK、不回 ACK**。ADV 收到后只做展示，不得回 `ZT_ACK`。
- `text` 以 LoRa 空口实际内容为准。若原文带 `[A:xxxx] ` 前缀（见 §4.4），原样保留；ADV 可解析前缀用于显示来源，但不得因此丢弃消息。

## 4. ADV → LoRa：ZT_TEXT + ZF_LORA

### 4.1 必填字段

| 字段 | 值 |
|------|-----|
| type | 2 (ZT_TEXT) |
| flags | `ZF_LORA`，可叠加 `ZF_WANT_ACK` |
| dst | **必须单播为桥节点 NodeNum**（不能是 0xFFFFFFFF） |
| ttl | 3 |
| frag/frags | 0 / 1（v1.1 不做分片转发） |
| payload | UTF-8 文本，建议 ≤ 72 B |

**LoRa 侧目标在 v1.1 固定为广播（0xFFFFFFFF）**：`dst` 只用于在 ZD 网里选中桥节点，不表示 LoRa 收件人。向某个 LoRa 节点单播留到后续版本，届时会使用新的 flags 位并在 payload 前置 4 B 目标地址；v1.1 的实现忽略这些，不做猜测。

**广播 + ZF_LORA 是非法组合**：C3 收到 `dst=0xFFFFFFFF` 且带 `ZF_LORA` 的帧，只按普通文本显示/中继，**不转发到 LoRa**，并打印告警日志。这样做是为了避免多个 C3 同时听见广播后重复上空气。

桥节点怎么找：C3 每 5 s 广播 `ZT_BEAT`，其 `header.from` 即桥的 NodeNum。ADV 取最近一次 BEAT 的 `from` 作为 dst；有多个 C3 时由用户在面板选择其一。

### 4.2 长度与截断

- C3 上空口前把文本截到 **72 字节**（UTF-8 完整字符边界），超出追加 `..`。
- ADV 应自行限制在 72 B 以内，避免用户输入被截。
- 文本中的 `\r`、`\n`、`\t` 会被 C3 替换为空格（沿用 v1 行为）。

### 4.3 转发行为

1. C3 本地消息列表显示原文（不带前缀）。
2. 按普通广播规则在 ZD 网内中继（ttl/CRC 规则不变）。
3. 调 LoRa 层发 Meshtastic 文本包：`to = 0xFFFFFFFF`（广播），`from` 固定为 **C3 自己的 NodeNum**（不能被 ADV 伪造）。频道固定为 C3 当前频道（"awa"），ADV 无需关心。
4. AA:D0 无天线设备上 `ZF_LORA` 恒失败，走 §5.3 回 `ZT_LNACK`。

### 4.4 来源标记（空口文本前缀）

LoRa 空口文本 = `[A:xxxx] ` + 原文，例如：

```
[A:3f68] hello from cardputer
```

- `xxxx` = ADV 的 `header.from` 低 16 位，**4 位小写十六进制**。
- 前缀与一个空格计入 72 B 限额。
- 目的：Meshtastic 侧用户能看出这条文本源自某个 ADV；C3 收到自己发出的包（被其他节点转发回来）时据此抑制回环。

### 4.5 回环抑制（C3 义务）

C3 记录最近 8 个自己发出的 LoRa 包 id；收到文本包 id 命中时**不产生 ZT_LTEXT**，避免 ADV 看到自己刚发的消息。

## 5. ACK 语义

### 5.1 ZT_ACK（type = 5）

```
payload (4 B): seq(2, LE) + msgid(2, LE)
```

- 原帧带 `ZF_WANT_ACK` 时，接收方处理后回发。
- 回发的 `seq`/`msgid` 必须是**原帧的值**（不是 ACK 发送方自己的序号）。
- ACK 帧：`type=5`、`flags=0`、`msgid=0`、`dst=0xFFFFFFFF`（沿用 v1 广播行为）。
- 收到方按 `(seq, msgid)` 匹配自己的待确认记录；`header.from` 只用于显示。
- **语义 = “本跳已接收并处理”**，不是端到端送达。

### 5.2 ZF_LORA 的 ACK 时机

- 只有 LoRa 空口发送完成（SX1262 TX-DONE）后，C3 才回 `ZT_ACK`。
- LoRa 广播本身没有端到端 ACK；`ZT_ACK` 仅表示“已经发到空口”。
- C3 内部对空口失败最多重试 3 次（同一 LoRa 包 id），这些重试**不会**产生额外的 ZD ACK。
- ADV 侧等待建议：≥ 15 s（SF12 单帧空口约 4.6 s，加上排队）。v1.1 **不做自动重试**；超时由界面显示“未确认”，由用户决定是否重发。重发必须使用**新 seq**、建议沿用同一 msgid。

### 5.3 ZT_LNACK（type = 7）：转发失败

```
payload (5 B): seq(2, LE) + msgid(2, LE) + reason(1)
```

`reason` 取值：

| 值 | 含义 | ADV 界面建议 |
|----|------|--------------|
| 1 | TX disabled（无天线安全策略，如 AA:D0） | “未转发：设备发射已禁用（无天线）” |
| 2 | 发送队列满 | “未转发：忙，请稍后重发” |
| 3 | 空口超时（3 次重试均失败） | “未转发：空口超时” |
| 4 | 帧不被支持（非文本 / 空文本 / 非法目标） | “未转发：不支持的请求” |

- 仅在原帧带 `ZF_WANT_ACK` 时发送；否则静默失败（C3 仍打日志）。
- 帧头同 ZT_ACK：`flags=0`、`msgid=0`、`dst=0xFFFFFFFF`。
- ADV 必须把 `ZT_LNACK` 当作明确的失败，不能显示为已发送。

## 6. ZT_BEAT 的 v1.1 扩展（能力位）

v1 payload 4 B：

```
[0..1] uint16 LE 电池 mV
[2]    uint8     电量 %
[3]    uint8     role（1 = 节点/中继）
```

v1.1 允许 `plen = 5`，追加：

```
[4]    uint8     caps
                 bit0 = 1  本机具备 ZD↔LoRa 桥能力（LoRa 硬件已初始化）
                 bit1 = 1  当前允许发射（未命中无天线策略）
                 bit2..7   保留，填 0
```

- 老解析器只看前 4 B，多出的字节被忽略，向后兼容。
- ADV 仅当 `caps.bit0 == 1` 时才允许用户选择“经 C3 转发到 LoRa”；`caps.bit1 == 0` 时置灰并提示“设备发射已禁用”。
- 由于 bit1 会随设备状态变化，ADV 应以最近一次 BEAT 为准（5 s 刷新）。

## 7. 目标地址表示汇总

| dst | 含义 | 谁能处理 |
|-----|------|----------|
| `0xFFFFFFFF` | 广播（所有 ZD 节点） | 所有人处理并按 ttl 中继 |
| `0x00000000` | 保留 | 收到即丢弃 |
| 其他 32 位值 | 单播到该 NodeNum | 仅 `dst == 自己` 的节点处理，其余只中继 |

- NodeNum 与 LoRa 的 Meshtastic NodeNum 同一编号空间（C3 的 NodeNum 由 MAC 推导，如 `0x7DF331D0`）。
- 单播帧的 `ZF_LORA` 只会被被寻址的那台 C3 执行，天然避免多桥重复上空气。

## 8. 示例帧（可直接用于自测）

### 8.1 ADV → C3 单播转发（ZT_TEXT, ZF_LORA|ZF_WANT_ACK）

ADV `0x7DF43F68` → C3 `0x7DF331D0`，文本 `hi`，seq=`0x1234`，msgid=`0x5678`：

```
5a 44 01 02 68 3f f4 7d d0 31 f3 7d 03 05 34 12 78 56 00 01 02 00 81 3c 68 69
```

### 8.2 同一情形，广播 dst（应被拒绝转发，只显示）

```
5a 44 01 02 68 3f f4 7d ff ff ff ff 03 05 34 12 78 56 00 01 02 00 12 a5 68 69
```

### 8.3 C3 → ADV 回传 LoRa 文本（ZT_LTEXT）

C3 `0x7DF331D0`，LoRa 来源 `0x7DF43F68`，rssi=`-65`，snr4=`17`，hops=`1`，文本 `hello`，seq=`0x0102`，msgid=`0x9ABC`：

```
5a 44 01 06 d0 31 f3 7d ff ff ff ff 03 00 02 01 bc 9a 00 01 0d 00
3a 5e 68 3f f4 7d bf 11 01 00 68 65 6c 6c 6f
```

## 9. 兼容性约束（必须遵守）

1. 不改 magic、ver、24 B 头布局、CRC 算法、ttl/中继规则、去重键。
2. 只在未占用的 type/flags 值上扩展；收到未知 type/flag 必须忽略而非报错。
3. 老 C3（无 v1.1）收到单播 `ZF_LORA` 文本时，会按普通文本显示并回 `ZT_ACK`，但**不会**发 LoRa。ADV 应结合 BEAT `caps.bit0` 判断是否展示该功能，避免“假成功”。
4. 老 ADV 收到 `ZT_LTEXT` 会因未知 type 忽略，不影响其他功能。
5. v1.1 新增 type 不使用分片；旧分片规则（frag 0..7、frags ≤ 8、每片 ≤ 226、重组上限 512 B）保持不变。
6. AA:D0（无天线设备）必须保持 `caps.bit1 = 0` 且对 `ZF_LORA` 回 `ZT_LNACK(reason=1)`，任何情况下不得满功率发射。

## 10. ADV 侧对接检查清单

- [ ] 解析 24 B 头并对整帧校验 CRC16-CCITT（init 0xFFFF，覆盖 0..21 + payload）。
- [ ] 按 `(from, seq)` 去重；重发换新 seq。
- [ ] 用 BEAT 的 `from` 作为桥 dst，用 `caps` 决定是否开放“转发到 LoRA”。
- [ ] 发送 `ZT_TEXT|ZF_LORA` 时 `dst` 为单播桥地址、`ttl=3`、`frag=0/frags=1`；LoRa 目标为广播，无需另填。
- [ ] 待确认记录按 `(seq,msgid)` 匹配 `ZT_ACK` / `ZT_LNACK`；超时 ≥15 s。
- [ ] 显示 `ZT_LNACK` 的具体 reason，不把失败显示成成功。
- [ ] 接收 `ZT_LTEXT`：从 payload[0..3] 取真实 LoRa 来源，`rssi/snr4/hops` 仅用于展示，不参与路由。
- [ ] 对 `ZT_LTEXT` 不回 ACK。

## 11. CRC 自校验（参考实现）

按 §0 算法，对 `header[0..21] + payload`（**不包含** CRC 两个字节，长度 = 22 + plen）逐字节计算：

```python
def zd_crc(body: bytes) -> int:
    c = 0xFFFF
    for x in body:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c

hdr = bytes.fromhex('5a440106d031f37dffffffff03000201bc9a00010d00')
pl  = bytes.fromhex('683ff47dbf11010068656c6c6f')     # plen = 0x000d = 13
print('%04X' % zd_crc(hdr + pl))                      # 5E3A
```

三条例帧的实测值（在线路上按小端写，即低字节在前）：

| 示例 | 计算值 | 线上字节 |
|------|--------|----------|
| 8.1 单播 | `0x3C81` | `81 3c` |
| 8.2 广播 | `0xA512` | `12 a5` |
| 8.3 LTEXT | `0x5E3A` | `3a 5e` |

已用上表逐字节复核，三个例子与固件一致。若复算结果不同，通常是把 CRC 字段本身也纳入了覆盖范围、`plen` 数错（8.3 是 13 不是 14）、或从偏移 2 开始算。**去重与 CRC 都按本节为准，不要凭印象改示例字节。**
