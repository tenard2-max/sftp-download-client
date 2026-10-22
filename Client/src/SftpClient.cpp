#include "msvc_compat.h"
#include "SftpClient.h"
#include "Fingerprint.h"
#include "Logger.h"
#include "Sha256.h"
#include "Util.h"

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <sstream>
#include <string>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ntdll.lib")

namespace {

// 1회 시도 결과: 성공 / 초기 접속 재시도 / 파일 전송 재시도 / 재시도 금지
enum AttemptResult {
    AttemptSuccess = 0,
    AttemptRetriable = 1,
    AttemptFatal = 2,
    AttemptTransfer = 3
};

const int kTransferRetryCount = 20;
const DWORD kTransferBudgetMs = 20UL * 60UL * 1000UL;

// ---------------------------------------------------------------------------
// 기능: GetTickCount 기반 경과 시간(ms) 측정기.
// 역할: 연결/다운로드 소요시간 로그.
// ---------------------------------------------------------------------------
struct ElapsedTimer {
    DWORD startTick;

    ElapsedTimer()
        : startTick(GetTickCount())
    {
    }

    // 기능: 생성 시점부터 지금까지의 경과 시간(ms)을 반환한다.
    // 역할: TCP/다운로드 소요시간을 로그에 남긴다.
    unsigned long ElapsedMs() const
    {
        return static_cast<unsigned long>(GetTickCount() - startTick);
    }
};

// ---------------------------------------------------------------------------
// 기능: 전체 경로에서 파일명만 추출한다.
// 역할: 다운로드 로그에 경로 대신 파일명을 짧게 남긴다.
// ---------------------------------------------------------------------------
std::string FileNameOnly(const std::string& path)
{
    const size_t pos = path.find_last_of("\\/");
    if (pos == std::string::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

// ---------------------------------------------------------------------------
// 기능: 로컬 파일 크기를 읽는다.
// 역할: 이미 받은 파일은 건너뛰고, 끊긴 파일은 그 크기부터 이어 받기 위해 사용한다.
// ---------------------------------------------------------------------------
bool LocalFileSize(const std::string& path, libssh2_uint64_t& size)
{
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info)) {
        size = 0;
        return false;
    }
    size = (static_cast<libssh2_uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    return true;
}

// ---------------------------------------------------------------------------
// 기능: 소켓을 안전하게 닫는다.
// 역할: 유효하지 않은 소켓은 건너뛰고, 세션 종료 시 TCP 연결을 해제한다.
// ---------------------------------------------------------------------------
void CloseSocket(libssh2_socket_t sock)
{
    if (sock != LIBSSH2_INVALID_SOCKET) {
        closesocket(sock);
    }
}

// ---------------------------------------------------------------------------
// 기능: 소켓 blocking/non-blocking 모드를 설정한다.
// 역할: Timeout 있는 connect를 위해 non-blocking 전환.
// ---------------------------------------------------------------------------
bool SetSocketBlocking(libssh2_socket_t sock, bool blocking)
{
    u_long mode = blocking ? 0 : 1;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
}

// ---------------------------------------------------------------------------
// 기능: 소켓 송수신 Timeout(ms)을 설정한다.
// 역할: 연결 이후 읽기/쓰기 지연 방지.
// ---------------------------------------------------------------------------
bool ApplySocketTimeouts(libssh2_socket_t sock, int timeoutSec)
{
    DWORD timeoutMs = static_cast<DWORD>(timeoutSec) * 1000UL;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeoutMs),
                   sizeof(timeoutMs)) != 0) {
        return false;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeoutMs),
                   sizeof(timeoutMs)) != 0) {
        return false;
    }
    return true;
}

// 연결 대기 결과. errorCode는 SO_ERROR 또는 WSA 오류 코드.
enum ConnectWaitKind {
    ConnectWaitOk = 0,
    ConnectWaitTimeout = 1,
    ConnectWaitFailed = 2
};

struct ConnectWaitResult {
    ConnectWaitKind kind;
    int errorCode;
};

