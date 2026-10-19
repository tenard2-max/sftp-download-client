@echo off
setlocal

rem sshd 서비스 시작은 관리자 권한이 필요하다.
net session >nul 2>&1
if errorlevel 1 (
  echo 관리자 권한으로 다시 실행합니다.
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b 0
)

if not exist "C:\OpenSSH\sshd.exe" (
  echo [ERROR] C:\OpenSSH\sshd.exe 가 없습니다.
  echo OpenSSH 서버가 설치되어 있는지 확인하세요.
  pause
  exit /b 1
)

sc query sshd | findstr /I "RUNNING" >nul
if not errorlevel 1 (
  echo sshd 가 이미 실행 중입니다.
  goto :show
)

echo sshd 서비스를 시작합니다.
net start sshd
if errorlevel 1 (
  echo [ERROR] sshd 시작에 실패했습니다.
  pause
  exit /b 1
)

:show
echo.
sc query sshd | findstr /I "SERVICE_NAME STATE"
echo.
echo 클라이언트 접속: 127.0.0.1:22
echo 종료: net stop sshd
echo.
pause
exit /b 0