# Windows 热点 / 网络共享采集

拓扑：`互联网 ← 主机上网网卡 ← Windows ICS/NAT ← 主机私有网卡/移动热点 ← 客户机`。
客户机必须实际通过主机上网，默认网关是主机的私有 IPv4。
两台机器仅连接同一台普通路由器，客户机流量不经过主机，不能使用此模式采集。

## 两种采集方式

| 模式 | UDP | TCP / 获取 bank |
|---|---|---|
| 默认被动模式 | 在 WinDivert NETWORK_FORWARD 读取客户机 UDP 副本；SNIFF + RECV_ONLY，不回注 | 不接管 TCP；可提供已知的当前 bank 文件，否则先保存密文 |
| `-RedirectTcp` | 同上 | 按客户机 IP 接入到服务端口 65010 的新 TCP 连接，自动获取服务器 IP；识别 TGCP mode 3 后执行 DH 替换、AES 翻译和 bank 提取，其他连接透传 |

这是 Windows 原生 C++ 采集入口，复用原有 bank broker、JSONL、原生人物解析和周期报告。
共享网络本身不会消除加密；被动模式没有 bank 就不能获得下行明文。
TCP 透明模式基于已有私有 TGCP/DH 协议实现，TLS 流量原样透传，不解密 TLS 或替换证书。

## 1. 查看网卡和共享状态

在项目根目录打开 PowerShell：

```powershell
native/tools/shared-network.ps1 -Mode Info
native/tools/run-native.ps1 network-info
```

脚本列出网卡索引、IPv4、邻居表、ICS 角色。ICS 的 Role 0 是上网上行，Role 1 是客户机侧。
邻居表只是发现线索，最终以客户机自身显示的 IP 和网关为准。
移动热点可能由系统单独管理，ICS COM 列表不保证反映全部热点状态；必须验证客户机能上网。

## 2. 主机向客户机共享网络

**Wi-Fi 热点：** 在 Windows“设置 → 网络和 Internet → 移动热点”选择上网来源、配置热点并打开，让客户机连入。
随后重新运行 Info，识别热点对应的私有虚拟网卡及主机地址。
脚本不创建 WLAN 配置，也不代填热点口令。

**有线共享：** 主机需要上网网卡与独立客户机侧网卡。客户机接后者，使用 DHCP。
可以在 Windows 网卡属性“共享”页配置，也可以用下列显式 ICS 命令。
数字 18、19 只是示例，必须换成 Info 中确认的索引：

```powershell
native/tools/shared-network.ps1 -Mode EnableSharing -PublicIfIndex 18 -PrivateIfIndex 19 -WhatIf
native/tools/shared-network.ps1 -Mode EnableSharing -PublicIfIndex 18 -PrivateIfIndex 19
```

EnableSharing 需要管理员，会修改 Windows ICS 并可能重新配置私有网卡地址；配置在采集结束后保留。
脚本拒绝覆盖其他已开启的共享组合；如私有端启用失败，只回滚本次新开启的公共端。
启用后重新运行 Info，更新客户机 DHCP 地址，确认其网关和网络连通性。
不要把 VMware VMnet8 / Hyper-V 默认交换机自带的 NAT 自动当成已配置的 Windows ICS。

## 3. 预检与启动

需要 64 位构建、管理员权限，以及成套的 WinDivert 2.2+ DLL / WinDivert64.sys。
默认使用仓库 `runtime/release_resources`，不另装 Npcap。
可用 `-WinDivertDll` 指定其他成套驱动的绝对 DLL 路径。
预检只加载 DLL 检查导出、编译过滤器和验证网卡/IP，不打开驱动、不修改网络或防火墙。
它不能证明驱动签名能被内核接受，也不能证明客户机流量确实经过该主机。

以下统一是示例：私有索引 42、客户机 192.168.137.2。服务器地址自动获取，TCP 服务端口暂时固定为 65010。
**它们不是本机探测结果，必须替换为实际配置。** 未设置 HostIp 时仅在私有网卡具有唯一非 APIPA IPv4 时自动采用。

```powershell
native/tools/shared-network.ps1 -Mode Preflight -PrivateIfIndex 42 -ClientIp 192.168.137.2
New-Item -ItemType Directory -Force captures | Out-Null
native/tools/shared-network.ps1 -Mode Capture -PrivateIfIndex 42 -ClientIp 192.168.137.2 -Seconds 300 -Device client-pc -Out captures/shared-passive-001.jsonl
```