// ---------------------------------------------------------------------------
// 기능: non-blocking connect 완료를 select로 대기한다.
// 역할: TCP 연결 Timeout(초)과 즉시 실패(도달 불가/거부)를 구분한다.
// ---------------------------------------------------------------------------
ConnectWaitResult WaitConnectComplete(libssh2_socket_t sock, int timeoutSec)
{
    ConnectWaitResult result;
    result.kind = ConnectWaitFailed;
    result.errorCode = 0;

    fd_set writeSet;
    fd_set exceptSet;
    FD_ZERO(&writeSet);
    FD_ZERO(&exceptSet);
    FD_SET(sock, &writeSet);
    FD_SET(sock, &exceptSet);

    timeval tv;
    tv.tv_sec = timeoutSec;
    tv.tv_usec = 0;

    // 쓰기/예외 집합으로 connect 완료 또는 실패를 기다린다.
    const int ready = select(0, NULL, &writeSet, &exceptSet, &tv);
    if (ready == 0) {
        result.kind = ConnectWaitTimeout;
        result.errorCode = WSAETIMEDOUT;
        return result;
    }
    if (ready < 0) {
        result.kind = ConnectWaitFailed;
        result.errorCode = WSAGetLastError();
        return result;
    }

    int soError = 0;
    int soLen = sizeof(soError);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&soError), &soLen) != 0) {
        result.kind = ConnectWaitFailed;
        result.errorCode = WSAGetLastError();
        return result;
    }
    if (soError != 0) {
        result.kind = ConnectWaitFailed;
        result.errorCode = soError;
        return result;
    }

    result.kind = ConnectWaitOk;
    result.errorCode = 0;
    return result;
}

// ---------------------------------------------------------------------------
// 기능: 서버 IP/포트로 TCP 연결한다 (Timeout 포함).
// 역할: SSH handshake 전 하위 소켓 확보.
// ---------------------------------------------------------------------------
libssh2_socket_t ConnectTcp(const std::string& host, int port, int timeoutSec)
{
    char portText[16];
    sprintf_s(portText, "%d", port);

    {
        std::ostringstream oss;
        oss << "host=" << host << " port=" << port
            << " timeout_sec=" << timeoutSec;
        LogInfo("TCP_CONNECT", oss.str());
    }

    ElapsedTimer timer;

    // DNS/주소 목록 해석
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = NULL;
    const int gai = getaddrinfo(host.c_str(), portText, &hints, &result);
    if (gai != 0 || result == NULL) {
        std::ostringstream oss;
        oss << "reason=dns_resolve_failed host=" << host
            << " port=" << port << " gai=" << gai
            << " elapsed_ms=" << timer.ElapsedMs();
        LogError("TCP_CONNECT", oss.str());
        return LIBSSH2_INVALID_SOCKET;
    }

    libssh2_socket_t sock = LIBSSH2_INVALID_SOCKET;
    bool timedOut = false;
    int lastWsa = 0;

    // 해석된 주소마다 연결 시도
    for (addrinfo* ptr = result; ptr != NULL; ptr = ptr->ai_next) {
        sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (sock == LIBSSH2_INVALID_SOCKET) {
            lastWsa = WSAGetLastError();
            continue;
        }

        // Timeout 적용을 위해 잠시 non-blocking
        if (!SetSocketBlocking(sock, false)) {
            lastWsa = WSAGetLastError();
            closesocket(sock);
            sock = LIBSSH2_INVALID_SOCKET;
            continue;
        }

        const int rc = connect(sock, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen));
        if (rc != 0) {
            const int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
                lastWsa = err;
                closesocket(sock);
                sock = LIBSSH2_INVALID_SOCKET;
                continue;
            }

            // connect 완료 대기. 타임아웃과 즉시 실패를 구분해 로그 사유를 남긴다.
            const ConnectWaitResult waited = WaitConnectComplete(sock, timeoutSec);
            if (waited.kind != ConnectWaitOk) {
                timedOut = (waited.kind == ConnectWaitTimeout);
                lastWsa = waited.errorCode;
                closesocket(sock);
                sock = LIBSSH2_INVALID_SOCKET;
                continue;
            }
        }

        // 이후 libssh2는 blocking 모드 사용
        if (!SetSocketBlocking(sock, true)) {
            lastWsa = WSAGetLastError();
            closesocket(sock);
            sock = LIBSSH2_INVALID_SOCKET;
            continue;
        }

        // 송수신 소켓 Timeout 적용
        if (!ApplySocketTimeouts(sock, timeoutSec)) {
            LogWarn("TCP_CONNECT", "reason=socket_timeout_setopt_failed continue=1");
        }

        break;
    }

    freeaddrinfo(result);

    if (sock == LIBSSH2_INVALID_SOCKET) {
        std::ostringstream oss;
        if (timedOut) {
            oss << "reason=tcp_connect_timeout timeout_sec=" << timeoutSec;
        } else {
            oss << "reason=tcp_connect_failed";
        }
        oss << " host=" << host << " port=" << port
            << " wsa=" << lastWsa
            << " elapsed_ms=" << timer.ElapsedMs();
        LogError("TCP_CONNECT", oss.str());
        return LIBSSH2_INVALID_SOCKET;
    }

    {
        std::ostringstream oss;
        oss << "reason=ok host=" << host << " port=" << port
            << " elapsed_ms=" << timer.ElapsedMs();
        LogOk("TCP_CONNECT", oss.str());
    }
    return sock;
}

