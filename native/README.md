# Radaer C++ 0.5.12

0.5.12 新增采集配置“保存日志”开关，默认开启、当前浏览器记住选择，下次开始采集生效。关闭后不写采集/运行事件 JSONL、不保存新 bank，不生成本次回放与 bank 报告；实时解析、雷达、内存事件显示继续工作，已有文件保留。启动终端的标准输出仍显示，不额外保存为文件。

0.5.11 将雷达头像上方身份信息合并为“人物名称 · 队伍编号 · 干员名称”一行，三个开关仍独立生效；人物列表同步同行显示。手机/平板使用单行统计条、更小的顶栏、底部工具和状态条；设置及人物列表默认收起，打开后最多占 44% 视口高度，内容内部滚动。显示开关改成双列，回放选择默认折叠，点击顶部“回放”自动展开。手机长人物名截短显示，人物列表/详情保留名称。

0.5.10 修正历史解析错误连带拒绝后续人物坐标：当前移动块完整解码时，以 `decoder_continuity_reset` 标记重建连续性，第一条有效坐标即可供雷达使用，重新累计可信样本。仅有已知前缀的样本不能单独完成错误恢复；握手、时间顺序、时效及配置的坐标/速度限制仍执行。未补齐的协议分支和未下发/未识别的人物坐标仍可能造成停更。

同时修正不同数据包在同一毫秒到达时被误判乱序：采集包序号与人物序号均递增才接受；原时间戳保持不变，速度校验使用 1 ms 的时间上界，仅报告速度下界。真正乱序、重复或无包序号依据的样本继续拒绝。

真实日志离线重解码：人物坐标拒绝数从 8,715 降到 86，额外恢复 8,629 条，坐标值未改写；25 项测试通过。长时间缺少坐标的情况仍存在，详见 [修正与验证范围](COORDINATE_RECOVERY_0510.md)。

0.5.9 加入雷达历史回放。点击雷达上方“回放”，选择主机保存的已结束采集记录，载入后可播放/暂停、拖动进度、选择 0.25–4 倍速或返回实时。人物名称、干员名、队伍编号、武器、头甲环、距离及物资筛选使用同一组显示开关，暂停时也能即时调整。回放只改变当前浏览器视图，不控制正在运行的采集。

0.5.8 将局域网访问收窄为手动选择的单个网卡地址。启动菜单显示网卡、IP/前缀和网关；监听地址、防火墙接口、本地地址和允许的客户端子网保持一致。其他网卡不监听。本机浏览器也使用选中的地址。支持 `-LanIfIndex` / `-LanIp` 指定选择，`-LocalOnly` 仅本机访问。

0.5.7 加入局域网 WebUI：启动脚本默认启用，管理员启动自动放行本地子网访问端口；控制台和启动窗口显示雷达访问链接。`-LocalOnly` 可恢复仅本机访问。保留 Host、同源及 POST 令牌校验。详见 [便携版与局域网访问](PORTABLE.md)。

0.5.6 修复人物坐标更新重建显示数据时遗漏 `equipment` 的问题，避免切枪、倒地状态更新与坐标更新交替到达时头盔/护甲等级闪烁；明确卸下装备仍会正常清除等级。雷达移除人物“暂存”及秒数提示，保留已有位置保留逻辑。回归覆盖状态更新后的坐标投影、真实装备移除，以及 `/api/state`、`/api/live` 的装备一致性；22 项 CTest 均通过。

0.5.5 加入 C++ 地面物资解析、名称及稀有度配色、独立分类/稀有度筛选面板。使用用户物品目录的 3,456 个名称和 65 个分类，支持休眠保留和确认移除、切局清理。详见 [物资显示与验证](LOOT_055.md)。

0.5.2 将原版干员头像绘制在雷达圆圈内，保留队伍色外圈、独立朝向箭头与倒地标记；地图、列表和详情显示干员名称，并可按干员搜索。头像随包提供且每种干员只加载一次。详见 [干员头像与名称](OPERATORS_052.md)。

0.5.1 接入原版 PropertyReplication 血量批次、独立生命状态更新及角色生命周期。雷达显示实际血量、存活/倒地状态；静止、休眠和离开同步范围保留最后位置，明确死亡后隐藏，销毁/卸载/槽位复用/切流时清理。修复旧坐标无法接收新视角的问题。详见 [血量与人物保留规则](HEALTH_LIFECYCLE_051.md)。

