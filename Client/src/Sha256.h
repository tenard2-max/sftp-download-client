#ifndef SFTP_DEMO_SHA256_H
#define SFTP_DEMO_SHA256_H

#include <string>

// ---------------------------------------------------------------------------
// 기능: 로컬 파일의 SHA-256 해시를 소문자 hex 문자열로 계산한다.
// 역할: 다운로드 성공 검증/감사 로그용 무결성 값 생성 (Windows BCrypt).
// 반환: 성공 시 hex 문자열, 실패 시 빈 문자열
// ---------------------------------------------------------------------------
std::string Sha256FileHex(const std::string& filePath);

#endif
