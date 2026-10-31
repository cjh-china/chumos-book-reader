@echo off
if "%1"=="stop" goto stop
tasklist /FI "IMAGENAME eq node.exe" 2>NUL | find /I "node.exe" >NUL
if not errorlevel 1 (
  echo Node 进程已在运行，端口 3000 可能已被占用
  echo 如需重启，请先运行: %~nx0 stop
  exit /b 1
)
start "" /B NeteaseCloudMusicApi
timeout /t 3 /nobreak >NUL
echo 音乐 API 服务已在后台启动 (http://127.0.0.1:3000)
exit /b 0
:stop
for /f "tokens=2" %%i in ('tasklist /FI "IMAGENAME eq node.exe" /FO CSV ^| find /I "node.exe"') do taskkill /PID %%i /F >NUL 2>&1
echo 已停止 Node 进程
exit /b 0