0.5.0 增加独立 `/radar` 战术页面：沿用原版地图标定和 120 ms 指数平滑，改用深色浮动面板、圆形方向标记、队伍颜色、人物列表和详情。控制台与雷达共用同一采集服务。C++ 新增下行速度、独立视角更新、游戏时间、已有 PlayerState 状态投影，以及独立保存的下行本人候选状态；修正上行速度/输入加速度命名。18 项测试通过，真实旧日志回放的坐标与质量结果保持一致。详见 [独立雷达与扩展数据](RADAR_050.md)。

0.4.2 在 Windows 共享网络和 Linux AF_PACKET 采集层加入 IPv4 UDP 分片重组，在解密前恢复完整 UDP 数据。支持乱序、重复分片去重、缺片超时、冲突拒绝和有界缓存；WebUI 显示分片、成功、重复、等待、超时及拒绝计数。合成测试中 500 个拆分包恢复出的明文和 992 条人物样本与未拆分基线一致；旧日志未保存原始分片，尚不能确认真实低频更新由分片造成。详见 [UDP 分片重组与验证](UDP_REASSEMBLY.md)。

0.4.1 将雷达从每 500 ms 重绘改为独立动画循环，目标最高 60 FPS；轻量 `/api/live` 请求完成后间隔 33 ms 再取最新位姿，控制台继续每 500 ms 刷新。加入默认 120 ms 延后的坐标/朝向插值、可关闭的平滑选项、真实数据 Hz 和画面 FPS。修复仅包含 Actor GUID 的创建即关闭通道消息被误判为截断；最新日志回放错误由 42 降至 20，坐标与质量结果不变。详见 [日志与刷新率检查](LOG_REVIEW_041.md)。

0.4.0 增加简易二维雷达：圆形人物标记及朝向箭头、本人跟随、范围选择、姓名开关和队伍颜色。底图暂为世界坐标网格。C++ 已接入上行 ch3 本人坐标/朝向/俯仰 full、lite 两类消息，保存样本与验证结果。修复 Character 创建消息多出的 8 位偏移、Controller Owner 桥接以及槽位复用关联，恢复队伍显示。详见 [雷达说明与验证](RADAR.md)。

0.3.6 按原版行为在 `0x10` 对象之前结束当前移动帧读取，保留此前坐标，不再生成这类 `player_decode_warning` 或增加警告计数。样本仍记录解析覆盖范围；其他字段警告与实际错误保持原有处理。

0.3.5 优化按位读取/复制和人物事件 JSON 构造。固定真实日志的内存人物解析中位耗时降低约 37%，包含验证和 JSON 序列化时降低约 24%；相同日志输出文件逐字节一致。完整测量范围和复现方式见 [性能报告](PERFORMANCE.md)。

0.3.4 修复人物校验被无关通道错误连带影响的问题。已确认的移动通道遇到未知 `0x10` 对象或 h21 后续属性时，保留此前明确解出的坐标并标注“部分解析”，不猜测未知字段长度；截断、非法位流及相关通道错误仍影响连续性验证。参见 [SDK 与解析优化报告](SDK_ANALYSIS.md)。

独立编译运行，不修改原有宿主程序。核心使用 C++20 / OpenSSL。

**WebUI：** 双击项目根目录 `start.cmd`（抓包使用 `start-admin.cmd`），或运行 `native/tools/start-webui.ps1`。自动发现局域网设备并选择客户机采集。
便携运行与打包入口见 [PORTABLE.md](PORTABLE.md)。
详见 [WebUI 使用说明](WEBUI.md)，启动后本机和局域网设备均使用选定网卡的访问地址；仅本机模式使用 `127.0.0.1:8765`。

