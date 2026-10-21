# Radaer 源码整理版

## OpenWrt 分支

`openwrt/tr3000` 分支支持在 ARM64 ImmortalWrt 路由器运行原生服务，提供 LAN WebUI、UDP 采集和 TCP 接入。
安装、构建及卸载说明见 [OpenWrt 部署说明](openwrt/README.md)。

## 当前 C++ 版本启动

双击根目录 `start.cmd` 启动新增 WebUI；需要网络采集时使用 `start-admin.cmd`。
默认地址为 `http://127.0.0.1:8765`，采集保存在 `captures/webui`。
C++ 0.5.2 已包含独立雷达 `http://127.0.0.1:8765/radar`：沿用原版地图标定与运动平滑，重做深色浮动面板、人物列表和详情；扩展速度、视角、人物状态及下行本人候选数据。采集与解析均在 C++ 内运行，无需 Python。Welcome 确认战斗流、按最近明文自动切流、15 秒静默进入空闲。雷达目标最高 120 FPS，分别显示真实坐标 Hz 与画面 FPS。TCP 接入暂时限定服务端口 65010，服务器地址自动获取。详见 [独立雷达与扩展数据](native/RADAR_050.md) 和 [人物解析说明](native/PLAYER_DECODING.md)。
0.5.1 新增真实血量、存活/倒地状态及按生命周期保留人物，修复静止时消失；详见 [血量与人物保留规则](native/HEALTH_LIFECYCLE_051.md)。
0.5.2 增加圆圈内干员头像和名称，资源本地提供，详见 [干员头像与名称](native/OPERATORS_052.md)。
0.5.3 控制台与雷达统一液态玻璃风格，取消暂存人物淡化，WebUI 默认启用 TCP 接入（手动开始采集后生效）；详见 [界面与默认配置](native/GLASS_UI_053.md)。
0.5.4 增加 C++ 手持武器、头盔/护甲等级解析和头像血量环，详见 [装备与血量环](native/EQUIPMENT_054.md)。
首次源码使用需先构建 `native`；分发包已经带好 EXE 和 DLL。
运行 `build/package-native.ps1` 可生成精简运行包与完整项目包，详见 [启动与打包说明](native/PORTABLE.md)。


这是根据原始离线解析程序整理出的可维护工程目录。桌面控制台、浏览器雷达页、数据解析模块和打包配置已经分开；运行时所需的原始协议模块和地图资源放在 `runtime` 中。

## 目录说明

| 目录/文件 | 用途 |
| --- | --- |
| `src/capture_launcher.py` | 桌面程序入口。启动本地 HTTP 控制台、调用采集宿主、管理采集状态，并在浏览器打开雷达页。 |
| `src/gateway_parser.py` | 重构后的解析服务入口，提供状态 API、事件处理和雷达后端接口。 |
| `src/packet_parser.py` | UDP 数据包读取、协议字段解析和坐标/序列号提取。 |
| `src/data_processor.py` | 坐标滤波、坐标变换和数据清洗。 |
| `src/websocket_server.py` | 将解析结果通过 WebSocket/JSON 推送给前端。 |
| `ui/launcher_ui.html` | 桌面控制台界面，包括登录、采集、停止、缓存和 PCAP 目录操作。 |
| `ui/original_radar_page.html` | 浏览器中的 2D/3D 雷达界面。左上角额外窗口按钮已移除，功能台标题副标题为 `佳佳Radaer`。 |
| `ui/radaer.ico` | Radaer 图标。 |
| `build/NvaLocalRadarFull.spec` | PyInstaller 单文件 EXE 配置，使用相对工程路径。 |
| `build/build.ps1` | 编译检查和打包脚本。 |
| `tools/patch_login_title.py` | 将内置解析器资源中的旧标题替换为 `佳佳Radaer`。 |
| `tools/verify_radar_ui.py` | 验证雷达页是否保留/移除指定界面元素。 |
| `tools/verify_title_patch.py` | 验证内置解析器标题替换结果。 |
| `tools/ROLLBACK.sh` | 回滚标题或雷达页修改到保留的原始副本。 |
| `reverse/embedded_gateway.dis.txt` | Python 3.14 字节码反汇编参考。 |
| `reverse/embedded_gateway.pyc` | 原始解析模块的字节码参考文件。 |
| `runtime/vendor_pyc` | 原包中仍以 Python 3.14 字节码形式保留的协议依赖。 |
| `runtime/ErCore` | 地图、3D 模型和前端运行资源。 |
| `runtime/brand_host_login/original_nva.exe` | 内置采集宿主；负责网卡发现、WinDivert 捕获、密钥处理和 UDP 解密。 |
| `runtime/release_resources` | `NvaAssets.pak` 和 WinDivert 驱动文件。 |
| `docs` | 逆向状态、修改差异和验证记录。 |

## 直接使用现成 EXE

已经打包好的程序在：

`C:\Users\Administrator\Desktop\DFM\Radaer.exe`

启动后默认是“停止采集”。点击“开始采集”才启动采集宿主并在默认浏览器打开雷达；PCAP/缓存位于 EXE 同目录的 `captures` 文件夹。

## 从源码打包

要求：Windows、Python 3.14、PyInstaller 6.22 或更新版本、Microsoft WebView2 Runtime，以及能加载 `pythonnet` 的桌面环境。

在 PowerShell 中执行：

```powershell
Set-Location C:\Users\Administrator\Desktop\Radaer
& .\build\build.ps1
```

输出文件为：

`C:\Users\Administrator\Desktop\Radaer\dist\Radaer.exe`

脚本会先编译检查 `src` 下的五个 Python 文件，再检查宿主、地图资源、WinDivert 文件和协议字节码依赖，最后调用 PyInstaller。

## 源码运行控制台

安装 `build/requirements.txt` 中的依赖后，可以从工程根目录运行：

```powershell
Set-Location C:\Users\Administrator\Desktop\Radaer
py -3.14 .\src\capture_launcher.py
```

源码运行会从 `runtime/brand_host_login` 和 `runtime/ErCore/web/m3d` 查找采集宿主及模型资源。桌面控制台使用 `127.0.0.1:17892`，原始解析后端使用 `127.0.0.1:17891`。

## 逆向范围说明

顶层网关、数据处理、WebSocket 和 UI 已整理为 Python/HTML 源码。原包中的部分协议依赖仍是 Python 3.14 字节码，因此 `runtime/vendor_pyc` 是构建输入，不是遗漏的普通 `.py` 文件。`reverse` 目录保存了反汇编和字符串参考，便于继续还原协议字段。

## 验证与回滚

验证记录见 `docs/VERIFICATION.txt`。雷达页 UI 可执行：

```powershell
py -3.14 .\tools\verify_radar_ui.py .\ui\original_radar_page.html modified
```

修改前页面副本为 `original_radar_page.before_radar_controls.html`（保存在原工作目录的完整工程中）；回滚脚本会把它恢复到指定测试副本，不会覆盖当前工作页面。
