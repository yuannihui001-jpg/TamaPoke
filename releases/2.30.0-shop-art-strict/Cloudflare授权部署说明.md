# v2.30.0 严格授权部署

Worker 源码位于仓库 `cloudflare/worker.js`，线上服务地址：

`https://tamapoke-license.yuannihui001.workers.dev`

## 部署顺序

1. 将 `02-在线服务加密镜像` 中的两个 `.enc` 文件复制到仓库 `web/firmware/`。
2. 在 Worker 中保留 KV 绑定名 `tomagochi`。
3. 在 Secrets 中保留 `FIRMWARE_KEY`，密钥只存在 Cloudflare 和本地私密文件中，不提交到 GitHub。
4. 部署 `cloudflare/worker.js`，确认 `/health` 返回 v2.30.0 后再使用安装页。

## 授权边界

浏览器安装令牌默认只存活 10 分钟；设备令牌默认存活 365 天并绑定设备 ID。设备首次授权后，后续 OTA 不再要求再次输入许可码，只要设备令牌仍有效且没有被撤销。未授权设备无法获取解密固件。

## 旧版本处理

线上仓库只保留当前版本加密镜像。明文 `.bin` 仅保留在本地发布目录，不能提交到公开仓库。
