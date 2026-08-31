# TamaPoke 中文固件安装

这个文件夹已经包含 Chrome 浏览器安装页面和 ESP32-S3 的固件清单。

## 最简单的使用方法

1. 在 Windows 上双击 `start_installer.bat`。
2. 页面打开后，使用电脑端 Chrome 或 Edge，连接微雪 ESP32-S3-Touch-AMOLED-1.75 的 USB-C 数据线。
3. 点击“选择串口并安装中文固件”，在弹窗中选择开发板串口并等待刷写完成。
4. 第一次安装可以选择擦除设备；升级时选择不擦除可保留宠物存档。
5. 插入 microSD 后，按页面第 2 步连接开发板并加载精灵资源。

## 注意事项

- 不能直接双击 `index.html`，浏览器会禁止本地文件访问 Web Serial；必须使用 `start_installer.bat`，或将 `web` 文件夹部署到 HTTPS 网站。
- 需要电脑端 Chrome/Edge，手机浏览器不支持这个串口安装流程。
- 使用支持数据传输的 USB-C 线。若看不到串口，请按住开发板 BOOT 键再插 USB，松开后重试。
- 刷写期间不要拔线或关闭页面。若浏览器提示选择擦除，只有首次安装或需要重置时才选择擦除。

## 发布到 GitHub Pages

将整个 `web` 文件夹发布到 GitHub Pages 后，可直接打开对应的 HTTPS 地址安装。每次更新固件时，需要同时更新 `firmware/tamapoke.bin` 和 `manifest.json`。
