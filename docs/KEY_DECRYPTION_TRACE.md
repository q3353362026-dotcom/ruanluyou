# 宿主密钥获取与解密实现追踪

分析日期：2026-10-18。对象：当前工作区的 `runtime/brand_host_login/original_nva.exe`。

## 1. 结论与修正

这份宿主的实时链路是 **TGCP TCP 主动中继 + UDP 被动抓包**。

- TCP：识别 TGCP mode 3，替换双方 DH 公钥，分别计算客户端侧和服务器侧 AES 密钥；读取明文后，使用另一侧密钥重加密并转发。
- UDP：WinDivert 使用 `SNIFF | RECV_ONLY` 捕获副本。下行副本使用 TCP 明文中提取的 128 字节 bank 解密；上行去掉 8 字节业务前缀。
- 因而，把所有流量改成纯被动抓包，不能直接替代现有密钥获取流程。主动 DH 协商本身也是当前密钥来源的一部分。
- 先前把 EXE 称为 NativeAOT 不准确。文件具有 .NET single-file bundle 6.0 清单，包含可读取 IL 的应用程序集 `Nva本地离线解析.dll`，本次已抽取。

结论来自宿主方法体与合成输入验证，不代表当前线上游戏版本已验证可用。

## 2. 分析对象与复现材料

- 原宿主 SHA256：`7be61a99f35edda730bb43ca664d2cbdca9d7954702c358c75b3865b417d7c0a`。
- 应用程序集 SHA256：`a6e91040d94c79e18e3b0307d47164f12480dcce83a343b38342bdca9bb2d540`。
- Bundle 清单 173 项，抽取 7 项应用/非框架程序集与配置。
- 导出 204 个类型、1381 个方法定义，方法体读取错误为 0。数量包含自动生成状态机与记录类方法。
- 原宿主和驱动未修改；源文件仅更正宿主格式的文档字符串。
- [Bundle 清单](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/bundle_manifest.json)、[IL 索引](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/index.json)。
- [提取脚本](C:/Users/Administrator/Documents/Radaer/tools/extract_host_bundle.py) 使用 Python 标准库。
- [离线 IL 读取器](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/inspector/Program.cs) 使用 PEReader/MetadataReader，不执行目标程序集入口。
- [合成验证器](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/validation/Program.cs) 仅调用已检查的密码学、分帧与内存会话方法。

读取器与验证器使用已有 .NET SDK，无第三方 NuGet 依赖。ILSpy 安装因网络失败、随后提权自动审批服务配置错误而未完成；本报告不依赖 ILSpy。

## 3. 实时入口与实际抓包模式

`RealtimeSession.StartAsync` 装配 `UdpKeyBroker`、`TcpRelayServer` 和 `HotspotTcpRouter`：TCP 中继的 observation 回调连接到 `broker.Add`；UDP 接收回调连接到 `broker.Observe`；输出统一交给 `RealtimeOutput.Publish`。

会话元数据明确写入 `tgcpProfile=mode3-md5-cbc`、`dhPrivateSource=fresh-PC-generated`、`phoneMemoryInput=false`。这条链路在 PC 生成 DH 私有值，不需要输入手机内存中的私钥。

证据：[实时会话元数据](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.RealtimeSession+_StartAsync_d__21.il.txt:267)、[TCP observation 回调](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.RealtimeSession+_StartAsync_d__21.il.txt:206)。

`OpenChecked(filter, layer, flags)` 原样传递 flags，priority 为 0：

| 通道 | layer | flags | 用途 |
|---|---:|---:|---|
| TCP forward | 1 / NETWORK_FORWARD | 0 | 捕获并重定向设备 TCP |
| TCP return | 0 / NETWORK | 0 | 处理本地中继返回设备的 TCP |
| UDP | 1 / NETWORK_FORWARD | 5 | SNIFF(1) + RECV_ONLY(4)，读取副本 |

证据：[句柄初始化](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.HotspotTcpRouter+_StartAsync_d__44.il.txt:224)、[OpenChecked](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.WinDivertNative.il.txt:65)。

`ReceiveLoop(kind=2)` 提取 UDP 载荷并 `ToArray`，构造 `UdpPacket` 调用分析回调后直接继续接收，未回注 UDP。此前针对可选 Python `WinDivertUDPSource` 的 recv→send 分析，不能套用到宿主 UDP 通道。证据：[ReceiveLoop](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.HotspotTcpRouter.il.txt:648)。

## 4. TGCP 帧布局

| 字段 | 字节偏移 | 编码 |
|---|---:|---|
| magic | 0..5 | `33 66 00 0B 00 0C` |
| command | 6..7 | UInt16 大端 |
| direction | 8 | 上行 0，下行 1 |
| sequence | 9..12 | UInt32 大端 |
| header length | 13..16 | UInt32 大端 |
| body length | 17..20 | UInt32 大端 |
| hello 参数 | 21..23 | `03 00 40` |
| hello DH 公钥 | 24..87 | 64 字节无符号大端 |

