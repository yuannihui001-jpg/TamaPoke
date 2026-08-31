@echo off
setlocal
cd /d "%~dp0"

where py >nul 2>nul
if not errorlevel 1 goto :python_launcher

where python >nul 2>nul
if not errorlevel 1 goto :python_launcher_python

echo 未找到 Python。请先安装 Python 3，或使用已部署的 HTTPS 网页链接。
goto :powershell_server

:python_launcher
start "TamaPoke installer server" /b py -m http.server 8000 --directory "%~dp0"
goto :open_browser

:python_launcher_python
start "TamaPoke installer server" /b python -m http.server 8000 --directory "%~dp0"
goto :open_browser

:powershell_server
start "TamaPoke installer server" /b powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0serve_installer.ps1" -Root "%~dp0" -Port 8000

:open_browser
timeout /t 1 /nobreak >nul
start "" "http://localhost:8000/"
echo 安装页面已打开： http://localhost:8000/
echo 关闭此窗口不会停止后台服务；如需停止，请在任务管理器中结束 Python。
pause
