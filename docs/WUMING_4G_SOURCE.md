# 无名 4G 源码梳理

> 历史资料：此处对应合并前的 4G 3.0.0 快照。当前统一源码直接在仓库根目录构建，见 [统一固件说明](UNIFIED_S31_NETWORK.md)。

此清单对应 [manifest](../variants/wuming-4g/manifest.json) 中的 54 个文件。基线为 `61a1103781267c33719e94d5873d421a1e7d09bf`，覆盖文件保留来源包字节；未发现运行源码删除项。

## 按职责阅读

| 层次 | 文件/目录（相对 overlay） | 职责 |
| --- | --- | --- |
| 模组驱动 | `components/cellular_modem/` | EC718 AT、拨号、SIM 选择、模块电源/休眠、C 接口 |
| 传输 | `components/uart_uhci/` | UART DMA 缓冲与收发，组件元数据标注 78/uart-uhci 0.2.2 |
| 网络接入 | `components/espressif__iot_eth/` | Ethernet/netif 适配；保留原包自带示例和许可 |
| 网络策略 | `main/app/cellular_policy.c/.h` | Wi-Fi 优先、拨号/重试、扫描暂停与恢复、切卡重拨 |
| 启动/依赖 | `main/CMakeLists.txt`、`boards/metalio47/src/main/app_main.c` | 注册蜂窝组件和策略任务 |
| 设置 | `main/settings.c/.h`、`main/apps/app_device_settings.c` | NVS `cell_en`/`sim_sel`、4G 开关、SIM 选择 |
| 在线业务 | `main/apps/app_weread.c`、`main/book/weread.c` | 4G 在线判断、等待连接、登录超时和 IPv4 |
| 同步/字体 | `main/book/book_xteink_sync.c`、`main/font/online_font.c` | IPv4 与部分状态内存调整 |
| 内存/扫描 | `main/apps/app_book.c`、`app_cloud_sync.c`、`app_transfer.c` | PSRAM 分配和扫描前后蜂窝资源协调 |
| 状态栏/图标 | `main/ui/ui_nav.c`、`tools/gen_ui_icons.py`、SVG 和两板型生成头 | 4G/卡槽图标与状态栏刷新标识 |
| lwIP 配置 | `boards/metalio47/sdkconfig.defaults` | TCP 发送缓冲从 5760 调整为 8192 |

## 硬件及状态边界

驱动默认使用 UART2：TX=58、RX=59、MRDY=60、SRDY=3，工作串口速率 3 Mbaud。模组经 iot_eth 接入 lwIP，而非把每个业务请求改写为 AT HTTP。硬件参数只描述此包默认值，不代表其他 4G 板修订兼容。

`cellular_policy` 管理链路启停；`cellular_modem` 管理 AT/模组；应用经原 HTTP 接口访问网络。Wi-Fi STA 就绪即优先，4G 开关默认开启。`sim_sel` 的设置值 0/1/2 为自动/卡槽/内置，区别于模组物理槽位编号。

策略常量为轮询 3 秒、Wi-Fi 宽限 8 秒、拨号超时 60 秒、失败重试间隔 60 秒。初始 `s_last_dial_ms=0` 也参与重试判断，初次拨号受到 60 秒节流约束。原包说明中的“8 秒拨号”不能直接作为实际初次联网时间承诺。

## 全部覆盖文件

路径均相对 [`overlay/`](../variants/wuming-4g/overlay/)。`modified` 表示替换固定基线同名文件，`added` 表示在基线上新增。

