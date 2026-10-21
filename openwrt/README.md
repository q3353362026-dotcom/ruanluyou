# Radaer on OpenWrt / ImmortalWrt

本分支提供原生 C++ 服务、LAN WebUI、Linux AF_PACKET UDP 采集、nftables TCP 接入和 procd 开机启动。
无需在路由器安装 Python、Node.js 或 Windows 驱动。
安装包为 musl 线程设置 1 MiB 默认栈，以容纳采集缓冲区和解码调用。

## 使用

安装后打开 `http://路由器LAN地址:8765`，雷达位于 `/radar`。
选择 `br-lan` 和客户机 IPv4，检查配置后开始采集。客户机应通过这台路由器上网。
默认仅接入该客户机访问远端 TCP 65010 的新连接；启动采集后需让客户机重新建立连接。
停止采集会关闭本次接入的 TCP 连接，删除应用专用 `ip radaer_native` 表。
UDP 只做副本采集。未启动采集时服务不会添加重定向规则。

Linux 设备发现读取邻居缓存，并用 DHCP 租约补充名称；缓存设备显示“待确认”，不做主动在线判断。
未列出的设备可以手动输入 IP。采集仅支持 IPv4；应关闭会绕过 CPU 采集路径的软/硬件流量卸载。
在 LAN 默认允许访问路由器自身的标准防火墙配置下工作；自定义拒绝 LAN 输入的规则需要允许临时 TCP 中继端口。

WebUI 只监听所选 LAN IPv4，检查来源网段、Host、Origin 和写请求令牌。
同一 LAN 的用户可以控制采集，因此应仅放在可信 LAN 中，不做公网端口转发。

## 安装与管理

### LuCI 服务菜单

安装 `luci-app-radaer_0.5.12-r2_all.ipk` 后，刷新 LuCI，在 **服务 → Radaer 雷达** 管理。
页面使用 LuCI 原生表单，随 Aurora / Bootstrap 等主题显示，支持状态、内存、启停、自启、
监听网络、端口、保存目录、最近日志和控制台/雷达入口。实际采集和雷达继续使用独立网页。

```sh
opkg install /tmp/luci-app-radaer_0.5.12-r2_all.ipk
```

修改设置后点击“保存并应用”，LuCI 应用 UCI 配置并触发服务重新加载。
“启用服务”控制服务是否允许运行；“开机启动”控制启动项，两者独立。
管理页需要 LuCI 登录，rpcd 权限限定为 Radaer 配置、状态、日志和固定的服务操作。
卸载管理页可执行 `opkg remove luci-app-radaer`，不会卸载采集服务或删除数据。

在已构建核心包的 SDK 中，执行 `sh openwrt/build-luci.sh /path/to/sdk 4` 生成独立 LuCI 包。

使用与目标架构匹配的 IPK。在路由器执行：

```sh
opkg install /tmp/radaer-native_0.5.12-r9_aarch64_cortex-a53.ipk
/etc/init.d/radaer enable
/etc/init.d/radaer restart
/etc/init.d/radaer status
logread -e radaer
```

依赖为 `libstdcpp6`、`libopenssl3`、`nftables-json` 及其运行依赖。
已有依赖时可离线安装 IPK；缺少依赖时使用当前固件匹配的软件源，避免替换不同内核版本的模块。

UCI 配置在 `/etc/config/radaer`：

```uci
config radaer 'main'
    option enabled '1'
    option network 'lan'
    option port '8765'
    option output_directory '/opt/radaer/captures'
```

修改后执行 `/etc/init.d/radaer restart`。服务随所选接口重连而重启。
日志与 bank 保存在输出目录；启用 Extroot 后它位于 U 盘上，运行期间应保持 U 盘连接。
页面可关闭“保存日志”以减少写入。文件不会自动轮换，应定期清理不需要的采集记录。
在 LuCI 点击“清理采集日志”，或在采集控制台的“运行日志”区点击“清理历史采集日志”，
再点击页面确认框中的“确认删除历史日志”。只删除输出目录中已结束的 `web-*.jsonl`，
保留活动日志、bank、其他文件及子目录；清理后无法回放已删除的记录。不会清除路由器的系统日志。
日志由独立线程批量写入，正常停止会等待队列写完；队列最多 8 MiB，存储长期跟不上时会报告
错误并结束采集，不会静默丢弃日志。异常断电仍可能损失尚未落盘的记录。
Linux 抓包在内核过滤目标客户机的 IPv4 UDP，保留双向流量和 IP 分片；优先申请单个 socket
的 4 MiB 接收缓冲（Linux 返回的计量通常为 8 MiB），权限不足时回退到系统允许的大小。
`capture_stats` 记录实际缓冲、累计收包/丢包和本统计周期最大处理延迟；丢包指采集副本丢失。
网页采集时长默认 `0`（不限时），也可以输入正整数秒数定时结束；命令行使用 `--seconds 0`。
采集运行期间，目标程序关闭再打开会自动接入新的 TCP 连接并重新获取 bank，无需重新开始采集。
目标设备 IP 必须保持不变且 TCP 接入已启用。停止采集、服务或路由器重启后需手动重新开始采集。
若采集丢包导致会话状态失效，界面会提示“采集丢包，请重新进入对局”。监听仍在运行，
新战斗连接确认后自动恢复；缺失初始化时不会沿用旧人物身份，不能保证同一对局无缝恢复。
服务启动、异常退出重启、停止时会清理应用专用 nft 表。

