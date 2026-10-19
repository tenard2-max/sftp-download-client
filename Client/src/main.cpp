#include "Config.h"
#include "SftpClient.h"
#include "Logger.h"

#include <sstream>

// ---------------------------------------------------------------------------
// 기능: Client.exe 프로세스 진입점.
// 역할: 로거 초기화 → config.ini 로드 → SFTP 다운로드 엔진 실행 → 종료코드 반환.
// ---------------------------------------------------------------------------
int main(int /*argc*/, char** /*argv*/)
{
    // 콘솔/파일 로그 시스템 기동 (Client.log)
    InitLogger();
    LogInfo("APP_START", "name=SFTP_Download_Client_PoC");

    AppConfig config;

    // config.ini 파싱 및 필수값/파일목록 검증
    if (!LoadConfig(NULL, config)) {
        LogError("CONFIG_LOAD", "reason=load_failed");
        return 2;
    }

    // 로드된 핵심 설정 요약 로그
    {
        std::ostringstream oss;
        oss << "version=" << config.version
            << " server_ip=" << config.host
            << " server_port=" << config.port
            << " server_directory=" << config.serverDirectory
            << " user=" << config.user
            << " file_count=" << config.files.size()
            << " timeout_sec=" << config.timeoutSec
            << " retry_count=" << config.retryCount;
        LogOk("CONFIG_LOAD", oss.str());
    }

    // SFTP 전체 접속/다운로드/재시도 워크플로 실행
    const int rc = RunSftpDownload(config);

    // 최종 종료 코드 기록
    {
        std::ostringstream oss;
        oss << "exit_code=" << rc;
        if (rc == 0) {
            LogOk("APP_EXIT", oss.str());
        } else {
            LogError("APP_EXIT", oss.str());
        }
    }
    return rc;
}
