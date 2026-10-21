# C++ reconstruction status — 2026-10-19

Implemented and built on Windows (MinGW UCRT64, C++20, OpenSSL 3.6.2):

- Two-sided DH public-key replacement, TCP stream reassembly, AES translation.
- Canonical 128-byte UDP bank extraction and conservative unique-candidate binding.
- XTEA downstream decryption, upstream prefix removal, bounded UDP structural checks.
- Player validation: finite coordinates, chronology, freshness, continuity,
  optional world/speed bounds, decoder errors and identity provenance.
- Capture journals, replay, multiple capture windows, fingerprint transitions,
  interval bounds, censored gaps and candidate-period statistics.
- Linux/OpenWrt AF_PACKET IPv4 capture source with kernel timestamps,
  drop counters and optional TCP REDIRECT original-destination relay.
- Shared Windows/Linux IPv4 UDP fragment reassembly before port filtering and
  decryption; bounded pending/recent caches, overlap rejection, timeout diagnostics.
- Native UE player decoder: bounded bitstreams, packet/bunch framing, reliable ordering,
  partial reassembly, repmove4 coordinates/yaw, two PlayerState catalogs and explicit
  Character MoveHandle/PlayerState/Owner links. Catalogs embedded in the executable.
- Automatic player output and validation during capture/replay; native decode-players CLI;
  bounded latest-player WebUI snapshots. Reference Python retained for development only.
- Windows ICS/Mobile Hotspot forwarded UDP sniff copies, scoped transparent TCP
  routing into the existing relay, adapter/DLL/filter preflight, QPC timestamps,
  bounded queues and explicit loss events. PowerShell sharing/capture helper.

Verified:

- 358 Python-reference differential vectors plus 36 nonempty coordinate samples
  compared by slot/time; UTF-8/UTF-16, arrays, initial transforms, reliable wrap/reordering.
- Encrypted replay-to-player pipeline; channel closure/reuse, epoch/gap resets,
  malformed movement rejection and continuity rejection after decoder errors.

- 531 native assertions and 6,782 original-assembly differential checks.
- 8,321 shared packet/routing/discovery-range assertions; WinDivert DLL read-only
  preflight, filter and checksum tests; all ten CTest targets pass. Real driver not opened.
- Native C++ loopback WebUI, neighbor/ICMP discovery, capture status and cancellation.
  HTTP integration and neighbor-only discovery pass. Browser review was denied
  by the local-origin permission gate; active ICMP scanning is not field-verified.
- Fake WinDivert DLL fixture feeds 500 encrypted synthetic UDP packets through
  the actual capture thread, queue, journal and bank broker. All 500 decrypt and yield 992 native player samples;
  graceful shutdown and receive-error gap recording pass without network I/O.
- Actual loopback TCP client/relay/server: fragmented hello, both substitutions,
  encrypted server hello, bank observation and bidirectional translation.
- Automatic TCP tests: 32 loopback connections, 24 concurrent; mock driver tests map
  three destination ports with independent aliases and restore replies, bypass local/existing flows.
- 500 synthetic UDP datagrams, 5 banks, 4 transitions, duplicate/gap/epoch checks,
  separate-session comparison, 7 player-quality cases and two local capture windows.
- Reference UE decoder processed 500 valid empty-bunch packets without errors.
  The packets have no player payload, so zero player samples is expected.

Remaining limits:

- Player coordinate/identity runtime is fully C++; full UE semantics are not ported.
  Health batch protocol, inventory/weapons, loot and complete upstream self-state are
  outside this port. Unknown native fields are diagnosed, never guessed. See PLAYER_DECODING.md.
- Linux/OpenWrt build and interface capture remain unverified. Local WSL cannot
  start because D:/WSL/Ubuntu/ext4.vhdx is missing.
- No real game traffic captured; no measured real bank period or player accuracy.
- Synthetic evidence is labelled and excluded from real reports by default.
- Original two-response bank handshake matcher is not ported; structural binding is used.
- Shared TCP now defaults to remote port 65010 for the selected client; bypasses other ports and the local subnet.
  Explicit --tcp-port 0 retains the original all-port mode. Existing DH sessions continue key translation after bank acquisition.
  Original destinations come from SYN mappings. Bind port 0 first, then use the actual port
  in temporary firewall rules and return filters. Native CLI and WebUI share this lifecycle.
  Only explicit PowerShell EnableSharing modifies ICS; tests modify neither ICS nor firewall.
- Original-style TGCP mode 3 classification, 1-second prefix deadline, server-first/EOF
  passthrough and no downgrade after DH commitment. Windows capacity is 256 relay connections.
