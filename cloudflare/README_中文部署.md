# TamaPoke 严格授权服务

这个 Worker 负责浏览器安装许可、官方固件设备登记、发放设备令牌、解密并返回固件。GitHub 中只保存加密固件，不能直接刷写。

## Cloudflare 设置

1. 在 Workers 和 Pages 创建 Worker，名称使用 tamapoke-license。
2. 将 worker.js 的内容粘贴到代码编辑器并部署。
3. 在 Worker 设置中添加 KV 绑定：当前线上变量名为 `tomagochi`，命名空间选择 TAMAPOKE_LICENSES；代码也兼容变量名 `LICENSES`。
4. 在 Worker 的 Secrets 中添加 FIRMWARE_KEY。它必须是 32 字节 AES 密钥的 base64url 文本，不能提交到 GitHub。
5. 将 tamapoke-2.32.0-merged.bin.enc 和 tamapoke-2.32.0-app.bin.enc 放到仓库 web/firmware/。

Worker 地址预计为：

https://tamapoke-license.yuannihui001.workers.dev

## 许可证记录

KV 中保存 license:许可码SHA256，值为：

    {"deviceId":"","revoked":false}

设备刷入官方固件后，首次在线更新会自动登记设备并保存令牌，不再重复输入许可码。浏览器首次安装到新设备仍需作者许可码；Worker 会把该许可码绑定到设备唯一 ID，之后其他设备不能复用。

## 安全边界

不要把 FIRMWARE_KEY、许可码明文或 Worker 管理令牌放进公开仓库。设备登记依赖官方固件版本和发布标识，适用于本项目官方刷写设备；发布标识嵌在固件中，不应宣传为不可提取的硬件证明。删除旧的明文 bin 前，应先确认 Worker 的 health、browser-manifest 和安装包下载返回成功；实际设备 OTA 仍需在已授权设备上验证。
