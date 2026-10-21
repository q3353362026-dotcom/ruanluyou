# radaer-native CI build

这个仓库只用于通过 GitHub Actions 交叉编译 `radaer-native`（OpenWrt aarch64/musl）。

## 为什么

目标路由器：Kwrt 25.12 / kernel 6.12.79 / aarch64_cortex-a53 / musl 1.2.5

本地没有可用的 aarch64-musl 交叉工具链（Windows），路由器本身也装不下 gcc
（overlay 130MB，gcc 解包 180MB）。所以用 GitHub Actions 的 OpenWrt SDK 编译。

## 产物

`radaer-native_0.5.12-r9_aarch64_cortex-a53.ipk`

安装：
```sh
opkg install /tmp/radaer-native_0.5.12-r9_aarch64_cortex-a53.ipk --force-reinstall
/etc/init.d/radaer restart
```

## 包含的修复

| 文件 | 修复 |
|---|---|
| `native/src/capture.cpp` | 接收缓冲 4MB → 64MB；内核丢包不再触发 `capture_gap`（原来一丢包就清空全部流，整局作废） |
| `native/src/players.cpp` | `status=="error"` 但有字段时降级为 `prefix_only`，不再整条丢弃；movement h21 解析失败只告警不抛异常 |
| `native/src/ue_properties.cpp` | 未知 native struct（Opaque）保留已解析的前缀字段（Owner/PlayerState/MoveHandle） |

## 触发

push 到 main，或在 Actions 页面手动 Run workflow。

## 本地等价命令

```sh
./openwrt/build-sdk.sh /path/to/openwrt-sdk 2
```