已有当前 bank 时加 `-BankFile captures/banks/<fingerprint>.bin`；可以传多个候选文件。
可加 `-UdpPort` 限制远端 UDP 端口，未指定时采集该客户机的全部 IPv4 UDP。

需要从新 TCP 握手获取 bank 时：

```powershell
native/tools/shared-network.ps1 -Mode Preflight -PrivateIfIndex 42 -ClientIp 192.168.137.2 -RedirectTcp
native/tools/shared-network.ps1 -Mode Capture -PrivateIfIndex 42 -ClientIp 192.168.137.2 -RedirectTcp -Seconds 600 -Device client-pc -Out captures/shared-dh-001.jsonl -SaveBanks captures/banks
```

启动后再在客户机新建游戏连接，已建立的 TCP 连接不会被接管。
默认仅匹配远端 TCP 65010，不限定服务器地址；其他端口、同网段和本机地址绕过。
`TcpPort` 默认 65010（启用 RedirectTcp 时）；显式 `-TcpPort 0` 恢复全端口。`TcpServer` 可进一步限定地址。`ListenPort` 默认 0（系统分配），也可显式指定非零端口。
先绑定并持有实际监听 socket，再由 C++ 为“客户机 IP → 主机私有 IP:实际监听端口 + 当前 exe”创建临时入站规则，随后开启 TCP 过滤器。
WebUI、PowerShell 和直接 CLI 共用这一流程。正常退出、Ctrl+C 或启动异常时自动清理；强制杀进程/断电可能留下 `RadaerNativeShared-*` 规则。
仅在已手动配置防火墙的环境才使用 CLI `--manage-firewall false`，它不会关闭或放宽系统防火墙。
预检中 `returnFilterPendingBind: true` / `tcpReturn: null` 表示动态端口尚未绑定；启动后 JSONL 的 `tcp_listener_started` 和页面显示实际地址。

识别遵循原版客户端前缀嗅探：非目标 magic/命令/mode、公钥无效、服务器先发、前缀等待超过 1 秒或未识别前 EOF 都进入透传，已缓存字节按方向完整送出。只对本实现支持的 mode 3 hello 执行替换。替换后禁止降级透传，握手/解析失败关闭当前连接。
捕获结束会关闭中继连接，客户机需要重连；不要把窗口间重连当作 bank 定时轮换。

## bank 获取后与战局切换

公钥只在 DH 握手阶段替换一次；后续不会重复替换公钥，但必须继续解密/重加密转发，因为客户端与服务端使用不同的会话密钥。完全不替换公钥可以普通中继，却无法从加密的 TCP 负载取得 bank。当前不提供“获得 bank 后直接退出中继”的无损模式。

2026-10-18 这份日志中，同一条 TCP 65010 连接先后下发两个 bank：

| 时间（UTC+8） | 事件 | 解释 |
| --- | --- | --- |
| 14:09:56.249 | 旧 UDP 流最后一次解密数据 | 可能结束、离开或网络暂停；仅凭静默不能确认战局结束 |
| 14:11:36.907 | 同一 TCP 连接下发新 bank | 新密钥候选，尚未证明新战局 |
| 14:11:59.222 | 新 UDP 地址/端口出现握手 | 新的游戏传输会话信号 |
| 14:11:59.426 | 新 UDP 流成功绑定新 bank | 已有多个不同数据包的结构校验依据 |

0.3.2 已接入原版的展示状态规则：用解密后 UE 控制通道 Welcome `/Game/...` 确认战斗流，在已确认流中自动选择最近收到明文的一条；超过 15 秒没有上下行明文则进入空闲并清空当前人物显示。新流确认后自动切换，同地址新 epoch 也需要重新确认；仅获得 bank 不会直接激活战斗流。

IP/端口变化、bank 变化或静默单独都不等同战局结束：重连、迁移、丢包也可能产生这些现象；bank 未变化也不代表一定仍在同一局。精确确认需要进一步解析战局 ID、进入/退出/结算协议消息，当前尚未实现。

Welcome 与状态变化已分别写入 `battle_welcome`、`battle_state` 日志。本版不会据此自动断开 TCP、停止采集或宣称确认战局结束。同一条 TCP 可跨多次 UDP 会话下发 bank，保持它的中继才能获取后续 bank。

## 4. 复用解密和验证工具

采集生成相同 `udp_raw`、`udp_plaintext`、bank 观察和绑定事件，现有工具可直接消费：

