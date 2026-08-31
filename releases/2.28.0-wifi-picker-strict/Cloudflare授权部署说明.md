# TamaPoke 严格授权服务

这个 Worker 负责许可证绑定设备、发放短期令牌、解密并返回固件。GitHub 中只保存加密固件，不能直接刷写。

## Cloudflare 设置

1. 在 Workers 和 Pages 创建 Worker，名称使用 tamapoke-license。
2. 将 worker.js 的内容粘贴到代码编辑器并部署。
3. 在 Worker 设置中添加 KV 绑定：当前线上变量名为 `tomagochi`，命名空间选择 TAMAPOKE_LICENSES；代码也兼容变量名 `LICENSES`。
4. 在 Worker 的 Secrets 中添加 FIRMWARE_KEY。它必须是 32 字节 AES 密钥的 base64url 文本，不能提交到 GitHub。
5. 将 tamapoke-2.28.0-merged.bin.enc 和 tamapoke-2.28.0-app.bin.enc 放到仓库 web/firmware/。

Worker 地址预计为：

https://tamapoke-license.yuannihui001.workers.dev

## 许可证记录

KV 中保存 license:许可码SHA256，值为：

    {"deviceId":"","revoked":false}

设备第一次发送 LICENSE 许可码 时，Worker 会把该许可码绑定到设备唯一 ID；之后其他设备不能复用。

## 安全边界

不要把 FIRMWARE_KEY、许可码明文或 Worker 管理令牌放进公开仓库。删除旧的明文 bin 前，应先确认 Worker 的 health、browser-manifest 和安装包下载返回成功；实际设备 OTA 仍需在已授权设备上验证。
