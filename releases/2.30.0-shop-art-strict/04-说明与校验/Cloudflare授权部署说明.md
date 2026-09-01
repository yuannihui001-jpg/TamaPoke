# v2.30.0 严格授权部署

线上服务：`https://tamapoke-license.yuannihui001.workers.dev`

- GitHub `web/firmware/` 只放置 v2.30.0 的两个 AES-GCM 加密镜像。
- Worker 保留 `tomagochi` KV 绑定和现有 `FIRMWARE_KEY` Secret。
- `/health` 应返回 v2.30.0，未授权的 `/v1/install` 和 `/v1/firmware` 请求应返回 403。
- 明文固件只保留在本地发布目录。
