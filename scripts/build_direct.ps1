# Visual Studio 없이 g++로 Client.exe를 직접 빌드한다.
# 필요: MinGW-w64 g++ (x64). 예) winget install BrechtSanders.WinLibs.POSIX.UCRT
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

function Find-Gxx {
    $cmd = Get-Command g++ -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $roots = @(
        "$env:USERPROFILE\mingw64\bin",
        "C:\mingw64\bin",
        "C:\msys64\mingw64\bin",
        "$env:LOCALAPPDATA\Microsoft\WinGet\Packages",
        "C:\Program Files\WinLibs",
        "C:\WinLibs"
    )
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $hit = Get-ChildItem -Path $root -Filter "g++.exe" -Recurse -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

$gxx = Find-Gxx
if (-not $gxx) {
    Write-Error "g++ 를 찾지 못했습니다. MinGW-w64 (x64)를 설치한 뒤 PATH에 넣거나 다시 실행하세요.`n예: winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT"
}
$gcc = Join-Path (Split-Path $gxx) "gcc.exe"
$binDir = Split-Path $gxx
Write-Host "compiler: $gxx"

$libRoot = Join-Path $Root "third_party\libssh2-1.11.1"
$libSrc = Join-Path $libRoot "src"
$libInc = Join-Path $libRoot "include"
$clientSrc = Join-Path $Root "Client\src"
$objDir = Join-Path $Root "build\direct\obj"
$outDir = Join-Path $Root "dist"
New-Item -ItemType Directory -Force -Path $objDir, $outDir | Out-Null

$cFlags = @(
    "-O2", "-m64", "-Wall", "-Wno-unused-parameter", "-Wno-unused-function",
    "-DWIN32", "-DNDEBUG", "-D_WIN32_WINNT=0x0A00",
    "-DLIBSSH2_WINCNG", "-DLIBSSH2_ECDSA_WINCNG",
    "-I$libInc", "-I$libSrc"
)
$cppFlags = @(
    "-std=c++14", "-O2", "-m64", "-Wall", "-Wno-unknown-pragmas",
    "-DWIN32", "-DNDEBUG", "-D_WIN32_WINNT=0x0A00",
    "-I$libInc", "-I$clientSrc"
)

$cFiles = @(
    "agent.c", "bcrypt_pbkdf.c", "channel.c", "comp.c", "chacha.c",
    "cipher-chachapoly.c", "crypt.c", "crypto.c", "global.c", "hostkey.c",
    "keepalive.c", "kex.c", "knownhost.c", "mac.c", "misc.c", "packet.c",
    "pem.c", "poly1305.c", "publickey.c", "scp.c", "session.c", "sftp.c",
    "transport.c", "userauth.c", "userauth_kbd_packet.c", "version.c"
)
$cppFiles = @(
    "main.cpp", "Config.cpp", "Fingerprint.cpp", "SftpClient.cpp",
    "Util.cpp", "Logger.cpp", "Sha256.cpp"
)

$env:PATH = "$binDir;$env:PATH"
$objects = @()

foreach ($name in $cFiles) {
    $src = Join-Path $libSrc $name
    $obj = Join-Path $objDir ($name + ".o")
    Write-Host "cc  $name"
    & $gcc @cFlags -c $src -o $obj
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $objects += $obj
}

foreach ($name in $cppFiles) {
    $src = Join-Path $clientSrc $name
    $obj = Join-Path $objDir ($name + ".o")
    Write-Host "c++ $name"
    & $gxx @cppFlags -c $src -o $obj
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $objects += $obj
}

$exe = Join-Path $outDir "Client.exe"
Write-Host "link $exe"
& $gxx -m64 -mwindows -static -o $exe @objects -lws2_32 -lbcrypt -lcrypt32 -lshlwapi
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not (Test-Path (Join-Path $outDir "config.ini"))) {
    Copy-Item (Join-Path $outDir "config.ini.example") (Join-Path $outDir "config.ini")
}
Write-Host "OK $exe"

$presenterSrc = Join-Path $clientSrc "Presenter.cpp"
$clientUi = Join-Path $outDir "sft_cli.exe"
$serverUi = Join-Path $outDir "sft_ser.exe"
Write-Host "c++ Presenter.cpp"
Copy-Item (Join-Path $Root "Client\res\atec_logo.png") (Join-Path $outDir "atec_logo.png") -Force
& $gxx -std=c++14 -O2 -m64 -mwindows -static -finput-charset=UTF-8 -Wall -DUNICODE -D_UNICODE `
    -DWIN32 -D_WIN32_WINNT=0x0A00 $presenterSrc -o $clientUi -ladvapi32 -lshell32 -luser32 -lgdi32 -lgdiplus
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Copy-Item $clientUi $serverUi -Force
Write-Host "OK $clientUi"
Write-Host "OK $serverUi"
Get-Item $exe, $clientUi, $serverUi | Select-Object FullName, Length
