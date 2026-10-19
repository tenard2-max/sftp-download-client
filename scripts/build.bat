@echo off
setlocal
cd /d "%~dp0.."

set CONFIG=Release
if /I "%~1"=="Debug" set CONFIG=Debug

where msbuild >nul 2>&1
if errorlevel 1 (
  echo [ERROR] msbuild 를 PATH 에서 찾지 못했습니다.
  echo Visual Studio 2015 Developer Command Prompt 에서 실행하거나,
  echo "C:\Program Files (x86)\MSBuild\14.0\Bin\MSBuild.exe" 경로를 확인하세요.
  exit /b 1
)

echo Building SftpClient.sln (%CONFIG%^|x64^) ...
msbuild "SftpClient.sln" /m /p:Configuration=%CONFIG% /p:Platform=x64
if errorlevel 1 exit /b 1

if not exist "dist" mkdir dist
copy /Y "build\%CONFIG%\Client.exe" "dist\Client.exe" >nul
if not exist "dist\config.ini" (
  copy /Y "dist\config.ini.example" "dist\config.ini" >nul
)
if not exist "dist\LICENSE_libssh2.txt" (
  copy /Y "third_party\libssh2-1.11.1\COPYING" "dist\LICENSE_libssh2.txt" >nul
)

echo.
echo Build OK. Deploy folder:
echo   dist\Client.exe
echo   dist\config.ini
echo   dist\LICENSE_libssh2.txt
echo.
echo Host fingerprint 확인 예 (Linux 서버):
echo   ssh-keygen -lf /etc/ssh/ssh_host_ecdsa_key.pub -E sha256
echo   ssh-keygen -lf /etc/ssh/ssh_host_rsa_key.pub -E sha256
exit /b 0