| 功能 | 实现与状态 |
|---|---|
| DH 公钥替换 | TCP 字节流重组 TGCP，每连接两组 DH，替换双方公钥，双向 AES 翻译；已通过内存及真实 loopback socket 测试 |
| UDP bank 解析 | 从 TCP 明文提取规范 Base64 的 128 字节候选；SHA-256 指纹去重 |
| UDP 解密 | 下行 XTEA，每块轮换 8 组子密钥；上行剥离 8 字节前缀；不足 8 字节尾部保留 |
| UDP 分片重组 | 采集副本先按 IPv4 源/目标/IP ID 重组，再过滤 UDP 端口、解密及解析人物；Windows mock 与原生边界测试通过 |
| bank 绑定 | 最近最多 4 包、至少 3 个不同密文，所有包结构有效且只有一个候选通过；连续 3 次失败解除绑定 |
| 人物解析与验证 | C++ 解码 UE packet/bunch、repmove4 坐标与朝向、两版 PlayerState 身份/队伍、Character 显式槽位关联；自动进行时序与合理性验证 |
| 本人与二维雷达 | 独立 `/radar` 页面；原版地图标定、原版平滑/插值、速度/视角详情、上下行本人对照、网格离线模式 |
| 多次采集 | 按设备/会话/UDP flow 比较指纹，记录变化窗口及周期上下界，排除重复输入、断档与合成数据 |
| OpenWrt/Linux | IPv4 AF_PACKET 被动读取 UDP 副本，可选 TCP relay / SO_ORIGINAL_DST；尚未实机编译、抓包验证 |
| Windows 热点/网络共享 | NETWORK_FORWARD 读取客户机 UDP 副本，可选按端口透明接入 DH 中继；预检及合成测试通过，实际客户机联调待验证 |
| WebUI / 设备发现 | C++ HTTP 服务、网卡选择、邻居表 + ICMP 自动扫描、采集启停、实时计数和日志；接口测试及雷达浏览器回放验收通过 |

**采集、解密、远端人物坐标/身份解析与验证均在 C++ 内运行，无需 Python。**
协议目录已嵌入 EXE；支持范围及未移植内容见 [人物解析说明](PLAYER_DECODING.md)。Python 桥接仅保留作开发对照，完整武器/物资/血量专用协议尚未移植。
它使用既有 engine packet / bunch 重组、MovementReplicationActor、repmove4，不扫描固定偏移 float，也不使用实验格式 `<Ifff` 猜坐标。

## 构建

Windows / MSYS2 UCRT64（本机验证通过）：

```powershell
$env:PATH="C:/msys64/ucrt64/bin;C:/Program Files/CMake/bin;"+$env:PATH
cmake -S native -B native/build -G "MinGW Makefiles" `
  -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe `
  -DCMAKE_MAKE_PROGRAM=C:/msys64/ucrt64/bin/mingw32-make.exe `
  -DOPENSSL_ROOT_DIR=C:/msys64/ucrt64 -DCMAKE_BUILD_TYPE=Release
cmake --build native/build -j 4
ctest --test-dir native/build --output-on-failure
native/tools/run-native.ps1 help
```

exe 需要 UCRT64 的 libcrypto、libstdc++、libgcc、winpthread DLL 在 PATH 中。
`tools/run-native.ps1` 自动加入本机已知依赖路径。

Linux / OpenWrt SDK：

```sh
cmake -S native -B native/build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build native/build-linux -j2
ctest --test-dir native/build-linux --output-on-failure
```

需要 C++20 编译器、CMake >= 3.20、目标架构的 OpenSSL libcrypto 开发文件。
OpenWrt 通常在匹配路由器架构的 SDK 中交叉编译：配置 SDK toolchain 与 sysroot，不能复制 Windows exe 到路由器。
本机 WSL 的 `D:/WSL/Ubuntu/ext4.vhdx` 缺失，Linux/OpenWrt 编译及网卡接收路径尚待目标设备验证。

## TCP 获取 bank 与 UDP 旁路采集

0.3.1 的 Windows TCP 接入暂时限定远端端口 65010：选中客户机、开启 TCP 接入，接管该客户机到 65010 的新 IPv4 TCP 连接，其他端口直接通过，从 SYN 映射取得原始目标地址与端口，按握手识别 TGCP mode 3。普通 TCP/TLS、非 mode 3、服务器先发以及 1 秒内不能完成识别的连接原样透传；同网段通信绕过。DH 公钥替换发生后解析出错会关闭该连接，不能再切回透传。

共享 TCP 默认绑定主机私有 IP 的系统分配端口（`HOST_IP:0`），保持同一个监听 socket，再以实际端口创建临时入站规则及回程过滤器。WebUI 不再填写 TCP 服务器 IP、服务端口或监听端口。CLI 的 `--tcp-port`、`--tcp-server` 和非零 `--listen` 仍可显式覆盖；`--tcp-port 0` 恢复全端口接入。WebUI、共享脚本和 CLI 的共享 TCP 默认均为 65010。

DH 公钥只在初始握手替换，后续持续使用两侧会话密钥进行解密/重加密转发，并继续寻找后续 bank。获得 bank 后不能无缝退出中继或转为密文字节直通；否则两端密钥不匹配，且 TCP 本身是两条独立连接。战局切换的判断依据与限制见 SHARED_NETWORK.md。


Windows 主机向客户机共享上网的完整步骤见 [SHARED_NETWORK.md](SHARED_NETWORK.md)。
入口：`native/tools/shared-network.ps1 -Mode Info`；脚本提供显式 ICS 配置、预检、限定客户机的采集和临时防火墙规则清理。

显式上游示例，必须替换实际 IP 和端口：

```sh
./radaer-native capture --out captures/run-001.jsonl --device game-pc \
  --seconds 600 --interface br-lan --client-ip 192.168.1.100 \
  --udp-port 20000 --relay true --listen 0.0.0.0:19001 \
  --upstream 203.0.113.10:19000