// ---------------------------------------------------------------------------
// 기능: SSH password 인증을 수행한다.
// 역할: 서버 로그인 (ID/PW).
// ---------------------------------------------------------------------------
bool AuthenticatePassword(LIBSSH2_SESSION* session,
                          const std::string& user,
                          const std::string& password)
{
    LogInfo("AUTH_PASSWORD", std::string("user=") + user);

    // 서버가 허용하는 인증 방식 조회
    const char* userauthlist =
        libssh2_userauth_list(session, user.c_str(),
                              static_cast<unsigned int>(user.size()));
    if (userauthlist != NULL) {
        LogInfo("AUTH_METHODS", std::string("methods=") + userauthlist);
        if (strstr(userauthlist, "password") == NULL &&
            strstr(userauthlist, "keyboard-interactive") == NULL) {
            LogError("AUTH_PASSWORD",
                     std::string("reason=password_not_allowed methods=") + userauthlist);
            return false;
        }
    } else {
        LogWarn("AUTH_METHODS", "reason=auth_list_empty_try_password");
    }

    // password 인증 시도
    if (libssh2_userauth_password(session, user.c_str(), password.c_str()) != 0) {
        char* errmsg = NULL;
        int errlen = 0;
        const int err = libssh2_session_last_error(session, &errmsg, &errlen, 0);
        std::ostringstream oss;
        oss << "reason=login_failed libssh2_err=" << err;
        if (errmsg != NULL && errlen > 0) {
            oss << " detail=" << errmsg;
        }
        LogError("AUTH_PASSWORD", oss.str());
        return false;
    }

    LogOk("AUTH_PASSWORD", std::string("reason=ok user=") + user);
    return true;
}