雷达默认收起设置与人物列表，点击底部图标展开。手机和平板使用左上角地图下拉菜单；
等待数据提示放在屏幕边缘。历史回放在“设置 → 历史回放”中打开。
新采集的人物使用已解析的非零 UIN 生成连接内的匿名身份键，同账号换角色/槽位会替换旧样本；
仅名字相同不会合并。时间更早的样本不能覆盖新身份，雷达也立即清除被替换的旧人物缓存。
旧日志没有该身份键时仍按 Actor/PlayerState GUID 匹配；完整验证账号迁移需重新解码原始记录。

从 r9 起，“物资”页最上方固定显示“容器”分类，包含首领死亡盒、角色死亡盒、人机死亡盒、
普通容器和类型未知的死亡盒，每项可独立开关并保存选择。容器不受物品稀有度筛选影响，
地图使用箱形标记；确实收到搜空标记的死亡盒显示“已搜空”。
死亡盒使用专用 RepLayout、死者类型字段和骨骼类型区分，未收到类型时不猜测。
普通容器只接受已识别类的出生位置，静态对象没有同步位置时不显示；没有解析箱内物品清单。
休眠或离开同步范围保留最后位置，销毁或隐藏时移除，切换连接/战局清除旧数据。
旧日志里的 `loot_state` 不会自动补齐新类别，新增容器记录从升级后的采集开始产生。

使用 MediaTek 原厂驱动的固件还可能启用独立的 TurboACC HNAT，即使防火墙中的
软件/硬件流量分载均已关闭，它也可能让转发流量绕过 AF_PACKET。表现为下行收包稀疏、
坐标更新慢、身份或通道分片缺失。可检查 `cat /sys/kernel/debug/hnat/hook_toggle`。
从 r7 起，Radaer 服务启动、重启及 procd 恢复进程时自动关闭此运行时开关，停止服务时自动开启；
关闭 UCI“启用服务”再应用也会开启。LuCI 状态行显示实际开关状态。此联动针对服务启停，
仅停止采集或关闭雷达网页不会开启加速。没有该 HNAT 接口的设备跳过操作。
不会更改 TurboACC 的持久化配置；服务运行时关闭加速可能降低转发吞吐。
若另外在 TurboACC 中重新应用配置，可能覆盖运行时开关，可重启 Radaer 服务重新关闭。

卸载（保留采集文件）：

```sh
/etc/init.d/radaer stop
/etc/init.d/radaer disable
opkg remove radaer-native
```

## 构建

使用 Linux 或 WSL 和与固件系列/CPU 匹配的 OpenWrt SDK。此部署使用 ImmortalWrt 24.10.2
`mediatek/filogic` SDK，GCC 13.3 / musl / `aarch64_cortex-a53`，针对已装有 OpenSSL 3 的 TR3000。
这是用户态程序，不向路由器安装 SDK 生成的内核模块。

官方 SDK：
<https://downloads.immortalwrt.org/releases/24.10.2/targets/mediatek/filogic/>

文件 `immortalwrt-sdk-24.10.2-mediatek-filogic_gcc-13.3.0_musl.Linux-x86_64.tar.zst`，
SHA256：`37f2112956fd362011374a94f387ab7e5b03fda6ed45e42488f289a53dcae3b6`。

Ubuntu 构建依赖：

```sh
sudo apt-get install build-essential cmake ninja-build python3 python3-setuptools swig \
    rsync git curl wget unzip gawk zstd libncurses-dev zlib1g-dev file
```

将 SDK 解压到 Linux 文件系统，然后配置与 SDK 对应的基础 feed：

```sh
cd /path/to/sdk
printf '%s\n' 'src-git base https://github.com/immortalwrt/immortalwrt.git;v24.10.2' > feeds.conf
./scripts/feeds update base
./scripts/feeds install -a -p base
sh /path/to/Radaer/openwrt/build-sdk.sh "$PWD" 4
```

SDK 默认可能启用所有包/内核模块，会增加首次构建时间。可在 SDK `make menuconfig` 关闭
`Select all target specific packages`、`Select all kernel module packages`、`Select all userspace packages`。
不要在不同 SDK 间复用 build/staging 目录。以 root 构建时 SDK 可能要求显式设置 `FORCE=1`。
输出在 `SDK/bin/packages/*/base/radaer-native_*.ipk`。

## 本地验证

```sh
sudo apt-get install libssl-dev python3 nodejs iproute2 nftables
cmake -S native -B build/linux-native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux-native -j4
ctest --test-dir build/linux-native --output-on-failure
sudo python3 native/tests/linux_capture.py build/linux-native/radaer-native native/webui
```

Linux 网络集成测试在独立网络命名空间中验证真实 TCP 重定向与原目的地址恢复、UDP 副本采集、
配置拒绝、停止和 SIGTERM 清理，并检查无关防火墙表仍存在。缺少 root/ip/nft 时该项跳过。
旧 Python 参考字节码需要 Python 3.14 与本地 runtime；缺少它们时保留原生断言，单独说明参考比对跳过。
Windows 测试保留原有驱动模拟和 Python 3.14 参考比对。
