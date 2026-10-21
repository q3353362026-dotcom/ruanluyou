# Radaer Windows 启动与打包

## 直接启动

1. 将 ZIP **完整解压**到可写目录，不要直接在压缩软件中运行 EXE。
2. 双击根目录 `start-admin.cmd`，在启动菜单中选择一个网卡/IP（显示名称、接口编号、IP/前缀和网关）；输入 `0` 为仅本机。确认后启动 WebUI，并在默认浏览器打开选中地址。
   0.5.8 只对选中的网卡地址开放，自动防火墙规则同时限定网卡、本地 IP 和该 IP 的子网。手机或其他电脑连接该网络后，打开启动窗口或控制台“独立战术雷达”中显示的 `http://选中网卡IP:8765/radar`。该模式也允许这些设备操作采集控制台；本机也使用此地址，不监听其他网卡或 loopback。
3. 需要抓包时使用 `start-admin.cmd`，接受 Windows UAC 提示；普通启动也可查看网卡、发现设备。
4. 共享上网先在 Windows 中配置热点或 ICS。进入页面后选择客户机所在网卡、扫描并选择设备，再检查配置及开始采集。
5. 需要通过 TCP 获取 bank 时勾选“TCP 接入（端口 65010）”，服务器地址自动识别；先启动采集，再让客户机重新连接。
6. 保留控制台窗口；Ctrl+C 停止服务及采集。仅关闭浏览器不会停止采集。

输出默认保存到解压目录的 `captures/webui`。端口冲突时选择其他端口：
0.5.9 可在雷达顶部点击“回放”，载入该目录中已经结束的采集记录；支持暂停、进度拖动和倍速。暂停时人物信息开关仍有效。历史采集记录不打入便携包，升级后需保留原来的 `captures/webui` 或通过 `-OutputDirectory` 指向原目录。

0.5.0 的独立雷达地址为 `http://127.0.0.1:8765/radar`，也可从控制台的「打开雷达」进入；只需运行一份服务。雷达页面和 Leaflet 随包提供，地图瓦片需要联网，离线可使用坐标网格。详见 [RADAR_050.md](RADAR_050.md)。

```powershell
.\start.cmd -Port 8766
.\start.cmd -NoBrowser -OutputDirectory 'D:\Radaer Captures'
.\start.cmd -LocalOnly
.\start-admin.cmd -LanIfIndex 8
.\start-admin.cmd -LanIfIndex 8 -LanIp 192.168.137.1
.\start.cmd -Check
.\native\tools\run-native.ps1 network-info
```

`-Check` 检查 EXE 能否启动，以及页面和驱动文件是否齐全，不加载抓包驱动或修改共享网络。
所有资源按脚本目录定位，可以从其他工作目录启动。脚本不会修改系统 PATH 或全局执行策略。
运行版已附带 OpenSSL、GCC、winpthread DLL 和 WinDivert DLL/驱动；原生 WebUI 无需安装 MSYS2、CMake、Node 或 Python。
适用于 Windows x64；采集及自动放行防火墙需要管理员权限。`-LocalOnly` 恢复仅本机访问。直接运行 EXE 时必须同时指定 `webui --lan true --lan-if-index 接口编号 --lan-ip 网卡IPv4`，防火墙由启动脚本管理。地址与接口不匹配、网卡停用或漏填参数时拒绝启动，不回退为全部网卡。

默认端口为 8765，`-Port` 同时更改监听端口和防火墙规则；服务正常退出时删除本次规则。强制结束进程或断电可能残留以 `Radaer-WebUI-` 开头的规则。只允许选中私有 IPv4 地址的对应子网访问，换网或开启热点后需重启服务并重新选择。网关仅用于帮助辨认网络，实际选择的是主机网卡 IP；热点网卡没有上游网关也可选择。客户端 Wi-Fi 隔离会阻止设备互访；不需要配置路由器公网端口映射。

## 两种压缩包

- `Radaer-版本-windows-x64.zip`：精简运行包，包含原生 EXE、所需 DLL、WinDivert、WebUI、启动/共享脚本和说明。
- `Radaer-版本-project.zip`：当前完整工程快照，附带相同的原生运行文件，同时保留 C++/Python 源码、原始宿主、协议字节码、地图资源及逆向资料。排除编译缓存、虚拟环境、采集记录和旧发行包。

`MANIFEST.json` 记录包内文件大小和 SHA-256；`SHA256SUMS.txt` 记录两个 ZIP 的 SHA-256。
项目包根目录的启动脚本默认启动新增 C++ WebUI；旧 Python 桌面入口仍是 `src/capture_launcher.py`。
人物坐标、朝向、名字/队伍关联与质量验证已包含在原生 EXE 中，精简运行包也可直接运行，不需要 Python、协议字节码或外部 schema。采集时自动解析保存；离线命令和未支持范围见 [PLAYER_DECODING.md](PLAYER_DECODING.md)。

## 重新打包

在完整工程目录运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\build\package-native.ps1
```

需要 CMake、MSYS2 UCRT64 C++/OpenSSL 工具链及 Python 3.11+；已有 Python 3.14 解码虚拟环境时优先使用。
可用 `-ToolchainRoot`、`-BuildDirectory`、`-OutputDirectory`、`-Python` 指定路径。
默认构建并运行 CTest，通过后递归收集 DLL、复制许可文本、验证无开发 PATH 时的启动和网卡查询、生成 ZIP 并检查 CRC。
`-SkipBuild` / `-SkipTests` 仅适用于已有构建；跳过测试会在打包记录中明确标注。
每次输出到独立的 `dist/Radaer-版本-时间戳`，不会覆盖旧包。

0.5.0 已使用真实旧日志完成桌面/窄屏雷达浏览器验收；当前版本尚未开启新一轮实机采集验证。数据与测试范围见 `native/RADAR_050.md`，早期版本验证记录保留在 `native/VALIDATION.json` 与 WebUI 说明。