// ---------------------------------------------------------------------------
// 기능: 원격 파일 1개를 로컬로 다운로드하고 size/시간/SHA256을 기록한다.
// 역할: SFTP 읽기 → 로컬 쓰기 → 검증 → 운영 로그.
// ---------------------------------------------------------------------------
bool DownloadOneFile(LIBSSH2_SFTP* sftp, const FileTransferItem& item)
{
    const std::string fileName = FileNameOnly(item.localPath);
    ElapsedTimer timer;

    {
        std::ostringstream oss;
        oss << "file=" << fileName
            << " remote=" << item.remotePath
            << " local=" << item.localPath;
        LogInfo("DOWNLOAD_START", oss.str());
    }

    // 원격 파일 존재/속성 확인
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    memset(&attrs, 0, sizeof(attrs));
    if (libssh2_sftp_stat(sftp, item.remotePath.c_str(), &attrs) != 0) {
        const unsigned long sftpErr = libssh2_sftp_last_error(sftp);
        std::ostringstream oss;
        oss << "reason=remote_stat_failed file=" << fileName
            << " remote=" << item.remotePath
            << " sftp_err=" << sftpErr
            << " elapsed_ms=" << timer.ElapsedMs();
        LogError("REMOTE_STAT", oss.str());
        return false;
    }

    if ((attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) != 0 &&
        !LIBSSH2_SFTP_S_ISREG(attrs.permissions)) {
        std::ostringstream oss;
        oss << "reason=not_regular_file file=" << fileName
            << " remote=" << item.remotePath
            << " permissions=" << attrs.permissions
            << " elapsed_ms=" << timer.ElapsedMs();
        LogError("REMOTE_STAT", oss.str());
        return false;
    }

    {
        std::ostringstream oss;
        oss << "file=" << fileName << " remote=" << item.remotePath;
        if (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) {
            oss << " remote_size=" << static_cast<unsigned long long>(attrs.filesize);
        }
        LogInfo("REMOTE_STAT", oss.str());
    }

    const bool knownSize = (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) != 0 && attrs.filesize > 0;
    libssh2_uint64_t localSize = 0;
    bool localExists = LocalFileSize(item.localPath, localSize);
    if (knownSize && localExists && localSize == attrs.filesize) {
        std::ostringstream oss;
        oss << "reason=already_complete file=" << fileName
            << " remote=" << item.remotePath
            << " local=" << item.localPath
            << " size=" << static_cast<unsigned long long>(localSize);
        LogOk("DOWNLOAD", oss.str());
        return true;
    }
    if (knownSize && localExists && localSize > attrs.filesize) {
        DeleteFileA(item.localPath.c_str());
        localExists = false;
        localSize = 0;
        LogWarn("DOWNLOAD_RESUME", std::string("reason=local_larger_restart file=") + fileName);
    }

    // 로컬 부모 폴더 생성
    if (!EnsureParentDirectory(item.localPath)) {
        std::ostringstream oss;
        oss << "reason=local_parent_dir_create_failed file=" << fileName
            << " local=" << item.localPath
            << " elapsed_ms=" << timer.ElapsedMs();
        LogError("LOCAL_CREATE", oss.str());
        return false;
    }

    // 원격 파일 오픈
    LIBSSH2_SFTP_HANDLE* handle =
        libssh2_sftp_open(sftp, item.remotePath.c_str(), LIBSSH2_FXF_READ, 0);
    if (handle == NULL) {
        const unsigned long sftpErr = libssh2_sftp_last_error(sftp);
        std::ostringstream oss;
        if (sftpErr == LIBSSH2_FX_NO_SUCH_FILE) {
            oss << "reason=remote_file_not_found";
        } else {
            oss << "reason=remote_open_failed";
        }
        oss << " file=" << fileName
            << " remote=" << item.remotePath
            << " sftp_err=" << sftpErr
            << " elapsed_ms=" << timer.ElapsedMs();
        LogError("REMOTE_OPEN", oss.str());
        return false;
    }
    LogInfo("REMOTE_OPEN", std::string("reason=ok file=") + fileName);

    libssh2_uint64_t offset = 0;
    const char* openMode = "wb";
    if (knownSize && localExists && localSize > 0 && localSize < attrs.filesize) {
        offset = localSize;
        openMode = "ab";
        libssh2_sftp_seek64(handle, offset);
        std::ostringstream oss;
        oss << "file=" << fileName
            << " offset=" << static_cast<unsigned long long>(offset)
            << " total=" << static_cast<unsigned long long>(attrs.filesize);
        LogInfo("DOWNLOAD_RESUME", oss.str());
    }

    // 로컬 파일 생성. 이어 받기는 기존 내용 뒤에 붙인다.
    FILE* localFile = NULL;
    if (fopen_s(&localFile, item.localPath.c_str(), openMode) != 0 || localFile == NULL) {
        std::ostringstream oss;
        oss << "reason=local_file_open_failed file=" << fileName
            << " local=" << item.localPath
            << " elapsed_ms=" << timer.ElapsedMs();
        LogError("LOCAL_CREATE", oss.str());
        libssh2_sftp_close(handle);
        return false;
    }
    LogInfo("LOCAL_CREATE", std::string("reason=ok local=") + item.localPath);

    // 청크 단위 다운로드
    char buffer[16 * 1024];
    bool ok = true;
    libssh2_uint64_t total = offset;
    const char* failReason = NULL;
    int lastPercent = -1;

    while (true) {
        const ssize_t n = libssh2_sftp_read(handle, buffer, sizeof(buffer));
        if (n > 0) {
            const size_t written = fwrite(buffer, 1, static_cast<size_t>(n), localFile);
            if (written != static_cast<size_t>(n)) {
                failReason = "local_write_failed";
                ok = false;
                break;
            }
            total += static_cast<libssh2_uint64_t>(n);
            if (knownSize) {
                int percent = static_cast<int>((total * 100) / attrs.filesize);
                if (percent > 99) {
                    percent = 99;
                }
                if (lastPercent < 0 || percent >= lastPercent + 5) {
                    std::ostringstream progress;
                    progress << "file=" << fileName
                             << " percent=" << percent
                             << " bytes=" << static_cast<unsigned long long>(total)
                             << " total=" << static_cast<unsigned long long>(attrs.filesize);
                    LogInfo("DOWNLOAD_PROGRESS", progress.str());
                    lastPercent = percent;
                }
            }
            continue;
        }

        if (n == 0) {
            break; // EOF
        }

        failReason = "remote_read_failed";
        ok = false;
        {
            std::ostringstream oss;
            oss << "reason=remote_read_failed file=" << fileName
                << " remote=" << item.remotePath
                << " code=" << n
                << " bytes_so_far=" << static_cast<unsigned long long>(total)
                << " elapsed_ms=" << timer.ElapsedMs();
            LogError("DOWNLOAD_READ", oss.str());
        }
        break;
    }

    fclose(localFile);
    libssh2_sftp_close(handle);

    if (!ok) {
        // 받아 둔 내용은 지우지 않는다. 다음 시도가 그 위치부터 이어 받는다.
        if (failReason != NULL && strcmp(failReason, "remote_read_failed") != 0) {
            std::ostringstream oss;
            oss << "reason=" << failReason
                << " file=" << fileName
                << " local=" << item.localPath
                << " bytes_so_far=" << static_cast<unsigned long long>(total)
                << " elapsed_ms=" << timer.ElapsedMs();
            LogError("DOWNLOAD_WRITE", oss.str());
        }
        return false;
    }

    // 원격 크기와 로컬 수신 크기 비교
    if ((attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) &&
        attrs.filesize > 0 &&
        total != attrs.filesize) {
        std::ostringstream oss;
        oss << "reason=size_mismatch file=" << fileName
            << " remote_size=" << static_cast<unsigned long long>(attrs.filesize)
            << " local_size=" << static_cast<unsigned long long>(total)
            << " elapsed_ms=" << timer.ElapsedMs();
        LogError("DOWNLOAD_VERIFY", oss.str());
        if (total > attrs.filesize) {
            DeleteFileA(item.localPath.c_str());
        }
        return false;
    }

    // 운영 로그용 SHA256 계산
    const unsigned long elapsedMs = timer.ElapsedMs();
    const std::string sha256 = Sha256FileHex(item.localPath);
    if (sha256.empty()) {
        std::ostringstream oss;
        oss << "reason=sha256_calc_failed file=" << fileName
            << " local=" << item.localPath
            << " size=" << static_cast<unsigned long long>(total)
            << " elapsed_ms=" << elapsedMs;
        LogError("DOWNLOAD_VERIFY", oss.str());
        return false;
    }

    {
        std::ostringstream oss;
        oss << "file=" << fileName
            << " remote=" << item.remotePath
            << " local=" << item.localPath
            << " size=" << static_cast<unsigned long long>(total)
            << " elapsed_ms=" << elapsedMs
            << " sha256=" << sha256;
        LogOk("DOWNLOAD", oss.str());
    }
    return true;
}