`Lengths` 限制 header 在 21..65536、body 不超过 16777216。`Sniff` 识别 client hello 0x1001、至少 88 字节头与 mode 3 参数。`TgcpSession` 还要求 client hello body 为空。`FrameBuffer` 处理 TCP 分片和多帧，不能假设一个 TCP 包就是一个 TGCP 帧。

证据：[Lengths](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.TgcpProtocol.il.txt:9)、[Sniff](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.TgcpProtocol.il.txt:67)、[Public](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.TgcpFrame.il.txt:102)、[ReplacePublic](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.TgcpFrame.il.txt:140)。

## 5. DH 获取：双方公钥替换

`TgcpSession` 构造时调用两次 `KeyPair`，生成 (x,X)、(y,Y)。私有值各取 64 随机字节，首字节调整为 `(b[0] & 0x7f) | 0x40`。公钥为 `X=2^x mod p`、`Y=2^y mod p`；p 是二进制内固定 512 位整数常量。公钥范围要求 `1 < public < p-1`。

设客户端持有私钥 a、公钥 A，服务器持有私钥 b、公钥 B：

~~~text
客户端发送 A → 宿主保存 A，向服务器发送 Y
服务器发送 B → 宿主保存 B，向客户端发送 X

K_client = MD5(unsigned_big_endian_minimal(A^x mod p))
K_server = MD5(unsigned_big_endian_minimal(B^y mod p))
~~~

客户端算出的 X^a 与宿主的 A^x 相同；服务器算出的 Y^b 与宿主的 B^y 相同。因此宿主分别掌握两侧会话密钥。

**共享值取最短无符号大端编码再 MD5，不补到 64 字节；公钥在线传输时才左补零到 64 字节。** MD5 结果为 16 字节，对应 AES-128。

状态机：0 等 client hello → 1 等 server hello → 2 正常中继。状态 1 暂存新增上行帧，收到 server hello 后继续双向处理。建立后出现重复 hello 会抛错。

证据：[会话构造](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.TgcpSession.il.txt:38)、[握手状态机](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.TgcpSession.il.txt:227)、[KeyPair](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.TgcpProtocol.il.txt:176)、[Derive](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.TgcpProtocol.il.txt:260)、[PublicBytes](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.TgcpProtocol.il.txt:230)。

## 6. TCP body：AES-CBC 与自定义填充

固定 IV：`00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f`。底层 `PaddingMode.None`，协议自行填充，并非 PKCS#7：

~~~text
r = plaintext_length % 16
padding_length = (16 if r < 11 else 32) - r
padded = plaintext + padding_length 字节的尾部空间
padded[-6:-1] = ASCII "tsf4g"
padded[-1] = padding_length
~~~

`Encrypt` 创建的其余尾部空间为零。`Unpad` 验证长度 6..21、标记和余数对应关系，但未逐字检查其余填充字节是否为零。

~~~text
上行：DecryptPadded(K_client) → 明文观察 → EncryptPadded(K_server)
下行：DecryptPadded(K_server) → 明文观察 → EncryptPadded(K_client)
~~~

转发时重加密完整 padded buffer；去填充副本用于观察与提取 bank。正常情况下 body 长度保持不变。server hello 如带 body，也进入这一处理分支。

证据：[DecryptPadded](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.TgcpProtocol.il.txt:368)、[Unpad](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.TgcpProtocol.il.txt:290)、[EncryptPadded](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.TgcpProtocol.il.txt:405)、[Encrypt](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.TgcpProtocol.il.txt:430)、[解密与重加密分支](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.TgcpSession.il.txt:379)。

## 7. 从 TCP 明文提取 UDP bank

`Banks(plain)` 的步骤：

1. 按 Latin-1 读取明文字节；
2. 搜索独立 Base64 token：`(?<![A-Za-z0-9+/])([A-Za-z0-9+/]{171}=)(?![A-Za-z0-9+/=])`；
3. 解码后恰好 128 字节，且重新 Base64 编码必须与原 token 一致；
4. `TgcpSession` 按 SHA256 指纹去重，放进 `TgcpObservation.Banks`；
5. `UdpKeyBroker.Add` 记录连接 ID、TGCP sequence、创建时间，保留至多 64 个候选。

这是内容扫描加后续验证，不是精确解析一个固定的业务字段；符合格式的内容先作为候选。

实时入口传入 `persistKeys=true`。按此实现，候选写为会话目录下的 `keys/<sha256>.bin`，并发布 `bank_candidate` 事件。本次没有启动捕获，也没有读取或生成真实会话密钥。

证据：[bank 提取](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.TgcpProtocol+_Banks_d__16.il.txt:46)、[Add](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.UdpKeyBroker.il.txt:118)、[实时入口启用候选落盘](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.RealtimeSession+_StartAsync_d__21.il.txt:188)。

## 8. UDP：上行去前缀，下行解密

`EmitPlain` 的方向处理必须区分：