- Actual Windows hotspot/ICS forwarding and driver injection are unverified.
- No IPv6 or pcap-file ingestion. IPv4 UDP fragments are reassembled; actual
  fragmented hotspot traffic and Linux/OpenWrt deployment remain unverified.

See README.md for operation and VALIDATION.json for verification scope.

## 0.4.2 UDP fragmentation

The native reassembler and mock capture tests compare unfragmented datagrams with
out-of-order fragments and copies arriving both before and after completion. UDP
payload bytes, decrypted bytes and 992 player poses match for all 500 datagrams.
An incomplete datagram expires even while WinDivertRecv is blocked without traffic.
WebUI API tests also pass fragmented upstream self and downstream player packets.
See UDP_REASSEMBLY.md for counters, cache limits and the historical-log conclusion.


## 0.3.2 战斗流状态

`BattleTracker` 消费已解密 UDP、原生解码器的首次 Welcome、验证后的人物样本和会话/绑定事件。仅 Welcome `/Game/...` 可确认战斗流；按明文到达顺序选择当前已确认流，上下行均刷新单调时钟。采集循环独立 tick，超过 15 秒进入 idle 并清空当前显示。每流保留最多 128 个槽位快照，切流不混合人物；新 epoch、重绑、断档和新采集均隔离旧状态，迟到的旧 epoch 不会撤销新流。

`battle_welcome` / `battle_state` 写入 JSONL，WebUI 展示当前地图、flow、epoch 和状态。离线命令按日志时间推进状态，`decode-players` 另写 `battle-events.jsonl`。空闲和切流不操作 TCP 中继。11 项 CTest 通过；已有真实日志离线回放识别两个 Welcome 与两次空闲，既有 55,206 条人物样本和 1,096 条解析诊断数量不变。


## 0.3.3 实时速率显示

`LiveRates` 在 WebUI 的事件观察器内统计最近 1000 ms：成功下行 UDP 明文、当前已确认战斗流的坐标源包（按 sourcePacketOrdinal 去重）及样本条数。后端使用单调时钟，固定一秒分母，首次启动不会以极短运行时间放大速率；轮询不重置计数，静默时查询会淘汰过期计数。最多保留 128 个槽位的独立频率、更新年龄和相邻源时间间隔。切流/epoch/空闲清空坐标窗口，采集结束/重启清空全部窗口。

前端每 500 ms 刷新 API，增加三张速率卡片及人物表的更新速度列。12 项 CTest 通过，包括窗口边界、同包多人物去重、轮询隔离和合成采集 HTTP 速率校验。未启动真实抓包，未改变 TCP 中继和协议解码行为。

## 0.3.4 解析前缀与错误隔离

`ue::movement` 以 `complete=false` 返回未知 0x10 对象前的已知字段和停止位置，既不消耗对象也不读取后续槽位。上层只在已确认移动通道使用该前缀；h21 后有其他属性时使用内容块长度边界结束当前块，并标注解析覆盖范围。实际截断等异常仍拒绝整帧。

每通道独立维护语义错误计数，每流另存下行包级错误计数；每条人物轨迹只取相关增量，原全流总错误量保留为统计。非可靠 bunch 与可靠 bunch 使用同样的局部异常隔离。`player_decode_warning` 单独记录不影响已知坐标连续性的停止；真实错误仍用 `player_decode_error`。WebUI 同时展示合理性状态和部分解析覆盖标记。

SDK 仅用于核对反射字段，未把内存偏移用作网络长度，也未据此猜测 0x10 格式。离线回放结果、SDK 证据与仍存限制见 `SDK_ANALYSIS.md`。

## 0.3.5 解析性能

`Bits::read` 在原边界检查后按字节合并 LSB 位流，最多读取九字节处理未对齐的 64 位字段；零位读取和 64 位掩码分别处理，避免无效移位。`slice` 对齐时直接复制、未对齐时组合相邻字节并遮蔽尾位；`append` 按字节 OR 拼接，保留原实现对目标尾部的语义。终止位按非零字节定位。

人物样本和事件使用对象逐字段赋值与移动，不再用大型 initializer_list 反复复制 JSON 树；坐标及属性中间值使用只读引用。协议分支、重组排序、校验阈值、日志每事件 flush 和 WebUI 刷新间隔均保持原有行为。新增开发用 `radaer-player-benchmark` 构建目标；381,995 项位流检查及全日志七文件哈希一致性验证见 `PERFORMANCE.md`。

## 0.3.6 静默处理 0x10

已确认移动通道遇到 0x10 时仍在未知对象前结束解析并输出已知坐标，但不再调用 warning 诊断。帧覆盖字段、校验逻辑及其他诊断保持原有行为。该调整同时作用于实时采集、WebUI 和离线回放，不依赖前端隐藏日志。