```

`203.0.113.10` / `20000` 是说明用占位值，不是探测到的游戏服务。
客户端目标 TCP 必须实际经过监听端口；启动程序本身不会改变路由。
已有精确 TCP REDIRECT 规则的 Linux 环境可把 `--upstream ...` 换成 `--original-destination true`。
上述 Linux/显式 relay 模式不自动写路由或防火墙规则。Windows 共享 TCP 模式会管理临时入站规则。TLS 流量原样透传，不解密 TLS。AF_PACKET 需要 root 或 CAP_NET_RAW。
选靠客户端的单一 LAN 接口，避免多接口重复抓包。

TCP 是应用层中继，系统 TCP 栈负责序号、重传与校验和。
DH 替换之后如果协议、解密或写入失败，关闭连接并记录错误，不能静默恢复原包直通。
退出 relay 会断开它负责的 TCP 连接；设置采集时长时应考虑这一点。
UDP 只复制、解密、分析，程序不会发出或修改 UDP 包。

已有 bank 时仅旁路采集：

```sh
./radaer-native capture --out captures/passive-001.jsonl --device game-pc \
  --seconds 600 --interface br-lan --client-ip 192.168.1.100 \
  --udp-port 20000 /secure/bank.bin
```

bank 文件必须恰好 128 字节原始二进制。只有旁路包，没有 bank 或 DH 中继，不能解密已有会话。
默认只记录 bank 指纹和 UDP 元数据，不保存 bank 内容、DH/AES 密钥、UDP 原始载荷或明文载荷；需要受控诊断时才显式加 `--save-banks DIRECTORY`、`--save-tcp-plaintext true` 或 `--save-udp-payload true`。这些选项产生的原始证据必须妥善保管并设置有限时长/空间。
`--save-tcp-plaintext true` 和 `--save-udp-payload true` 只控制 journal 落盘，不影响进程内协议解析；默认日志可用于流级统计和状态机回归，但不能直接 replay 缺失的 UDP payload。

## 人物样本与可靠性检查

`capture` 和 `replay` 自动解析人物，向同一个采集 JSONL 写入 `player_sample`、`player_identity`、`player_validation`、`player_decode_error` 和结束时的 `player_decoder_summary`。从 0.3.2 起，收到控制通道 Welcome `/Game/...` 后确认战斗流；自动选择最近收到解密 UDP 的已确认流，WebUI 仅展示该流最多 128 个槽位。上下行明文均刷新活跃时间，超过 15 秒没有明文进入 `idle` 并清空当前显示，恢复明文后自动恢复。完整历史持续写盘。

`battle_welcome` 保存首次 Welcome 的 flow、epoch 和地图；`battle_state` 保存等待、活跃、切流、空闲、恢复和采集停止的变化。实时采集用单调时钟独立检查超时，离线回放按输入时间推进。空闲不等于结算；这些状态不会断开 TCP 中继或停止 bank 获取。新 epoch 和绑定重置后需要重新观察 Welcome；中途采集漏掉 Welcome 时仍保存解析样本，但不会自动显示为当前战斗流。

0.3.3 的 WebUI 新增最近 1 秒的 UDP 解密包/秒、当前战斗流坐标更新 Hz 和坐标样本条/秒。解密只计成功输出明文的下行包；坐标 Hz 按源包去重，同包更新多个人物只计一次，样本条数则分别计入。人物表显示每槽位更新 Hz、距上次更新的时间，悬停可查看相邻样本的源包时间间隔。统计使用后端单调时钟，页面每 0.5 秒刷新；静默时速率自动归零，切流和 epoch 变化清空坐标速率。它们反映本机的数据处理节奏，不代表游戏画面 FPS 或网络延迟。

对已有包含 `udp_plaintext` 的采集文件离线解析：

```powershell
native/tools/run-native.ps1 decode-players `
  --in captures/webui/web-xxxx.jsonl --output-directory captures/decoded-001
```