| 方向 | 处理 |
|---|---|
| 上行 | `packet.Payload[8:]`，不调用 XTEA |
| 下行 | `UdpDecoder.Decrypt(packet.Payload, bank)` |

上行的 8 字节是 **UDP payload 内的业务前缀**，不是 UDP 协议头。抓包层已经剥离 UDP 协议头。Python 收到 `udp_plaintext` 后不能再次去前缀，和此前 `request_prefix=False` 的结果一致。

下行算法：

- bank 固定 128 字节 = 8 组 × 16 字节子密钥；
- payload 每 8 字节为一个分组，读成两个 UInt32 小端字；
- 第 i 个分组使用子密钥 `i % 8`，每个数据报从第 0 组开始；
- 子密钥读成 4 个 UInt32 小端字；
- 每组 32 轮 XTEA，delta=`0x9e3779b9`，起始 sum=`0xc6ef3720`；
- 运算按 uint32 回绕，分组之间没有 CBC 链接；
- 不满 8 字节的尾部原样保留；输入数组先复制，再对副本解密。

~~~text
for each complete 8-byte block at offset:
    k = four little-endian uint32 words from bank[((offset / 8) % 8) * 16:]
    v0, v1 = little-endian block words
    sum = 0xc6ef3720
    repeat 32:
        v1 -= (((v0 << 4) XOR (v0 >> 5)) + v0) XOR (sum + k[(sum >> 11) & 3])
        sum -= 0x9e3779b9
        v0 -= (((v1 << 4) XOR (v1 >> 5)) + v1) XOR (sum + k[sum & 3])
    write v0, v1 back to output, wrapping arithmetic to uint32
~~~

证据：[Decrypt](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.UdpDecoder.il.txt:3)、[EmitPlain](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.UdpKeyBroker.il.txt:990)。

## 9. 候选 bank 如何绑定 UDP 流

`TryBind` 检查两种条件：

- `HandshakeMatches` 要求的握手序列及两条下行应答，理由记录为 `two_handshake_responses`；
- 或最近最多 4 个、载荷长度至少 10 的下行包中，至少存在 3 个不同载荷，且所有样本解密后都通过 `ValidEnvelope`。

只有 **恰好一个** 候选通过，才绑定 bank 并分配新的 epoch；0 个或多个通过都等待。绑定后发布 `udp_bank_bound`，回放暂存包。

新的上行初始握手会清除旧绑定。下行失败计数达到 3 时也重置；成功输出会清零计数，上行失败本身不增加计数。

`EmitPlain` 接受通过 `ValidEnvelope` 的数据，或长度 25 且首字节最低位为 1 的握手形式，之后发布 `udp_plaintext`。`ValidEnvelope` 检查长度 10..2048、头尾位、边界和 bunch 结构；它是结构检查，不是密码学 MAC。

证据：[TryBind](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.UdpKeyBroker.il.txt:739)、[HandshakeMatches](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.UdpKeyBroker.il.txt:580)、[Observe](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.UdpKeyBroker.il.txt:276)、[ValidEnvelope](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/il/GatewayCapture.Realtime.UdpDecoder.il.txt:139)。

## 10. OpenWrt 迁移的实际边界

可以迁移：UDP 被动抓包、设备方向与端点归一化、下行 XTEA、上行前缀处理、bank 绑定、明文事件输出。

不能直接以镜像替代：现有 TGCP 密钥获取依靠主动 DH 替换。仅有原始 PCAP、没有对应会话 bank 或可用私有材料时，无法按当前 `Derive` 路径获得所需密钥。

可以评估两种架构：

1. 保留主动密钥获取路径，向 OpenWrt/分析机提供对应会话 bank，再处理 UDP 副本；现有宿主仍须参与该 TCP 协商。
2. 在 Linux/OpenWrt 重新实现主动 TCP 中继与 TGCP 协商，同时保留 UDP 被动捕获。

第二种需要替换 Windows 热点发现、WinDivert 重定向和连接回程映射等平台逻辑。不是把 WinDivert 改成 tcpdump 就结束。以上为基于实现的架构判断，没有实际部署。

## 11. 验证与边界

[验证结果](C:/Users/Administrator/Documents/Radaer/reverse/host_crypto/validation_results.json)：781 个断言全部通过。

- 96 种明文长度验证自定义填充、AES-CBC 加密、解密与去填充；
- 480 个 UDP 合成案例覆盖 0..159 字节、3 组 bank，包含跨 64 字节轮换与所有尾部长度；
- 验证最短大端共享值→MD5、合法 Base64 bank 与错误长度拒绝；
- 内存中模拟两端，验证分片 client hello、双方公钥替换、server hello 内 bank 提取与双向重加密；
- 未启动宿主入口、网络套接字或抓包驱动，未使用真实对局或真实密钥。

未覆盖：线上握手兼容性、真实网卡重定向、丢包延迟、OpenWrt 端到端运行。合成测试证明方法级理解与二进制一致，不证明这些外部条件。