```powershell
native/tools/run-native.ps1 decode-players --in captures/shared-dh-001.jsonl --output-directory captures/shared-players-001
native/tools/run-native.ps1 report --out captures/shared-report.json captures/shared-dh-001.jsonl captures/shared-dh-002.jsonl
```

人物解码与 `validate-players` 的完整参数见 README。坐标与身份已直接由 C++ 解析并保存；支持范围见 PLAYER_DECODING.md。
多次采集保持 Device 不变并使用新文件名；每次透明采集都要在启动后重建客户机 TCP 连接。
研究连续周期优先选择一次较长采集，再用不同会话复核；`collect.py` 也接受同一套共享 CLI 参数，
每次共享 TCP 会话由 C++ 管理临时防火墙规则；它不会开启共享或让游戏重连。

## 数据路径与可靠性

- UDP 时间来自 WinDivert 的 QPC 接收时间，并转换为本会话墙钟时间；主循环不重新给包打时间戳。
- 过滤客户机地址的双向转发流量，不同时抓主机上行出口，避免 NAT 前后重复采样。
- NETWORK_FORWARD 的 IfIdx 可能描述不同链路，过滤按客户机 IPv4；私有接口索引用于 TCP 入站注入。
- UDP 用户态队列最多 8192 包 / 32 MiB。IPv4 UDP 分片先重组，完整后才进入队列；正常分片不再产生 capture_gap。队列溢出、无效包、分片冲突或缺片 5 秒超时仍清理队列并中断连续性判断。统计和验证方法见 [UDP_REASSEMBLY.md](UDP_REASSEMBLY.md)。
- 驱动队列为 8192 包 / 16 MiB / 2000 ms；WinDivert 没有这里可读取的可靠丢包计数，日志 kernelDrops 为 null，不能宣称无丢包。
- TCP 新 SYN 建立独立端口映射；重传复用映射，相同四元组的新序列号使用新映射，旧映射返包被丢弃。
- 改写 IP/端口后调用 WinDivertHelperCalcChecksums 修复校验和。TCP 数据仍由现有中继按字节流处理。
- Windows 最大 256 条并发中继连接，映射上限 8192；闲置映射五分钟回收。UDP 分片重组只处理采集副本，TCP 仍由系统字节流与 TGCP 帧缓存处理；不含 IPv6 分片。
- 采集失败/关闭会影响已接管的 TCP 会话；UDP 被动句柄没有重发路径。

## 验证状态（2026-10-18）

Windows Release 构建和 10 个 CTest 测试通过；包解析/路由/扫描范围测试 8321 条断言。
实际随仓库 WinDivert DLL：4 种预检、9 种错误配置拒绝、43 个过滤匹配与 4 个校验和测试通过。
真实 DLL 测试只调用用户态 helper，没有开启 WinDivert 驱动句柄。
另用独立模拟 DLL 将 500 个加密合成 UDP 包送入实际采集线程、队列、日志和 bank broker，全部解密；正常退出及接收错误记录通过。
新增模拟 TCP 验证：动态监听端口、80/443/65535 三条目标映射、回程源地址恢复、原服务器回复抑制，以及同网段/已有连接绕过。真实 loopback 验证 32 条连接（24 条并发）、非 mode 3/TLS 字节透传、服务器先发、前缀超时、半关闭、分片 DH 与替换后失败关闭。
模拟 DLL 只在测试临时目录作为驱动替身，不捕获、发送或安装任何真实网络驱动。
PowerShell Info/Preflight 执行通过，EnableSharing 的 WhatIf 通过；未实际启用 ICS 或添加防火墙规则。
当前检查时没有可用 Wi-Fi 网卡、ICS COM 显示未开启共享。尚未进行真实客户机热点/ICS 端到端测试。
没有真实游戏采集，不给出实际 bank 周期或人物准确率结论。

实现依据：原项目 `GatewayCapture.Realtime.HotspotTcpRouter`、`RealtimeSession.StartAsync`（监听端口 0）、`TcpRelayServer.RunAsync`、`TgcpProtocol.Sniff` 的 IL（`reverse/host_crypto/il/`）；WinDivert v2.2.2 官方头文件随许可证保存在 `third_party/windivert`。
官方文档参考：https://reqrypt.org/windivert-doc.html
ICS 参考：https://learn.microsoft.com/en-us/windows/win32/nativewifi/using-hosted-network-and-internet-connection-sharing
