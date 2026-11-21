# 无名 4G 版微读 3.0.0

> 历史资料：此处对应合并前的 4G 3.0.0 快照。当前统一源码直接在仓库根目录构建，见 [统一固件说明](UNIFIED_S31_NETWORK.md)。

本版面向用户所称的**无名 4G 版**，随附源码的板型标识仍为 `metalio47`，固件名仍包含 Metalio S31。硬件配置为 ESP32-S31、16 MiB 闪存、EC718 Cat.1 模组。普通 S31 与无名 4G 下载项分别标注，请按实际设备选择。

## 来源与仓库组织

来源为用户提供的 `kiikoread-4g-v3.0.0-release.zip`。逐文件比对确认，它基于本仓库 v3.0.0（提交 `61a1103781267c33719e94d5873d421a1e7d09bf`），有 54 个运行源码、构建或图标文件差异。保存于 [`variants/wuming-4g/overlay/`](../variants/wuming-4g/overlay/)，并用 [manifest](../variants/wuming-4g/manifest.json) 固定基线和校验值。

采用固定基线加覆盖文件的方式收录完整可还原源码，避免复制整套共享代码，也避免把 4G 依赖带入普通 S31/Pico 默认构建。后续根目录代码变化不会自动进入这个 3.0.0 变体；升级基线需重新合并并验证。

压缩包中的本地调试文档、图标预览、分段固件、重复源码及补丁未作为发行附件上传。完整源码路径和模块边界见 [4G 源码梳理](WUMING_4G_SOURCE.md)。

## 4G 行为

- Wi-Fi STA 在线时优先使用 Wi-Fi，停止 4G 链路；无 Wi-Fi 且 4G 开关开启时尝试拨号。
- 设置中加入“4G 网络”和“SIM 卡”；可选择自动、卡槽卡、内置卡。自动模式先试卡槽，无卡时再回退内置卡。
- 状态栏显示 4G 状态；微信读书在线判断纳入蜂窝连接，登录长轮询使用更长超时，相关 HTTP 请求强制 IPv4。
- 扫描 Wi-Fi 时暂时释放 4G 资源，扫描后恢复；书架、二维码和部分同步状态转移到 PSRAM，以减轻内部 RAM 压力。
- 保留 v3.0.0 的分章节按需缓存、续传、书架同步、账户与阅读进度功能。

这些行为来自代码检查；不是本次新增的设备验收结论。源码初次拨号也受开机后 60 秒重试节流条件影响，不能把 8 秒 Wi-Fi 宽限期理解为必定 8 秒联网。

## 构建

需要 Git、Python **3.12+**（还原工具使用安全 tar 解包过滤器）与 ESP-IDF **v6.1** 正式版。依赖缓存未随包提供，首次配置需要联网下载。先在本仓库根目录还原源码，输出目录必须为空：

```powershell
python tools/materialize_wuming_4g.py --output build/wuming-4g/source
Set-Location build/wuming-4g/source
python tools/build_firmware.py --board metalio47 --idf-metalio47 $env:IDF_PATH --python (Get-Command python).Source --output build/boards --s31-pie --s31-text-ripple --s31-reader-panel37
```

Linux/macOS 中先执行还原命令并 `cd build/wuming-4g/source`，再使用：

```bash
python tools/build_firmware.py --board metalio47 --idf-metalio47 "$IDF_PATH" --python "$(command -v python)" --output build/boards --s31-pie --s31-text-ripple --s31-reader-panel37
```

`metalio47` 为沿用的内部板型名；在还原的源码树中构建才包含 4G。三个 S31 开关对应原包声明的构建选项。通用环境说明见 [S31 构建指南](BUILD_METALIO_S31.md)。本次发布使用原包固件，未宣称本次重新编译或做过硬件测试，也未验证重新构建能够逐字节复现附件。

## 下载与全闪存刷写

在 [v3.0.0 发行版](https://github.com/YIYO-Designer/kiikoreader-wereader/releases/tag/v3.0.0) 额外提供一个附件：

`Kiikoread_Metalio_S31_4G_full_16MB.bin`

- 长度：**16,777,216 字节（16 MiB）**。
- 写入地址：**`0x0`**。
- SHA-256：`d018e213b2eda50ac8945e5a745f88592720e9436e16f7cf26dd562cbf245fe7`。
- MD5（与原包核对）：`7497f264a4797e3c07d01fa796449799`。

这是原包全闪存镜像的原始字节。只新增这一份 `.bin` 附件；普通 S31 已有固件继续保留，源码归仓库管理。

**刷写会覆盖整片闪存，清空设备设置、Wi-Fi、登录状态及内部 storage 书库/缓存。请先备份需要保留的内容。** 安装支持 `esp32s31` 的 esptool，在固件文件所在目录执行，替换串口号：

```powershell
Get-FileHash .\Kiikoread_Metalio_S31_4G_full_16MB.bin -Algorithm SHA256
python -m esptool --chip esp32s31 --port COM17 --baud 460800 write-flash 0x0 .\Kiikoread_Metalio_S31_4G_full_16MB.bin
```

刷写前关闭占用串口的工具。检查写入校验成功及工具正常完成；出现错误不要仅凭零散成功日志认定完成。

| 区域 | 地址 | 长度 |
| --- | --- | --- |
| Bootloader | `0x2000` | 镜像内已有 |
| 分区表 | `0x8000` | 镜像内已有 |
| NVS | `0x9000` | 16 KiB |
| OTA data | `0xd000` | 8 KiB |
| PHY init | `0xf000` | 4 KiB |
| ota_0 | `0x10000` | 6 MiB |
| ota_1 | `0x610000` | 6 MiB |
| storage | `0xc10000` | `0x3e0000` |
| coredump | `0xff0000` | 64 KiB |

全镜像、源码板级配置和分段固件的分区布局在本次收录时交叉核对；分段固件只用于校验，不作为附件发布。

## 已知限制与验收范围

原包记录部分内置物联网卡无法访问 `weread.qq.com`；微信读书建议使用能正常访问该服务的外置 SIM 或 Wi-Fi。连接上 4G 不代表服务端一定允许访问。本次未重新实测卡片兼容性，也未独立确认网络拦截原因。

原包记录 4G 与 Wi-Fi 会争用内部 RAM，Wi-Fi 扫描会临时断开 4G，可能短暂停顿。拨号、切卡、扫描后重连、真实阅读同步和功耗仍需设备验证。根目录现有 CI 检查普通 S31/Pico，不代表该固定基线 4G 变体已通过设备验收。

新增或导入组件各自沿用文件头、组件清单和原许可；见 [第三方说明](../THIRD_PARTY_NOTICES.md)。

## 本次收录验证（2026-11-21）

- 完整源码还原后，1,739 个非本地文档文件与来源包逐一比较一致（忽略文本 CRLF/LF 差异），基线无删除文件。
- 54 个覆盖文件以原字节保存并逐个校验 SHA-256；拒绝向非空目录还原。
- 16 MiB 镜像的 bootloader、分区表、OTA data、app 四段均与原包分段文件相同；MD5 与原包记录一致，发布另记 SHA-256。
- 4G 源码的 metalio47 板级视图生成成功；根项目及还原后的项目均通过 `board_layer_host_test.py`。
- 两套源码的 `license_notices_host_test.py` 均报告既有缺失文件 `licenses/LICENSE-CrossMux.txt`；该问题在未修改的 v3.0.0 基线上亦存在，本次未补写或猜测许可文本。
- 未执行 ESP-IDF 完整编译和实机测试。
