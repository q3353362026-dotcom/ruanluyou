# 协议解析静态追踪

> 后续补充：宿主密钥获取与解密现已追通，见 [KEY_DECRYPTION_TRACE.md](KEY_DECRYPTION_TRACE.md)。本文原先的未分析范围保留为当时记录。

分析日期：2026-10-18。

范围：针对当前工作区保留的 Python 3.14 字节码，使用匹配 magic 的 Python 3.14.5，通过 marshal 和 dis 静态读取。未导入或执行目标模块，未启动采集宿主、驱动或网络抓包，未用真实对局验证。模块注释里的“verified”属于原作者记录，本报告不把它当作本次实测结论。

共生成 31 个模块、499 个代码对象的索引与反汇编。代码对象包含模块体、类体和嵌套函数，不代表 499 个独立业务函数。索引保留各输入 .pyc 的 SHA256；过长指令操作数有截断标记，原始字节码仍是完整依据。

## 1. 两条运行路径的边界

默认完整 EXE 的采集与解密仍来自 original_nva.exe。src/gateway_parser.py 是可单独重建的明文事件消费者。默认 build/build.ps1 没有把新网关重新嵌入宿主。本次查明的是工作区网关源码与 vendor_pyc 依赖的静态链路，不能据此断言内置宿主中的每个模块与这些副本逐字一致。

## 2. 已追通的数据链路

~~~text
宿主输出 udp_plaintext 事件
  -> gateway_parser.Engine.accept
  -> PlaintextLoader.pending_packets
  -> flowlive.IncrementalFlowLoader.consume_packet
  -> codec.engine_packet.decode_engine_packet
       -> codec.packet.decode_packet_parts（外层分帧）
       -> termination / PacketHeader（内层分帧）
       -> codec.engine_bunch.decode_engine_bunches
  -> ReliableSequenceUnwrapper + PartialBunchReassembler
  -> 按 channel 存储 Logical Bunch（LB）
  -> liveview.LiveView.poll
       -> NetGUID 导出、NewActor 和 content block 分帧
       -> PlayerState 身份属性
       -> Character 身份关联和 movement handle
       -> MovementReplicationActor 的 h21 槽位坐标
       -> 血量、武器、物资、盒子等状态跟踪器
  -> LiveView._derive 关联身份与槽位
  -> LiveView.snapshot
  -> Engine.flush -> /api/state
~~~

入口证据：[flowlive.IncrementalFlowLoader.consume_packet](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/flowlive.dis.txt:1177)、[codec.engine_packet.decode_engine_packet](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/codec.engine_packet.dis.txt:679)、[liveview.LiveView.poll](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/liveview.dis.txt:6874)。

## 3. 包与通道层

codec.packet 将方向相关的外层封装与 UE 风格位流分开；通用 request 模式可以移除 8 字节前缀和尾部填充。但 gateway_parser.PlaintextLoader 明确传 request_prefix=False，因此不能给当前明文输入再次强行剥掉 8 字节。

decode_engine_packet 在正常数据包上定位内外 termination bit，处理 PacketHeader，然后仅把纯 bunch 区域交给 bunch 解码器。错误会记录到 trace.error / bunch_parse.error。

bunch 头包含通道号、open/close、可靠性、NetGUID 导出标志、partial 分片标志、可靠序号和 payload 位数。payload 按位处理，不能默认字节对齐。

ReliableSequenceUnwrapper 按 stream/channel 展开循环可靠序号。PartialBunchReassembler 保存每个通道尚未完成的分片链，在 final 到来后输出逻辑 bunch；孤立分片、可靠性变化、可靠序号缺口和新首片替换旧链都会被诊断或丢弃。

证据：[codec.engine_bunch._read_one_bunch](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/codec.engine_bunch.dis.txt:1078)、[codec.engine_stream.ReliableSequenceUnwrapper.unwrap](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/codec.engine_stream.dis.txt:152)、[codec.engine_stream.PartialBunchReassembler.feed](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/codec.engine_stream.dis.txt:900)。

Handshake.ok 仅判断 welcome 是否存在；full 才进一步要求 hello、login、join、challenge。因而“握手可用”不等于完整捕获所有握手阶段。

## 4. NetGUID、对象生命周期与属性分帧