生成 `players.jsonl`、`identities.jsonl`、`player-quality.jsonl`、`self.jsonl`、`self-quality.jsonl`、`self-server.jsonl`、`self-server-quality.jsonl`、`motion-updates.jsonl`、`decoder-errors.jsonl`、`decoder-warnings.jsonl`、`battle-events.jsonl`、`summary.json`，输出目录必须不存在。输入中原有的人物和战斗流派生事件会忽略，避免重复解码。只有密文时，先使用准确 bank 运行 `replay`。

需要自定义速度/坐标阈值时：

```powershell
native/tools/run-native.ps1 validate-players `
  --in captures/decoded-001/players.jsonl --out captures/quality-custom.jsonl `
  --max-coordinate 2097152 --max-speed 10000
```

`10000` 是配置示例，应按实际坐标单位设置。默认至少连续 3 个合理样本；陈旧/连续性断档阈值均 2000 ms。解码器拒绝非有限、全零和超过 UE 世界范围的坐标；质量验证的额外范围、速度阈值默认关闭。

身份仅通过 Character 的 MoveHandle → PlayerState/Owner 显式 GUID 关系关联；无关系或冲突时保留未分类槽位，不猜名字。握手未确认、解码错误、重复、倒序和时间异常会降低质量。`plausible` 表示样本合理性通过，不等于身份或服务器真实性认证。没有可解析人物时明确输出 `no_player_samples`。

## 多次采集和周期分析

```sh
python native/tools/collect.py --exe ./radaer-native --output captures/series-001 \
  --device game-pc --count 3 --seconds 600 --gap 10 -- \
  --interface br-lan --client-ip 192.168.1.100 --udp-port 20000 /secure/bank.bin
./radaer-native report --out captures/bank-report.json captures/series-001/session-*.jsonl
```

collect 启动多个独立窗口，写 `collection.json` 和累计 `report-NNN.json`。
如果额外传 relay 参数，每个窗口结束都会结束中继，下一窗口需要新的握手或显式 bank。
判断会话内轮换应先做足够长的连续采集；比较重新登录/不同对局时独立采集并保持同一个 `--device`。
`--udp-port` 应限定目标服务，防止其他应用 UDP 混入。

统计口径：

- 相同指纹重新绑定、epoch 增加，不算密钥变化。
- bank 候选出现不等于已使用；仅结构验证通过的下行包计入使用证据。
- 25 字节握手外形不足以证明 bank，不作为使用时间证据。
- 变化位于最后一个旧 bank 包与第一个新 bank 包之间，报告 `afterMs` / `byMs`。
- 默认 >5000 ms 观察空档、捕获丢包、重连握手中断连续区间，不把跨会话空档计为周期。
- 连续序列至少 4 次变化/3 个间隔，且稳定、时间分辨率足够才输出候选周期；仍非确定周期。
- 跨多次采集比较指纹集合；不同流本来就可能用不同 bank，不能直接归因为定时轮换。
- 默认排除合成样本；测试展示用 `report --synthetic-only true ...`，输出明确标记 `synthetic_test_only`。
- replay 不是新采集，默认排除出真实采集统计。

## 离线 UDP 重放