| 类型 | 路径 |
| --- | --- |
| modified | [`boards/metalio47/sdkconfig.defaults`](../variants/wuming-4g/overlay/boards/metalio47/sdkconfig.defaults) |
| modified | [`boards/metalio47/src/main/app_main.c`](../variants/wuming-4g/overlay/boards/metalio47/src/main/app_main.c) |
| modified | [`boards/metalio47/src/main/assets/ui_icons.h`](../variants/wuming-4g/overlay/boards/metalio47/src/main/assets/ui_icons.h) |
| modified | [`boards/pico/src/main/assets/ui_icons.h`](../variants/wuming-4g/overlay/boards/pico/src/main/assets/ui_icons.h) |
| added | [`components/cellular_modem/CMakeLists.txt`](../variants/wuming-4g/overlay/components/cellular_modem/CMakeLists.txt) |
| added | [`components/cellular_modem/include/cellular_modem.h`](../variants/wuming-4g/overlay/components/cellular_modem/include/cellular_modem.h) |
| added | [`components/cellular_modem/include/uart_eth_modem.h`](../variants/wuming-4g/overlay/components/cellular_modem/include/uart_eth_modem.h) |
| added | [`components/cellular_modem/src/cellular_modem.cpp`](../variants/wuming-4g/overlay/components/cellular_modem/src/cellular_modem.cpp) |
| added | [`components/cellular_modem/src/uart_eth_modem.cc`](../variants/wuming-4g/overlay/components/cellular_modem/src/uart_eth_modem.cc) |
| added | [`components/espressif__iot_eth/CHANGELOG.md`](../variants/wuming-4g/overlay/components/espressif__iot_eth/CHANGELOG.md) |
| added | [`components/espressif__iot_eth/CMakeLists.txt`](../variants/wuming-4g/overlay/components/espressif__iot_eth/CMakeLists.txt) |
| added | [`components/espressif__iot_eth/examples/usb_rndis_4g_module/CMakeLists.txt`](../variants/wuming-4g/overlay/components/espressif__iot_eth/examples/usb_rndis_4g_module/CMakeLists.txt) |
| added | [`components/espressif__iot_eth/examples/usb_rndis_4g_module/components/app_wifi/app_wifi.c`](../variants/wuming-4g/overlay/components/espressif__iot_eth/examples/usb_rndis_4g_module/components/app_wifi/app_wifi.c) |
| added | [`components/espressif__iot_eth/examples/usb_rndis_4g_module/components/app_wifi/CMakeLists.txt`](../variants/wuming-4g/overlay/components/espressif__iot_eth/examples/usb_rndis_4g_module/components/app_wifi/CMakeLists.txt) |
| added | [`components/espressif__iot_eth/examples/usb_rndis_4g_module/components/app_wifi/include/app_wifi.h`](../variants/wuming-4g/overlay/components/espressif__iot_eth/examples/usb_rndis_4g_module/components/app_wifi/include/app_wifi.h) |
| added | [`components/espressif__iot_eth/examples/usb_rndis_4g_module/components/app_wifi/Kconfig`](../variants/wuming-4g/overlay/components/espressif__iot_eth/examples/usb_rndis_4g_module/components/app_wifi/Kconfig) |
| added | [`components/espressif__iot_eth/examples/usb_rndis_4g_module/main/CMakeLists.txt`](../variants/wuming-4g/overlay/components/espressif__iot_eth/examples/usb_rndis_4g_module/main/CMakeLists.txt) |
| added | [`components/espressif__iot_eth/examples/usb_rndis_4g_module/main/idf_component.yml`](../variants/wuming-4g/overlay/components/espressif__iot_eth/examples/usb_rndis_4g_module/main/idf_component.yml) |
| added | [`components/espressif__iot_eth/examples/usb_rndis_4g_module/main/Kconfig.projbuild`](../variants/wuming-4g/overlay/components/espressif__iot_eth/examples/usb_rndis_4g_module/main/Kconfig.projbuild) |
| added | [`components/espressif__iot_eth/examples/usb_rndis_4g_module/main/usb_rndis_4g_module.c`](../variants/wuming-4g/overlay/components/espressif__iot_eth/examples/usb_rndis_4g_module/main/usb_rndis_4g_module.c) |
| added | [`components/espressif__iot_eth/examples/usb_rndis_4g_module/sdkconfig.defaults`](../variants/wuming-4g/overlay/components/espressif__iot_eth/examples/usb_rndis_4g_module/sdkconfig.defaults) |
| added | [`components/espressif__iot_eth/examples/usb_rndis_4g_module/sdkconfig.defaults.esp32p4`](../variants/wuming-4g/overlay/components/espressif__iot_eth/examples/usb_rndis_4g_module/sdkconfig.defaults.esp32p4) |
| added | [`components/espressif__iot_eth/examples/usb_rndis_4g_module/sdkconfig.defaults.esp32s3`](../variants/wuming-4g/overlay/components/espressif__iot_eth/examples/usb_rndis_4g_module/sdkconfig.defaults.esp32s3) |
| added | [`components/espressif__iot_eth/idf_component.yml`](../variants/wuming-4g/overlay/components/espressif__iot_eth/idf_component.yml) |
| added | [`components/espressif__iot_eth/include/iot_eth.h`](../variants/wuming-4g/overlay/components/espressif__iot_eth/include/iot_eth.h) |
| added | [`components/espressif__iot_eth/include/iot_eth_netif_glue.h`](../variants/wuming-4g/overlay/components/espressif__iot_eth/include/iot_eth_netif_glue.h) |
| added | [`components/espressif__iot_eth/include/iot_eth_types.h`](../variants/wuming-4g/overlay/components/espressif__iot_eth/include/iot_eth_types.h) |
| added | [`components/espressif__iot_eth/interface/iot_eth_interface.h`](../variants/wuming-4g/overlay/components/espressif__iot_eth/interface/iot_eth_interface.h) |
| added | [`components/espressif__iot_eth/iot_eth.c`](../variants/wuming-4g/overlay/components/espressif__iot_eth/iot_eth.c) |
| added | [`components/espressif__iot_eth/iot_eth_netif_glue.c`](../variants/wuming-4g/overlay/components/espressif__iot_eth/iot_eth_netif_glue.c) |
| added | [`components/espressif__iot_eth/license.txt`](../variants/wuming-4g/overlay/components/espressif__iot_eth/license.txt) |
| added | [`components/uart_uhci/CMakeLists.txt`](../variants/wuming-4g/overlay/components/uart_uhci/CMakeLists.txt) |
| added | [`components/uart_uhci/idf_component.yml`](../variants/wuming-4g/overlay/components/uart_uhci/idf_component.yml) |
| added | [`components/uart_uhci/include/uart_uhci.h`](../variants/wuming-4g/overlay/components/uart_uhci/include/uart_uhci.h) |
| added | [`components/uart_uhci/README.md`](../variants/wuming-4g/overlay/components/uart_uhci/README.md) |
| added | [`components/uart_uhci/README_zh.md`](../variants/wuming-4g/overlay/components/uart_uhci/README_zh.md) |
| added | [`components/uart_uhci/src/uart_uhci.cc`](../variants/wuming-4g/overlay/components/uart_uhci/src/uart_uhci.cc) |
| added | [`main/app/cellular_policy.c`](../variants/wuming-4g/overlay/main/app/cellular_policy.c) |
| added | [`main/app/cellular_policy.h`](../variants/wuming-4g/overlay/main/app/cellular_policy.h) |
| modified | [`main/apps/app_book.c`](../variants/wuming-4g/overlay/main/apps/app_book.c) |
| modified | [`main/apps/app_cloud_sync.c`](../variants/wuming-4g/overlay/main/apps/app_cloud_sync.c) |
| modified | [`main/apps/app_device_settings.c`](../variants/wuming-4g/overlay/main/apps/app_device_settings.c) |
| modified | [`main/apps/app_transfer.c`](../variants/wuming-4g/overlay/main/apps/app_transfer.c) |
| modified | [`main/apps/app_weread.c`](../variants/wuming-4g/overlay/main/apps/app_weread.c) |
| modified | [`main/book/book_xteink_sync.c`](../variants/wuming-4g/overlay/main/book/book_xteink_sync.c) |
| modified | [`main/book/weread.c`](../variants/wuming-4g/overlay/main/book/weread.c) |
| modified | [`main/CMakeLists.txt`](../variants/wuming-4g/overlay/main/CMakeLists.txt) |
| modified | [`main/font/online_font.c`](../variants/wuming-4g/overlay/main/font/online_font.c) |
| modified | [`main/settings.c`](../variants/wuming-4g/overlay/main/settings.c) |
| modified | [`main/settings.h`](../variants/wuming-4g/overlay/main/settings.h) |
| modified | [`main/ui/ui_nav.c`](../variants/wuming-4g/overlay/main/ui/ui_nav.c) |
| modified | [`tools/gen_ui_icons.py`](../variants/wuming-4g/overlay/tools/gen_ui_icons.py) |
| added | [`tools/icons/custom/signal-4g.svg`](../variants/wuming-4g/overlay/tools/icons/custom/signal-4g.svg) |
| added | [`tools/icons/custom/sim-card.svg`](../variants/wuming-4g/overlay/tools/icons/custom/sim-card.svg) |

## 来源资料取舍

来源包 `03-docs/4g-change-inventory.md` 的部分描述与本次实际比对不一致（例如提到封面模块修改，但该文件与基线一致），因此此处按文件实际差异重新梳理。原包重复补丁、`docs/local` 中的图标预览与本地记录不纳入运行源码。完整来源压缩包的 SHA-256 保存在 manifest，可用于追溯输入。