sna.parse_prefix 处理 PackageMap 导出、MustBeMappedGUIDs 和 NewActor 前缀。导出内容将 NetGUID 关联到对象/类路径；NewActor 则给出 actor、archetype、出生位置等。随后 blocks.tile 定位 Actor 或 SubObject 的内容块，保留 has_rep、deleted、payload_start 和 payload_end 等边界信息。

“通道号”“actor NetGUID”“PlayerState NetGUID”“movement slot”是不同身份，不能互相直接替代。LiveView 分别保留 players、links、ch_actor、ps_ch_actor、slots 等集合，再做关联。

证据：[sna.parse_prefix](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/sna.dis.txt:841)、[blocks.tile](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/blocks.dis.txt:97)、[liveview.LiveView._handle_char_lb](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/liveview.dis.txt:5607)。

## 5. PlayerState 按类路径选择协议配置

playerstate_protocol.select_profile 依据导出的类/包路径匹配 profile，decode_playerstate 再把属性位区间交给 repdec6.decode_block。它通过 schema 的 parent/cmd 信息把解出的值映射成具名字段。

repdec6 的帧结构包括 checksum 标记位、packed handle、可选 checksum、动态数组元素及结束 handle 0。cmd_index、parent_index、网络 handle 和对象内存 offset 是不同坐标系。schema 中的 object_offset 不能直接当作 UDP payload 偏移。

当前 sol_0904 schema 中的已核对字段：

| 网络 handle | 字段 | 备注 |
|---:|---|---|
| 27 | PlayerNamePrivate | 玩家名 |
| 28 | Uin | 身份字段 |
| 62 | TeamID | 队伍字段 |
| 129 | PlayerGamePlayState | 不是 bIsDeadBox |
| 130 | CurrentCharacterLiveStatus | 当前生命状态字段 |
| 132 | bIsDeadBox | 死亡盒标记 |

这些值仅对应该 profile，不是跨版本常量。decode_playerstate 即使未达到 exact 状态，也可能返回已读出的 identity；完整 fields 只有 status == exact 才填充。调用方区分完整结果与未完全验证的身份结果。

证据：[playerstate_protocol.select_profile](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/playerstate_protocol.dis.txt:115)、[playerstate_protocol.decode_playerstate](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/playerstate_protocol.dis.txt:534)、[liveview.LiveView._handle_player_lb](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/liveview.dis.txt:5165)；配置文件 runtime/ErCore/src/core/playerstate_schema_0904.json。

## 6. 人物坐标来自移动槽位

LiveView._handle_mra_lb 从内容块读取属性 handle，只有 h == 21 才调用 repmove4.read_frame4。

read_frame4 读取数量和存在位图，得到本帧出现的 slot；每当 slot >> 4 变化时，重新读取共享上下文。每个 item 再根据 mask 解码具体字段，而不是固定结构的 x/y/z 三个浮点数。

位置的核心还原形式为：

~~~text
width = 32 - ctx.bits[k]
component = signed_zigzag32(read_bits(width) | ctx.val[k]) / 10.0
~~~

这里的除以 10 是协议量化的还原步骤，不能与渲染层厘米转米的除以 100 混淆。解出坐标后还会经过 WORLD 检查，随后写入：

~~~text
slots[slot_id].append((sequence, timestamp, x, y, z, yaw))
~~~

证据：[liveview.LiveView._handle_mra_lb](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/liveview.dis.txt:6673)、[repmove4.read_item](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/repmove4.dis.txt:124)、[repmove4.read_frame4](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/repmove4.dis.txt:562)。

## 7. 玩家名字如何对应一个移动点

最直接的路径是 Character 属性明确给出的 movement handle。当前 er_live.merge_character_identity 将 h14/h18/h60 分别写到 owner、ps_guid、move_handle，并为 movement handle 保留赋值序号历史。

explicit_slot_claims 按当前或指定时间点的赋值历史关联 slot 与 actor，处理槽位复用和冲突。名称等信息则通过 Character.ps_guid 或 owner 关系连接到 PlayerState。

当没有显式 movement handle 时，LiveView._derive 仍存在回退逻辑，包括：

