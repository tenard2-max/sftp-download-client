#ifndef SFTP_DEMO_CONFIG_H
#define SFTP_DEMO_CONFIG_H

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 원격/로컬 파일 1쌍 정보
// ---------------------------------------------------------------------------
struct FileTransferItem {
    std::string remotePath; // SFTP 서버상 최종 경로 (ServerDirectory 반영 후)
    std::string localPath;  // Windows 로컬 저장 경로
};

// ---------------------------------------------------------------------------
// config.ini 전체 설정값
// ---------------------------------------------------------------------------
struct AppConfig {
    // 패키지/설정 버전 (예: 1.0.0)
    std::string version;

    // 서버 접속 정보 (ServerIP/ServerPort 우선, Host/Port 호환)
    std::string host;
    int port;

    // SFTP 원격 기본 디렉터리 (절대경로, 예: /data)
    std::string serverDirectory;

    std::string user;
    std::string password;
    std::string hostFingerprint;
    std::vector<FileTransferItem> files;

    // TCP/SSH/SFTP 타임아웃(초). 기본 132
    int timeoutSec;
    // 초기 접속 실패 시 시도 횟수. 기본 5, 모두 실패하면 종료
    int retryCount;
    // 재시도 대기(초). 기본 5
    int retryIntervalSec;
    // config.ini DosWindow. show 이면 DOS 창을 띄운다. 기본은 숨김
    bool showDosWindow;

    AppConfig()
        : port(22)
        , timeoutSec(132)
        , retryCount(5)
        , retryIntervalSec(5)
        , showDosWindow(false)
    {
        version = "1.0.0";
    }
};

// ---------------------------------------------------------------------------
// 기능: config.ini를 읽어 AppConfig를 채운다.
// 역할: 필수값 검증, ServerDirectory+RemoteFile 경로 결합, 파일 목록 구성.
// 인자: absolutePath - NULL이면 exe 옆 config.ini 사용
// 반환: 성공 true / 실패 false (로그에 사유 기록)
// ---------------------------------------------------------------------------
bool LoadConfig(const char* absolutePath, AppConfig& outConfig);

// ---------------------------------------------------------------------------
// 기능: config.ini 의 DosWindow 값만 먼저 읽는다.
// 역할: 로그를 열기 전에 DOS 창을 보일지 결정한다. 없거나 hide 이면 false.
// ---------------------------------------------------------------------------
bool PeekDosWindow();

#endif