// ---------------------------------------------------------------------------
// 기능: SFTP/SSH/소켓 리소스를 순서대로 해제한다.
// 역할: 시도 종료 시 누수 방지 및 정상 disconnect.
// ---------------------------------------------------------------------------
void CleanupSession(LIBSSH2_SFTP*& sftp,
                    LIBSSH2_SESSION*& session,
                    libssh2_socket_t& sock)
{
    if (sftp != NULL) {
        libssh2_sftp_shutdown(sftp);
        sftp = NULL;
        LogInfo("SFTP_SHUTDOWN", "reason=ok");
    }
    if (session != NULL) {
        libssh2_session_disconnect(session, "Normal Shutdown");
        libssh2_session_free(session);
        session = NULL;
        LogInfo("SSH_DISCONNECT", "reason=ok");
    }
    CloseSocket(sock);
    sock = LIBSSH2_INVALID_SOCKET;
}

// ---------------------------------------------------------------------------
// 기능: 연결~다운로드까지 1회 전체 시도를 수행한다.
// 역할: RunSftpDownload의 재시도 루프에서 호출되는 단위 작업.
// 반환: Success / 초기 접속 재시도 / 파일 전송 재시도 / Fatal(HostKey·로그인 실패)
// ---------------------------------------------------------------------------
AttemptResult AttemptDownload(const AppConfig& config)
{
    libssh2_socket_t sock = LIBSSH2_INVALID_SOCKET;
    LIBSSH2_SESSION* session = NULL;
    LIBSSH2_SFTP* sftp = NULL;
    AttemptResult result = AttemptRetriable;
    ElapsedTimer attemptTimer;

    LogInfo("ATTEMPT_START", "phase=begin");

    // 1) TCP 연결
    sock = ConnectTcp(config.host, config.port, config.timeoutSec);
    if (sock == LIBSSH2_INVALID_SOCKET) {
        return AttemptRetriable;
    }

    // 2) SSH 세션 생성
    LogInfo("SSH_SESSION_INIT", "phase=create");
    session = libssh2_session_init();
    if (session == NULL) {
        LogError("SSH_SESSION_INIT", "reason=session_create_failed");
        CleanupSession(sftp, session, sock);
        return AttemptRetriable;
    }
    LogOk("SSH_SESSION_INIT", "reason=ok");

    // blocking + 세션 Timeout(ms)
    libssh2_session_set_blocking(session, 1);
    libssh2_session_set_timeout(session, config.timeoutSec * 1000);
    {
        std::ostringstream oss;
        oss << "timeout_sec=" << config.timeoutSec;
        LogInfo("SSH_TIMEOUT", oss.str());
    }

    // 3) SSH handshake
    LogInfo("SSH_HANDSHAKE", "phase=start");
    if (libssh2_session_handshake(session, sock) != 0) {
        char* errmsg = NULL;
        int errlen = 0;
        const int err = libssh2_session_last_error(session, &errmsg, &errlen, 0);
        std::ostringstream oss;
        oss << "reason=handshake_failed libssh2_err=" << err;
        if (errmsg != NULL && errlen > 0) {
            oss << " detail=" << errmsg;
        }
        LogError("SSH_HANDSHAKE", oss.str());
        CleanupSession(sftp, session, sock);
        return AttemptRetriable;
    }
    LogOk("SSH_HANDSHAKE", "reason=ok");

    // 4) Host Key fingerprint 검증 (불일치=Fatal, 재시도 안 함)
    LogInfo("HOSTKEY_VERIFY", "phase=start");
    {
        const std::string actual = BuildSha256Fingerprint(session);
        if (actual.empty()) {
            LogError("HOSTKEY_VERIFY", "reason=fingerprint_calc_failed");
            CleanupSession(sftp, session, sock);
            return AttemptFatal;
        }

        {
            std::ostringstream oss;
            oss << "actual=" << actual
                << " expected=" << config.hostFingerprint;
            LogInfo("HOSTKEY_FINGERPRINT", oss.str());
        }

        if (!FingerprintsEqual(actual, config.hostFingerprint)) {
            std::ostringstream oss;
            oss << "reason=fingerprint_mismatch expected="
                << config.hostFingerprint << " actual=" << actual;
            LogError("HOSTKEY_VERIFY", oss.str());
            CleanupSession(sftp, session, sock);
            return AttemptFatal;
        }

        LogOk("HOSTKEY_VERIFY", "reason=ok");
    }

    // 5) ID/PW 로그인. 계정/비밀번호 실패는 같은 값으로 다시 시도하지 않고 바로 종료한다.
    if (!AuthenticatePassword(session, config.user, config.password)) {
        CleanupSession(sftp, session, sock);
        return AttemptFatal;
    }

    // 6) SFTP 세션 시작
    LogInfo("SFTP_INIT", "phase=start");
    sftp = libssh2_sftp_init(session);
    if (sftp == NULL) {
        LogError("SFTP_INIT", "reason=sftp_session_failed");
        CleanupSession(sftp, session, sock);
        return AttemptRetriable;
    }
    LogOk("SFTP_INIT", "reason=ok");

    // 7) 설정된 파일들 순차 다운로드
    bool allOk = true;
    for (size_t i = 0; i < config.files.size(); ++i) {
        std::ostringstream oss;
        oss << "index=" << (i + 1) << "/" << config.files.size()
            << " remote=" << config.files[i].remotePath;
        LogInfo("DOWNLOAD_ITEM", oss.str());

        if (!DownloadOneFile(sftp, config.files[i])) {
            allOk = false;
            break;
        }
    }

    if (allOk) {
        std::ostringstream oss;
        oss << "reason=ok file_count=" << config.files.size()
            << " elapsed_ms=" << attemptTimer.ElapsedMs();
        LogOk("ATTEMPT_RESULT", oss.str());
        result = AttemptSuccess;
    } else {
        std::ostringstream oss;
        oss << "reason=download_failed elapsed_ms=" << attemptTimer.ElapsedMs();
        LogError("ATTEMPT_RESULT", oss.str());
        result = AttemptTransfer;
    }

    // 8) 세션/소켓 정리
    CleanupSession(sftp, session, sock);
    return result;
}

} // namespace