- 已知身份角色的出生位置与邻近序号的槽位位置匹配，条件使用三维距离平方小于 250000；按项目厘米单位相当于 5 米范围。
- 小队位置广播与槽位的水平距离匹配，使用距离平方小于 90000，即 3 米范围。
- 匿名槽位与未关联 PlayerState 的时序候选分配。
- 从已关联数据统计 PlayerId-slot 偏移，再把优势偏移用于补全；判断包括至少 4 个支持样本、至少 80% 支持且超过第二名 3 倍。
- 最后一个空闲身份与最后一个空闲槽位的一对一回退。

因此，“坐标已解码”“身份已具名”“身份与位置有明确协议绑定”是三种不同证据强度。错误名字、队伍误判或重生后串号，应优先检查关联过程，而非仅检查地图投影。

证据：[er_live.merge_character_identity](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/er_live.dis.txt:2044)、[er_live.explicit_slot_claims](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/er_live.dis.txt:2120)、[liveview.LiveView._derive](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/liveview.dis.txt:10242)。

## 8. 自身姿态另有上行来源

consume_packet 对 request 有独立分支。_c2s_self_from_bunches 只处理 channel 3，重组后交给 c2s_self.parse_fgp；成功时更新 self_pose、pose_samples 和接收时刻。

网关会在姿态足够新鲜时补充 self_aim_yaw / self_pitch 等字段。因此镜头朝向与远端玩家坐标不必来自同一个下行数据源。parse_fgp 的完整/精简尾部路径需要后续用输入样本核对，本次未把其注释中的历史采样率当成实测数据。

证据：[flowlive.IncrementalFlowLoader._c2s_self_from_bunches](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/flowlive.dis.txt:1675)、[c2s_self.parse_fgp](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/c2s_self.dis.txt:386)；src/gateway_parser.py 的 flush()。

## 9. 血量、物资与盒子

LiveView.poll 调用各自的增量状态跟踪器。PlayerState/Character 的生命状态、HealthBatchTracker 的血量、WeaponTracker 的武器、PickupTracker 的地面物品、DeathBoxTracker 的盒子、ContainerTracker 的容器和 box_contents 的内容，最后参与快照组合。

这里不是一个统一的“扫内存字段”过程：每类对象有自身的导出类、属性布局、通道生命周期及状态合并规则。物品关闭通道也不天然等于被玩家捡走，不能把两个事件直接等同。

证据：[liveview.LiveView.poll](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/liveview.dis.txt:6874)、[property_health.HealthBatchTracker.consume](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/property_health.dis.txt:842)、[pickup_state.PickupTracker.consume](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/pickup_state.dis.txt:693)、[death_box_state.DeathBoxTracker.consume](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/death_box_state.dis.txt:1856)、[liveview.LiveView.snapshot](C:/Users/Administrator/Documents/Radaer/reverse/protocol_trace/liveview.dis.txt:15208)。本次深入验证重点是包链路、身份关联与移动解码，未逐条还原所有物品/武器字段。

## 10. 已确认的静态缺口与注意事项

1. repmove4.read_item 遇到 mask & 0x10 会返回 stop 标记。read_frame4 随即提前返回，把 stopped/left/pos 写入结果。LiveView._handle_mra_lb 只消费 fr['items']，没有检查 stopped。这意味着它可能接受一个部分解出的帧，遗漏后续 slot 更新。是否在当前游戏数据中触发，需要样本验证。
2. _handle_mra_lb 对分帧及 read_frame4 的异常直接跳过，没有在此函数中向上增加明确的移动解析错误计数。因此总错误指标为零不能单独证明移动链路完整。
3. 一些原模块注释已经与实现不一致。例如 repmove4 对 mask 0x02 的说明写 3 个 u32，而实际代码读取 range(6)。维护时应以指令和样本为准。
4. 人物名字/槽位存在启发式回退，应该在诊断中保留绑定来源，不能把所有具名实体当作显式协议绑定。
5. 本报告没有恢复 original_nva.exe 的密钥与解密实现，也没有证明当前游戏版本仍与这些 schema 匹配。

## 11. 后续验证建议

以已有、授权的明文 JSONL 或缓存会话作为固定输入，优先增加三类可观测信息：移动帧 stopped/剩余位数、每个槽位的身份绑定来源、当前类路径/profile 与属性解码 status。再比较样本中 actor 出生/销毁、槽位复用及物品转移时序。先确定哪个环节出现缺失，再修改解码器。

本次仅新增静态检查脚本、反汇编资料和此报告，没有修改运行代码或原始字节码。
