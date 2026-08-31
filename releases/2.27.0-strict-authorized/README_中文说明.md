# TamaPoke v2.27.0 严格授权版

本文件夹是唯一的本地版本归档目录，所有文件均对应同一次 v2.27.0 编译。

## 刷写文件

- `TamaPoke-v2.27.0-merged.bin`：完整镜像，首次刷写或需要完整恢复时使用。
- `TamaPoke-v2.27.0-app.bin`：应用分区镜像，仅供已授权设备在线更新使用。
- `TamaPoke-v2.27.0-bootloader.bin`、`TamaPoke-v2.27.0-partitions.bin`：需要分区刷写工具时使用；通常优先使用 merged 镜像。

## 加密文件

- `tamapoke-2.27.0-app.bin.enc`：Cloudflare Worker 解密后用于设备 OTA。
- `tamapoke-2.27.0-merged.bin.enc`：Cloudflare Worker 解密后用于浏览器一键安装。

加密文件不能直接刷写。解密密钥只保存在 Cloudflare Secret `FIRMWARE_KEY`，不放入仓库。

## 安装入口

浏览器安装页面：<https://tamapoke-license.yuannihui001.workers.dev/>

必须输入作者许可码后才能生成 10 分钟有效的临时安装地址。设备 OTA 还会校验设备芯片 ID 与已绑定授权。

## 校验

所有镜像的文件大小和 SHA-256 值见 `SHA256_校验.txt`。

`old-public` 目录仅用于本机保存已从 GitHub 删除的旧公开文件，不应重新上传到公开目录。