// ---------------------------------------------------------------------------
// 기능: SFTP 다운로드 전체 워크플로(초기화/재시도/종료)를 관리한다.
// 역할: main()에서 호출하는 엔진 진입점.
// ---------------------------------------------------------------------------
int RunSftpDownload(const AppConfig& config)
{
    // Fingerprint 미등록이면 네트워크 접속 자체를 금지
    if (!IsFingerprintConfigured(config.hostFingerprint)) {
        LogError("HOSTKEY_VERIFY", "reason=fingerprint_not_configured action=deny_connect");
        return 1;
    }

    // Winsock 기동
    WSADATA wsadata;
    const int wsa = WSAStartup(MAKEWORD(2, 2), &wsadata);
    if (wsa != 0) {
        std::ostringstream oss;
        oss << "reason=wsastartup_failed wsa=" << wsa;
        LogError("WSA_STARTUP", oss.str());
        return 1;
    }
    LogOk("WSA_STARTUP", "reason=ok");

    // libssh2 전역 초기화
    if (libssh2_init(0) != 0) {
        LogError("LIBSSH2_INIT", "reason=libssh2_init_failed");
        WSACleanup();
        return 1;
    }
    LogOk("LIBSSH2_INIT", "reason=ok");

    {
        std::ostringstream oss;
        oss << "timeout_sec=" << config.timeoutSec
            << " retry_count=" << config.retryCount
            << " retry_interval_sec=" << config.retryIntervalSec
            << " version=" << config.version
            << " server_ip=" << config.host
            << " server_port=" << config.port
            << " server_directory=" << config.serverDirectory
            << " file_count=" << config.files.size();
        LogInfo("RUN_PARAM", oss.str());
    }

    int exitCode = 1;
    bool transferPhase = false;
    int connectAttempt = 0;
    int transferAttempt = 0;
    DWORD transferOrigin = 0;

    // 초기 접속 실패는 설정 횟수(기본 5회)까지, 파일 전송 실패는 20회와 20분 안에서 다시 시도한다.
    for (;;) {
        const DWORD attemptStart = GetTickCount();
        if (transferPhase) {
            const DWORD elapsed = attemptStart - transferOrigin;
            if (transferAttempt >= kTransferRetryCount) {
                std::ostringstream oss;
                oss << "reason=max_retry_exceeded stage=transfer retry_count=" << kTransferRetryCount;
                LogError("RUN_RESULT", oss.str());
                break;
            }
            if (elapsed >= kTransferBudgetMs) {
                LogError("RUN_RESULT", "reason=transfer_time_exceeded budget_min=20 stage=transfer");
                break;
            }
            ++transferAttempt;
            std::ostringstream oss;
            oss << "attempt=" << transferAttempt << "/" << kTransferRetryCount << " stage=transfer";
            LogInfo("RETRY", oss.str());
        } else {
            if (connectAttempt >= config.retryCount) {
                std::ostringstream oss;
                oss << "reason=max_retry_exceeded stage=connect retry_count=" << config.retryCount;
                LogError("RUN_RESULT", oss.str());
                break;
            }
            ++connectAttempt;
            std::ostringstream oss;
            oss << "attempt=" << connectAttempt << "/" << config.retryCount << " stage=connect";
            LogInfo("RETRY", oss.str());
        }

        const AttemptResult result = AttemptDownload(config);
        if (result == AttemptSuccess) {
            LogOk("RUN_RESULT", "reason=success");
            exitCode = 0;
            break;
        }
        if (result == AttemptFatal) {
            LogError("RUN_RESULT", "reason=fatal_no_retry");
            exitCode = 1;
            break;
        }

        if (result == AttemptTransfer && !transferPhase) {
            transferPhase = true;
            transferOrigin = attemptStart;
            transferAttempt = 1;
        }

        const char* stage = transferPhase ? "transfer" : "connect";
        const int limit = transferPhase ? kTransferRetryCount : config.retryCount;
        const int used = transferPhase ? transferAttempt : connectAttempt;
        if (transferPhase) {
            const DWORD elapsed = GetTickCount() - transferOrigin;
            if (used >= limit || elapsed >= kTransferBudgetMs) {
                std::ostringstream oss;
                if (elapsed >= kTransferBudgetMs) {
                    oss << "reason=transfer_time_exceeded budget_min=20 stage=transfer";
                } else {
                    oss << "reason=max_retry_exceeded stage=transfer retry_count=" << limit;
                }
                LogError("RUN_RESULT", oss.str());
                break;
            }
        } else if (used >= limit) {
            std::ostringstream oss;
            oss << "reason=max_retry_exceeded stage=connect retry_count=" << limit;
            LogError("RUN_RESULT", oss.str());
            break;
        }

        std::ostringstream oss;
        oss << "reason=retriable_failure next_attempt=" << (used + 1)
            << " stage=" << stage
            << " sleep_sec=" << config.retryIntervalSec;
        LogWarn("RETRY", oss.str());
        if (config.retryIntervalSec > 0) {
            Sleep(static_cast<DWORD>(config.retryIntervalSec) * 1000UL);
        }
    }

    // 전역 자원 해제
    libssh2_exit();
    WSACleanup();
    LogInfo("CLEANUP", "reason=process_exit");
    return exitCode;
}