```powershell
native/tools/run-native.ps1 replay --in captures/run-001.jsonl `
  --out captures/replay-001.jsonl --device game-pc --synthetic false captures/banks/<sha256>.bin
```

只消费 `udp_raw` 和显式 bank；原始 TCP 握手录制不能当作新 DH 会话重新解密。
输出路径必须不存在，避免覆盖之前的采集结果。

## 验证与限制

- `ctest`：AES 填充、XTEA 尾部、DH、TCP 分片/双向翻译、bank 绑定/重绑、人物质量、合成隔离。
- `tests/oracle`：只反射原程序集的密码学/结构方法；96 组 AES、480 组 UDP、5468 组 envelope、81 组 DH，共 6782 个 C++ 对照检查。
- 原程序集对照工具启动本地模拟客户端/服务端，测试真实 socket 中继，不连接游戏。
- `tests/integration.py`：500 个合成 UDP 包、5 个 bank、4 次切换，重复输入、断档、同 key 不同 epoch、7 类人物样本。
- `tests/player_reference.py`：358 组底层差分向量，另有 36 条非空坐标逐槽位/时间与原 Python 完整链路对照；覆盖两版属性、UTF-8/UTF-16、数组、NewActor 初始变换、乱序/序号回绕、0x10 停止位置及已知前缀。
- `tests/player_integration.py`：原生离线解析、加密 UDP 重放到人物、通道关闭/复用、bank epoch/采集断档和错误后的质量重置。
- `tests/shared_capture_mock.py`：模拟 WinDivert 的 500 包实际经过采集线程、队列、解密、人物解析与日志，得到 992 条非空坐标；未打开真实驱动。

```powershell
dotnet run --project native/tests/oracle -- `
  reverse/host_crypto/bundle/Nva本地离线解析.dll native/build/golden.json `
  native/build/radaer-native.exe native/build/new-loopback.jsonl
native/tools/run-native.ps1 check-vectors --in native/build/golden.json
native/build/decoder-env/Scripts/python.exe native/tests/integration.py native/build/radaer-native.exe
```

未移植原程序两响应握手专用 bank 匹配捷径，采用更保守的多包结构绑定路径。
只捕获 IPv4；UDP IP 分片重组已接入，缺片超时或冲突产生观察缺口；不含 IPv6、pcap 文件读取、完整 C++ UE 语义移植。
Linux 路径使用内核接收时间戳；内核丢包会产生含时间范围的 `capture_gap`，日志序号缺口也会中断周期区间。
尚未验证路由器负载、硬件 offload、真实游戏更新兼容性。
**已使用用户历史采集日志做离线回放对照，但没有独立游戏状态真值，尚不能断言人物坐标准确率或实际 udpbank 变化周期。**

## 源码入口

- `src/core.cpp`：DH / AES / Base64 / XTEA。
- `src/protocol.cpp`：TGCP 流重组、中继状态机、UDP envelope。
- `src/monitor.cpp`：bank broker、日志、人物可靠性。
- `src/report.cpp`：采集比较、变化窗口与候选周期。
- `src/capture.cpp`：socket relay、Linux passive capture。
- `src/shared_windows.cpp`：Windows 网卡预检、WinDivert UDP 副本和透明 TCP 接入。
- `src/shared_packets.cpp`：IPv4/TCP/UDP 包解析和连接映射。
- `src/ipv4_reassembly.cpp`：Windows/Linux 共用的 IPv4 UDP 分片重组与诊断计数。
- `tools/decode_players.py`：旧 Python 参考桥接，仅供开发对照。
- `src/ue.cpp`、`src/ue_movement.cpp`、`src/ue_properties.cpp`：UE 位流、包、移动与属性解析。
- `src/players.cpp`：通道重组、身份关联、样本输出和离线解析。
- `src/battle.cpp`：Welcome 确认、自动切流、15 秒明文静默超时和当前流人物快照。
- `src/rates.cpp`：WebUI 解密/坐标滑动窗口速率和槽位更新时间。

协议依据：项目 `docs/KEY_DECRYPTION_TRACE.md`、`reverse/host_crypto/il` 和 `reverse/protocol_trace`。
第三方 JSON 单头库固定 nlohmann/json v3.11.3，MIT 许可证在 `third_party/nlohmann`。
