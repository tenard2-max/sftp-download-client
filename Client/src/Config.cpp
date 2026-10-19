#include "msvc_compat.h"
#include "Config.h"
#include "Util.h"
#include "Logger.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

const size_t kMaxFiles = 20;

// ---------------------------------------------------------------------------
// 기능: "Key=Value" 한 줄을 분리한다.
// 역할: config.ini 라인 파싱.
// ---------------------------------------------------------------------------
bool ParseKeyValue(const std::string& line, std::string& key, std::string& value)
{
    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
        return false;
    }

    // 좌측=키, 우측=값 (Trim 적용)
    key = Trim(line.substr(0, eq));
    value = Trim(line.substr(eq + 1));
    return !key.empty();
}

// ---------------------------------------------------------------------------
// 기능: RemoteFileN / LocalFileN 형태 키인지 판별하고 N을 추출한다.
// 역할: 1~20개 파일 쌍 설정 파싱.
// ---------------------------------------------------------------------------
bool LooksLikeIndexedKey(const std::string& key, const char* prefix, int* indexOut)
{
    const size_t prefixLen = strlen(prefix);
    if (key.size() <= prefixLen) {
        return false;
    }
    if (_strnicmp(key.c_str(), prefix, static_cast<unsigned int>(prefixLen)) != 0) {
        return false;
    }

    const char* numberPart = key.c_str() + prefixLen;
    if (*numberPart == '\0') {
        return false;
    }

    for (const char* p = numberPart; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }

    const int index = atoi(numberPart);
    if (index < 1 || index > static_cast<int>(kMaxFiles)) {
        return false;
    }

    *indexOut = index;
    return true;
}

// ---------------------------------------------------------------------------
// 기능: 경로가 '/' 또는 '\' 로 시작하는지 확인한다.
// 역할: ServerDirectory·RemoteFile이 절대경로인지 판별한다.
// ---------------------------------------------------------------------------
bool StartsWithSlash(const std::string& path)
{
    return !path.empty() && (path[0] == '/' || path[0] == '\\');
}

// ---------------------------------------------------------------------------
// 기능: 경로 끝의 슬래시를 제거한다.
// 역할: ServerDirectory와 파일명을 붙일 때 슬래시가 두 번 생기지 않게 한다.
// ---------------------------------------------------------------------------
std::string TrimTrailingSlashes(std::string path)
{
    while (path.size() > 1 &&
           (path[path.size() - 1] == '/' || path[path.size() - 1] == '\\')) {
        path.erase(path.size() - 1);
    }
    return path;
}

// ---------------------------------------------------------------------------
// 기능: 경로 앞의 슬래시를 제거한다.
// 역할: 상대 RemoteFile을 ServerDirectory 뒤에 붙이기 전에 선행 구분자를 뺀다.
// ---------------------------------------------------------------------------
std::string TrimLeadingSlashes(std::string path)
{
    while (!path.empty() && (path[0] == '/' || path[0] == '\\')) {
        path.erase(0, 1);
    }
    return path;
}

// ---------------------------------------------------------------------------
// 기능: ServerDirectory와 RemoteFile을 합쳐 최종 원격 경로를 만든다.
// 역할: 상대경로면 ServerDirectory 아래, 절대경로면 그대로 사용.
// ---------------------------------------------------------------------------
std::string BuildRemotePath(const std::string& serverDirectory,
                            const std::string& remoteFile)
{
    if (remoteFile.empty()) {
        return remoteFile;
    }

    // 절대경로면 ServerDirectory와 결합하지 않음
    if (StartsWithSlash(remoteFile)) {
        return remoteFile;
    }

    if (serverDirectory.empty()) {
        return remoteFile;
    }

    const std::string base = TrimTrailingSlashes(serverDirectory);
    const std::string leaf = TrimLeadingSlashes(remoteFile);
    if (base.empty()) {
        return std::string("/") + leaf;
    }
    if (leaf.empty()) {
        return base;
    }
    return base + "/" + leaf;
}

} // namespace

