# TamaPoke 在线更新通道

设备设置页的“更新”按钮使用 GitHub 原始文件上的应用分区镜像：

`https://raw.githubusercontent.com/yuannihui001-jpg/TamaPoke/main/web/firmware/tamapoke-app.bin`

每次发布新固件时：

1. 使用 Arduino CLI 生成 `TamaPoke.ino.bin`（应用分区镜像）。
2. 将它重命名为 `web/firmware/tamapoke-app.bin` 并发布到 GitHub Pages。
3. 同时更新 `web/manifest.json` 的版本号，供 USB 一键安装页显示。
4. 完整刷写镜像仍为 `web/firmware/tamapoke.bin`，不要用它替代 OTA 应用镜像。

Wi-Fi 凭据通过 USB 串口保存：

`WIFI <SSID> <PASSWORD>`

设备下滑设置页点击“连接 WiFi”后，点击“更新”即可下载 OTA 应用镜像。更新过程不要断电。
