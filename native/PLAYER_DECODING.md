# C++ 人物解析

从 0.2.0 起，`radaer-native.exe` 直接完成已解密 UDP → UE 位流 → 人物样本 → 合理性检查。运行时不加载 Python、`.pyc` 或外部协议目录；PlayerState/Character 目录已编译进程序。

## 使用与保存

WebUI 或命令行 `capture`、`replay` 自动启用。原始包、UDP 明文和人物事件保存在同一个 JSONL 文件；默认 WebUI 路径为 `captures/webui/web-时间戳-随机值.jsonl`，每条事件写入后 flush。

已有包含 `udp_plaintext` 的记录可独立解析，无需 bank：

```powershell
native/tools/run-native.ps1 decode-players `
  --in captures/webui/web-xxxx.jsonl --output-directory captures/decoded-001
```

输出目录必须不存在，生成：

| 文件 | 内容 |
|---|---|
| `players.jsonl` | `player_sample`：每次有效位置更新 |
| `identities.jsonl` | `player_identity`：PlayerState 属性更新及解析状态 |
| `player-quality.jsonl` | `player_validation`：每个坐标样本的质量检查 |
| `self.jsonl` | `self_sample`：上行 ch3 本人坐标、速度、朝向、俯仰及关联队伍 |
| `self-quality.jsonl` | `self_validation`：本人样本的独立连续性验证 |
| `decoder-errors.jsonl` | 未支持字段、截断、重组缺口及输入错误 |
| `decoder-warnings.jsonl` | h21 后续属性等部分解析提示；0.3.6 起不再记录预期的 0x10 停止 |
| `battle-events.jsonl` | Welcome 确认、当前流切换、静默空闲及恢复的状态事件 |
| `summary.json` | 输入包数、样本数、身份更新数、重复和错误统计 |

只有 `udp_raw` 密文的文件需先用匹配 bank 执行 `replay`。无法解析出人物时返回 `no_player_samples`；命令成功不保证每个包都受支持，需检查统计中的 `errors`。非法 JSON/事件异常记入 `exceptions` 并返回非零退出码。

## 已实现

- LSB 位读取、DFM packed integer、SerializeInt、UTF-8/UTF-16 FString、FName、压缩向量/旋转。
- UE PacketNotify、两层终止位、bunch 边界、可靠序号回绕、重复抑制、乱序缓存和 partial 合并；包方向独立跟踪。
- NetGUID/PackageMap 导出、NewActor 初始位置/旋转/缩放/速度、content block。
- MovementReplicationActor 的 h21/repmove4：位图、共享压缩上下文、变化分量、坐标与朝向。
- `BP_SOLPlayerState_C` / `BP_DFMPlayerState_C` 两版属性目录；支持基本标量、字符串、枚举、引用，以及已知数组的边界遍历。数组不会被扁平化为人物顶层字段。
- Character 前段 Owner、PlayerState 和 MoveHandle。通过明确 GUID/槽位关系关联名字、TeamID 等；存在冲突或缺少证据时保留未关联槽位。
- 会话、UDP flow、bank epoch 隔离；采集断档/重新绑定清除旧状态；通道关闭、复用、身份变化会分开位置轨迹。

可靠 bunch 按通道排序；关闭后保留连接内可靠序号进度，避免旧重传重新污染新通道。未知协议不通过任意扫描浮点数猜测位置。

## 样本字段与可信度

`data` 包括 `flow/epoch/channel/slot/track`、源包序号与时间、`x/y/z/yaw`、`name/teamId`、Actor/PlayerState GUID、关联来源、bank 指纹和已缓存属性。

- `channelSource=exported_archetype`：类导出指向已知 MovementReplicationActor。
- `channelSource=structural_frame_h21`：中途采集、未见类导出，靠完整 h21 结构识别。仍保留为未分类槽位，直到取得明确人物关联。
- `identitySource=explicit_character_move_handle`：存在 Character 槽位引用；名字还需要对应 PlayerState。并非密码学身份认证。
- `identityFieldsExact`：最近 PlayerState 属性块是否完整解码；`properties` 为累计已知值，不代表一次完整服务端快照。
- `handshakeOk`：观察到控制通道 Welcome `/Game/...`，不等于 TLS/签名认证。
- `decodeCompleteness=known_prefix`：只解析到已知前缀；`movementFrameComplete` / `movementBlockComplete` 分别表示移动帧和属性块是否读完，`decodeStopReason` 记录停止原因。`complete` 也只描述当前 h21 块，不表示完整游戏协议。
- `semanticErrorsSincePrevious`：同一方向、同一通道的错误增量；`packetErrorsSincePrevious`：下行包整体结构错误增量。`decoderSemanticErrors` 仍保留全流错误总量，用于统计而非直接判定人物失败。

坐标须有限、非全零且绝对值不超过 2097152。质量检查默认要求连续 3 个合理样本；未见握手、相关解析错误、重复、时间倒退和陈旧数据会被标记。0.3.4 起，已由类导出或先前完整 h21 块确认的移动通道遇到 0x10 native object 时，保留已读取的槽位，包括当前槽位在未知对象之前的坐标，停止后续槽位读取。完整移动帧后出现未知属性时，同样保留坐标并停止该属性块；下一 content block 由独立长度边界定位。截断、上下文缺失、终止 handle 缺失及尾部异常仍拒绝该帧；未确认通道不能凭部分帧建立识别。

部分解析样本带 `partial_movement_block` 提示，并继续经过同样的握手、连续样本、坐标和时间检查。实际错误只影响对应通道；包整体结构错误保守影响整条下行流。非可靠 bunch 出错后仍能处理同包后续 bunch，不清除无关通道的分片与可靠队列。`plausible` 仅代表合理性通过，`cryptographicAuthenticated` 始终为 false。

0.3.6 起，已确认移动通道的 `0x10` 停止作为预期行为静默处理，不再生成 `player_decode_warning`，也不计入警告数量。坐标样本仍保留 `decodeCompleteness` 等覆盖信息；未确认通道、截断和其他异常仍按原有规则处理。

WebUI 只展示当前已确认战斗流最近更新的 128 个槽位，超过 2 秒未更新标为历史；日志保存全部实际输出样本。Welcome 确认后按明文包到达顺序自动切流，上下行均刷新活跃时间；超过 15 秒无明文时清空展示，收到明文后恢复。绑定重置、新 epoch 和采集断档会撤销对应握手确认，旧 epoch 的迟到数据不会覆盖新状态。不会把采集统计当作真实在线人数，也不把静默标为明确结算。

## 范围与限制

此移植覆盖原 Python 桥接输出的远端移动槽位，以及明确的人物身份/队伍关联；不是整个旧雷达的全部协议移植。

0.4.0 已支持上行自身移动 full/lite 消息和二维雷达，详见 [RADAR.md](RADAR.md)。尚未支持位置/速度/视角以外的完整上行自身状态、血量专用批量协议、完整武器/背包/物资、未知 native struct、NetField compatibility exports。Character 仅解析身份关联所需前段；PlayerState 遇未知字段报告 `unsupported`，不推测字段长度。

限制每流 2048 个通道、8192 个 Actor/槽位、32768 个 NetGUID、每通道 64 条乱序 bunch、单个重组负载 1 MiB、每流重组缓存 8 MiB、64 个活动 flow、每帧 512 个移动槽位。超过边界会报告诊断或输入异常。丢失初始握手/类导出或协议版本变化可能使身份缺失或无法解析。

## 验证范围

Windows Release 构建；13 项 CTest 包括 358 组原 Python 底层差分向量、36 条非空坐标完整链路对照、加密重放到人物、乱序/分片/回绕、通道复用、断档与 epoch 重置；模拟 WinDivert 500 包产生 992 条坐标。战斗流状态测试覆盖多流选择、无 Welcome 时不显示、15 秒阈值、超时恢复、重绑和迟到旧 epoch。0.3.4 增加部分帧停止位置、未知通道不得部分识别、真实截断仍拒绝、跨通道错误与分片隔离、部分坐标仍校验速度/握手，以及 HTTP 返回部分解析字段与提示的检查。0.3.5 增加 381,995 项位流边界差分检查，验证读取、切片和拼接优化后的输出与逐位参考实现一致。

0.3.2 还回放了已有的 2026-10-18 14:00–14:19 采集日志：两个 UDP 流均识别到 Welcome，依次进入活跃、15 秒静默空闲，再自动切至第二条流；50,385 个明文包仍输出 55,206 条人物样本，既有 1,096 条解析诊断未在本次改动中修复。这是历史日志验证，未重新进行实机采集，也不能据此确认游戏结算时刻、真实人物准确率或 bank 变化周期。浏览器视觉验收仍受本机页面访问权限阻挡。

开发对照使用 `native/tests/player_reference.py`，需 Python 3.14、NumPy 和完整工程中的原协议模块；不影响原生运行包。

0.3.4 对用户 15:07–15:15 日志固定快照回放：21,422 个明文包、样本数 19,833 → 20,491、未通过 1,319 → 291。原有坐标逐条不变，新增 658 条坐标/朝向均与原版匹配；83 条错误仍保留，另有 276 条部分解析提示。完整差分仍存在 1 条原已存在的坐标不一致，不能把与原版一致当作独立准确率证明，详见 [SDK_ANALYSIS.md](SDK_ANALYSIS.md)。

0.3.5 优化位流操作和事件 JSON 构造，相同快照下全部七个回放输出文件与 0.3.4 的 SHA-256 一致。解析耗时与整体回放耗时见 [PERFORMANCE.md](PERFORMANCE.md)；没有更改未知字段处理或可靠性门槛。