// ---------------------------------------------------------------------------
// 기능: config.ini를 읽어 AppConfig를 구성한다.
// 역할: Version/ServerIP/Port/Directory/인증/파일목록/Timeout/Retry 검증.
// ---------------------------------------------------------------------------
bool LoadConfig(const char* absolutePath, AppConfig& outConfig)
{
    outConfig = AppConfig();

    // config.ini 경로 결정 (미지정 시 exe 옆)
    std::string path;
    if (absolutePath != NULL && absolutePath[0] != '\0') {
        path = absolutePath;
    } else {
        path = JoinPath(GetExecutableDirectory(), "config.ini");
    }

    // 설정 파일 오픈
    std::ifstream input(path.c_str());
    if (!input) {
        std::ostringstream oss;
        oss << "reason=open_failed path=" << path;
        LogError("CONFIG_LOAD", oss.str());
        return false;
    }

    std::string remote[kMaxFiles + 1];
    std::string local[kMaxFiles + 1];
    bool used[kMaxFiles + 1] = {false};
    bool hasServerIp = false;
    bool hasServerPort = false;

    // 라인 단위 Key=Value 파싱
    std::string line;
    int lineNo = 0;
    while (std::getline(input, line)) {
        ++lineNo;
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }

        std::string key;
        std::string value;
        if (!ParseKeyValue(line, key, value)) {
            std::ostringstream oss;
            oss << "reason=bad_format line=" << lineNo << " text=" << line;
            LogError("CONFIG_LOAD", oss.str());
            return false;
        }

        if (_stricmp(key.c_str(), "Version") == 0) {
            outConfig.version = value;
        } else if (_stricmp(key.c_str(), "ServerIP") == 0 ||
                   _stricmp(key.c_str(), "ServerIp") == 0) {
            outConfig.host = value;
            hasServerIp = true;
        } else if (_stricmp(key.c_str(), "ServerPort") == 0) {
            outConfig.port = atoi(value.c_str());
            hasServerPort = true;
        } else if (_stricmp(key.c_str(), "ServerDirectory") == 0 ||
                   _stricmp(key.c_str(), "ServerDir") == 0) {
            outConfig.serverDirectory = value;
        } else if (_stricmp(key.c_str(), "Host") == 0) {
            // 구버전 호환: ServerIP가 없을 때만 Host 사용
            if (!hasServerIp) {
                outConfig.host = value;
            }
        } else if (_stricmp(key.c_str(), "Port") == 0) {
            // 구버전 호환: ServerPort가 없을 때만 Port 사용
            if (!hasServerPort) {
                outConfig.port = atoi(value.c_str());
            }
        } else if (_stricmp(key.c_str(), "User") == 0) {
            outConfig.user = value;
        } else if (_stricmp(key.c_str(), "Password") == 0) {
            outConfig.password = value;
        } else if (_stricmp(key.c_str(), "HostFingerprint") == 0) {
            outConfig.hostFingerprint = value;
        } else if (_stricmp(key.c_str(), "TimeoutSec") == 0) {
            outConfig.timeoutSec = atoi(value.c_str());
        } else if (_stricmp(key.c_str(), "RetryCount") == 0) {
            outConfig.retryCount = atoi(value.c_str());
        } else if (_stricmp(key.c_str(), "RetryIntervalSec") == 0) {
            outConfig.retryIntervalSec = atoi(value.c_str());
        } else if (_stricmp(key.c_str(), "RemoteFile") == 0) {
            remote[1] = value;
            used[1] = true;
        } else if (_stricmp(key.c_str(), "LocalFile") == 0) {
            local[1] = value;
            used[1] = true;
        } else {
            int index = 0;
            if (LooksLikeIndexedKey(key, "RemoteFile", &index)) {
                remote[index] = value;
                used[index] = true;
            } else if (LooksLikeIndexedKey(key, "LocalFile", &index)) {
                local[index] = value;
                used[index] = true;
            } else {
                LogWarn("CONFIG_LOAD", std::string("reason=unknown_key key=") + key);
            }
        }
    }

    // 필수값 검증
    if (outConfig.version.empty()) {
        LogError("CONFIG_LOAD", "reason=version_missing");
        return false;
    }
    if (outConfig.host.empty()) {
        LogError("CONFIG_LOAD", "reason=server_ip_missing key=ServerIP");
        return false;
    }
    if (outConfig.port <= 0 || outConfig.port > 65535) {
        std::ostringstream oss;
        oss << "reason=bad_server_port value=" << outConfig.port;
        LogError("CONFIG_LOAD", oss.str());
        return false;
    }
    if (outConfig.serverDirectory.empty()) {
        LogError("CONFIG_LOAD", "reason=server_directory_missing key=ServerDirectory");
        return false;
    }
    if (!StartsWithSlash(outConfig.serverDirectory)) {
        LogError("CONFIG_LOAD",
                 "reason=server_directory_must_be_absolute example=/data");
        return false;
    }
    if (outConfig.user.empty()) {
        LogError("CONFIG_LOAD", "reason=user_missing");
        return false;
    }
    if (outConfig.password.empty()) {
        LogError("CONFIG_LOAD", "reason=password_missing");
        return false;
    }
    if (outConfig.timeoutSec <= 0) {
        std::ostringstream oss;
        oss << "reason=bad_timeout_sec value=" << outConfig.timeoutSec;
        LogError("CONFIG_LOAD", oss.str());
        return false;
    }
    if (outConfig.retryCount < 1) {
        std::ostringstream oss;
        oss << "reason=bad_retry_count value=" << outConfig.retryCount;
        LogError("CONFIG_LOAD", oss.str());
        return false;
    }
    if (outConfig.retryIntervalSec < 0) {
        std::ostringstream oss;
        oss << "reason=bad_retry_interval_sec value=" << outConfig.retryIntervalSec;
        LogError("CONFIG_LOAD", oss.str());
        return false;
    }

    outConfig.serverDirectory = TrimTrailingSlashes(outConfig.serverDirectory);

    // RemoteFile/LocalFile 쌍을 최종 경로로 변환해 목록 구성
    for (size_t i = 1; i <= kMaxFiles; ++i) {
        if (!used[i] && remote[i].empty() && local[i].empty()) {
            continue;
        }

        if (remote[i].empty() || local[i].empty()) {
            std::ostringstream oss;
            oss << "reason=incomplete_file_pair index=" << i;
            LogError("CONFIG_LOAD", oss.str());
            return false;
        }

        FileTransferItem item;
        // ServerDirectory + RemoteFile → 최종 원격 경로
        item.remotePath = BuildRemotePath(outConfig.serverDirectory, remote[i]);
        item.localPath = local[i];
        outConfig.files.push_back(item);

        {
            std::ostringstream oss;
            oss << "index=" << i
                << " remote_input=" << remote[i]
                << " remote_resolved=" << item.remotePath
                << " local=" << item.localPath;
            LogInfo("CONFIG_FILE", oss.str());
        }
    }

    if (outConfig.files.empty()) {
        LogError("CONFIG_LOAD", "reason=no_download_files");
        return false;
    }

    std::ostringstream oss;
    oss << "path=" << path
        << " version=" << outConfig.version
        << " server_ip=" << outConfig.host
        << " server_port=" << outConfig.port
        << " server_directory=" << outConfig.serverDirectory
        << " file_count=" << outConfig.files.size()
        << " timeout_sec=" << outConfig.timeoutSec
        << " retry_count=" << outConfig.retryCount
        << " retry_interval_sec=" << outConfig.retryIntervalSec;
    LogInfo("CONFIG_LOAD", oss.str());
    return true;
}
