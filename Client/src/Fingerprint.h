#ifndef SFTP_DEMO_FINGERPRINT_H
#define SFTP_DEMO_FINGERPRINT_H

#include <libssh2.h>
#include <string>

// ---------------------------------------------------------------------------
// 기능: SSH 세션의 서버 Host Key로 OpenSSH 형식 SHA256 fingerprint를 만든다.
// 역할: Host Key 검증 시 실제 서버 fingerprint 산출.
// 형식: "SHA256:<base64>" (패딩 '=' 제거)
// 반환: 성공 시 fingerprint, 실패 시 빈 문자열
// ---------------------------------------------------------------------------
std::string BuildSha256Fingerprint(LIBSSH2_SESSION* session);

// ---------------------------------------------------------------------------
// 기능: 두 fingerprint 문자열을 정규화 후 비교한다.
// 역할: config.ini 값과 서버 실측값 일치 여부 판정.
// ---------------------------------------------------------------------------
bool FingerprintsEqual(const std::string& left, const std::string& right);

// ---------------------------------------------------------------------------
// 기능: HostFingerprint 설정이 유효하게 등록되었는지 검사한다.
// 역할: 미등록/와일드카드/trust-all 형태면 연결 금지.
// ---------------------------------------------------------------------------
bool IsFingerprintConfigured(const std::string& configured);

#endif
