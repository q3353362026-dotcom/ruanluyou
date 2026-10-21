# 0.4.2 UDP 分片重组与验证

日期：2026-10-19。本次只做编译、内存/模拟驱动测试和已有日志分析，没有启动真实抓包或改变共享网络、路由及防火墙。

## 处理位置

Windows `NETWORK_FORWARD` 的 UDP 副本与 Linux `AF_PACKET` 共用 `Ipv4UdpReassembler`：

```text
采集 IPv4 副本 → IP 分片重组 → UDP 端口过滤 → bank 绑定/解密
              → UE packet/bunch 重组 → 人物/本人坐标 → 验证与雷达
```

原先采集层跳过带 MF 或 fragment offset 的 IPv4 包，Windows 还会产生连续性缺口。现在等待完整分片后只交付一次 UDP 数据报；未拆分的包立即通过。这里补的是 IP 层，既有 UE bunch 分段拼接继续保留。

重组只处理副本，不修改或重新发送实际 UDP 流量。Windows 的过滤条件使用 `ip.Protocol == 17`，包含没有 UDP 头的后续分片。没有改用只匹配 `udp` 的过滤条件。`WINDIVERT_FLAG_FRAGMENTS` 仅影响 NETWORK 层入站，在 NETWORK_FORWARD 层无效，见 [WinDivert 官方说明](https://reqrypt.org/windivert-doc.html#divert_open)。

## 边界与限制

- 按源 IPv4、目标 IPv4、IP ID 隔离，同一实例只接收 UDP；Windows 继续按所选客户机 IP 过滤，Linux 绑定所选接口。
- 允许乱序；完全一致的重复分片不延长存活时间；重组后保留有界的近期载荷缓存，识别迟到副本。
- 重叠但不一致的范围、互相矛盾的末尾、异常长度、非法 flags 或 UDP 长度不匹配会拒绝整个数据报，并暂时隔离该 IP ID。
- 最多 128 个待重组包、每包 128 个分片、4 MiB 待重组载荷；从首个分片收到时起 5 秒超时。另有最多 128 个/4 MiB 的近期完成载荷缓存，保存最多 5 秒或提前淘汰。缓存上限不含容器和 IP 头的小量开销。
- 超时或冲突沿用现有 capture_gap 机制，会重置连续性；Windows 同时清理待处理队列，目前不是按 UDP 流隔离。
- 不凭空补缺失字节，不为完整 UDP 数据报额外排序，不扩大人物合理性阈值，不改变 `0x10` 前停止解析的行为。没有增加 UDP 校验和验证。
- 近期缓存淘汰后才抵达的重复分片仍可能成为待重组项。短时间内同地址复用相同 IP ID 且片段内容相同，无法仅凭 IP 分片区分新包与重复包；本实现将与近期完整载荷一致的片段视为副本。
- Linux 分支已经接入共用实现，但本机 WSL 磁盘缺失，尚未完成 Linux/OpenWrt 编译及真实接口抓包验证。IPv6 不支持。

## 测试证据

1. 原生单元测试 106 项断言：三片的全部六种排列、重复/迟到重复、缺片超时、重叠与末尾冲突、地址/IP ID 隔离（包括未分片包复用 ID）、容量边界、IPv4 options、最大长度和重建 IP 头校验和。
2. 实际 Windows 采集线程使用 mock DLL 读取 500 个加密合成 UDP 包。拆成两片，倒序输入，并分别在完成前后重复尾片：共 2,000 次分片输入 → 500 个重组包，1,000 个副本被去重。恢复出的原始 UDP 载荷、解密明文、992 条人物的槽位/XYZ/朝向/队伍均与未拆分基线一致；没有 capture_gap。
3. 仅输入一个尾片后停止提供流量：即使接收线程阻塞，也会在 5 秒后产生一次重组超时/连续性缺口；没有输出残缺 UDP 或人物样本。
4. WebUI API 的上下行测试改用乱序分片，仍得到 37 条远端样本与 5 条本人样本；统计与最终摘要可通过 API 读取。
5. 16 项 CTest 全部通过。没有加载真实 WinDivert 驱动；HTTP 和 TCP 测试只在本机回环运行。

测试入口：`native/tests/ipv4_reassembly_tests.cpp`、`shared_capture_mock.py` 和 `webui_integration.py`。可用 `ctest --test-dir native/build --output-on-failure` 复现。

## 现有真实日志能说明什么

最近已有日志仍是 `web-1792311484292-d4d2ef4b.jsonl`，来自 0.4.0：

- SHA-256：`b196e0fa26e7f2fee027ee73f8a2ae070bce9fd520ccebaa52f8dc5075358903`。
- 10,684 条 `udp_raw`，UDP 载荷最大 1,350 字节；没有超过 1,472 字节的已记录载荷。
- `invalidPackets=0`、`discardedByApplication=0`、`queueOverflowEvents=0`，`capture_gap` 为 0；`kernelDrops=null`，系统丢包未知。
- 旧日志只有进入解析流程后的 UDP 载荷，没有原始 IP 头或遗漏的分片，不能逆推出当时的分片，也不能恢复已丢失的包。按通常 20 字节 IPv4 头估算，已记录最大包为 1,378 字节；这不能证明路径 MTU，也不能排除未记录的大包或缺片。

因此，已经验证“完整分片到齐时，新实现不会因拆分丢掉人物数据”；**尚无证据将真实 10–20 Hz 坐标更新或剩余解析错误归因于 IP 分片**。不能宣称此次更新已经把真实坐标提高到 60 Hz。之前 0.4.1 的解析错误修复及原版对照结果仍见 LOG_REVIEW_041.md。

## 下次采集怎么判断

WebUI 实时速率区域增加“UDP 分片重组”，每秒更新统计；停止后显示最终摘要。计数范围是采集点看到的客户机 IPv4 UDP，在 UDP 端口和战斗流筛选之前，不能把其他流的重组数直接视为战斗包数。

| 计数/字段 | 含义 |
|---|---|
| `ipv4FragmentsReceived` | 收到的 IP 分片数，包含重复/异常片 |
| `ipv4DatagramsReassembled` | 成功恢复的完整 UDP 包数 |
| `ipv4DuplicateFragments` | 忽略的完全一致副本数 |
| `ipv4ReassemblyPending` | 正在等待其他片的包数；停止时未满 5 秒的缺片仍会显示等待 |
| `ipv4ReassemblyTimeouts` | 缺片超过 5 秒的包数 |
| `ipv4ReassemblyRejected` | 发生冲突、长度错误或超限的拒绝次数；隔离期内继续到达的片不会重复增加此计数 |
| `ipFragmentCount` | 每条 `udp_raw`/`udp_plaintext` 的唯一分片数；1 为未由本组件重组 |
| `reassemblyDelayMs` | 从最早分片捕获到完成分片捕获的间隔，保留原始最早捕获时间；不是公网延迟 |

统计保存在 `capture_stats` 和停止时的 `udp_reassembly_summary`；重组标记跟随原始及明文 UDP 日志，离线 replay 也保留这些字段。可通过 `sourcePacketOrdinal` 将人物样本关联到源 UDP 包。

分片数为 0 表示这个采集点没有交给应用 IP 分片，不能排除上游已经重组或丢弃。若有成功重组且战斗流明文带 `ipFragmentCount > 1`，说明这项修复确实参与该对局的数据恢复；若超时/拒绝持续增长，则需要结合连续性缺口检查采集丢失或异常分片。若全部指标正常而坐标仍约 10–20 Hz，应继续检查协议复制频率及剩余字段解析，不能靠重组增加服务器未发送的坐标。
