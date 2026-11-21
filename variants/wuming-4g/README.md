# 无名 4G · 微读 3.0.0 源码变体

此目录保存用户提供的 `kiikoread-4g-v3.0.0-release.zip` 相对仓库 v3.0.0 的源码差异。
完整使用、构建、刷写与限制见 [无名 4G 说明](../../docs/WUMING_4G.md)。

- `overlay/`：按原项目路径保存的 54 个新增或修改文件，保留原文件字节和许可标记。
- `manifest.json`：固定基线提交、来源压缩包 SHA-256、固件 SHA-256 与逐文件校验值。
- [源码清单](../../docs/WUMING_4G_SOURCE.md)：模块职责和完整文件列表。

还原完整源码：在仓库根目录运行 `python tools/materialize_wuming_4g.py --output build/wuming-4g/source`。
工具通过 Git 读取固定基线，再覆盖此目录；需要 Python 3.12+、Git 和包含基线提交的仓库历史。
根目录普通 S31/Pico 源码继续按原方式构建。不要直接把此 overlay 用作独立工程，也不要用普通 S31 构建命令代替还原步骤。
