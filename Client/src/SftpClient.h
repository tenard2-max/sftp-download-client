#ifndef SFTP_DEMO_SFTP_CLIENT_H
#define SFTP_DEMO_SFTP_CLIENT_H

#include "Config.h"

// ---------------------------------------------------------------------------
// 기능: SFTP 서버 접속부터 파일 다운로드, 재시도, 종료까지 전체 워크플로 수행.
// 역할: main()이 호출하는 다운로드 엔진 진입점. Timeout/Retry/HostKey 검증 포함.
// 반환: 0=성공, 그 외=실패
// ---------------------------------------------------------------------------
int RunSftpDownload(const AppConfig& config);

#endif
