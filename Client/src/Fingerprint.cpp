#include "msvc_compat.h"
#include "Fingerprint.h"
#include "Util.h"

#include <libssh2.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace {

const char kBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// ---------------------------------------------------------------------------
// 기능: 바이너리 데이터를 Base64 문자열로 인코딩한다.
// 역할: Host Key SHA256 해시를 OpenSSH fingerprint 형식으로 만들 때 사용.
// ---------------------------------------------------------------------------
std::string Base64Encode(const unsigned char* data, size_t length)
{
    std::string output;
    output.reserve(((length + 2) / 3) * 4);

    size_t i = 0;
    while (i + 2 < length) {
        const unsigned int n =
            (static_cast<unsigned int>(data[i]) << 16) |
            (static_cast<unsigned int>(data[i + 1]) << 8) |
            static_cast<unsigned int>(data[i + 2]);
        output.push_back(kBase64Table[(n >> 18) & 63]);
        output.push_back(kBase64Table[(n >> 12) & 63]);
        output.push_back(kBase64Table[(n >> 6) & 63]);
        output.push_back(kBase64Table[n & 63]);
        i += 3;
    }

    if (i < length) {
        unsigned int n = static_cast<unsigned int>(data[i]) << 16;
        output.push_back(kBase64Table[(n >> 18) & 63]);
        if (i + 1 < length) {
            n |= static_cast<unsigned int>(data[i + 1]) << 8;
            output.push_back(kBase64Table[(n >> 12) & 63]);
            output.push_back(kBase64Table[(n >> 6) & 63]);
            output.push_back('=');
        } else {
            output.push_back(kBase64Table[(n >> 12) & 63]);
            output.push_back('=');
            output.push_back('=');
        }
    }

    return output;
}

// ---------------------------------------------------------------------------
// 기능: Base64 끝의 '=' 패딩을 제거한다.
// 역할: OpenSSH fingerprint 표기와 맞추기.
// ---------------------------------------------------------------------------
std::string StripBase64Padding(std::string value)
{
    while (!value.empty() && value[value.size() - 1] == '=') {
        value.erase(value.size() - 1);
    }
    return value;
}

// ---------------------------------------------------------------------------
// 기능: fingerprint 문자열을 "SHA256:<값>" 형태로 정규화한다.
// 역할: 접두어/공백/패딩 차이를 없애고 공정 비교.
// ---------------------------------------------------------------------------
std::string NormalizeFingerprint(const std::string& raw)
{
    std::string value = Trim(raw);
    value.erase(std::remove(value.begin(), value.end(), ' '), value.end());
    value.erase(std::remove(value.begin(), value.end(), '\t'), value.end());

    const std::string lower = ToLowerAscii(value);
    if (lower.size() >= 7 && lower.compare(0, 7, "sha256:") == 0) {
        value = value.substr(7);
    }

    return "SHA256:" + StripBase64Padding(value);
}

} // namespace

// ---------------------------------------------------------------------------
// 기능: SSH 세션 Host Key의 SHA256 fingerprint를 생성한다.
// 역할: 서버 실측 fingerprint 산출 (Host Key 검증용).
// ---------------------------------------------------------------------------
std::string BuildSha256Fingerprint(LIBSSH2_SESSION* session)
{
    if (session == NULL) {
        return std::string();
    }

    // libssh2가 계산한 32바이트 SHA256 해시 취득
    const char* hash = libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (hash == NULL) {
        return std::string();
    }

    // OpenSSH 형식(Base64, 패딩 제거)으로 변환
    const std::string encoded =
        Base64Encode(reinterpret_cast<const unsigned char*>(hash), 32);
    return "SHA256:" + StripBase64Padding(encoded);
}

// ---------------------------------------------------------------------------
// 기능: 두 fingerprint를 정규화 후 대소문자 무시 비교한다.
// 역할: config 기대값 vs 서버 실측값 일치 판정.
// ---------------------------------------------------------------------------
bool FingerprintsEqual(const std::string& left, const std::string& right)
{
    // 접두어/공백/패딩 차이를 없앤 뒤 비교한다.
    const std::string a = NormalizeFingerprint(left);
    const std::string b = NormalizeFingerprint(right);
    return _stricmp(a.c_str(), b.c_str()) == 0;
}

// ---------------------------------------------------------------------------
// 기능: HostFingerprint 설정이 연결 허용 가능한지 검사한다.
// 역할: 미등록/와일드카드/trust-all 이면 false (연결 금지).
// ---------------------------------------------------------------------------
bool IsFingerprintConfigured(const std::string& configured)
{
    // 앞뒤 공백을 제거한 설정값만 검사한다.
    const std::string value = Trim(configured);
    if (value.empty()) {
        return false;
    }

    // 검증을 끄는 예약어는 미등록과 같이 취급한다.
    const std::string lower = ToLowerAscii(value);
    if (lower == "none" || lower == "trust-all" || lower == "*") {
        return false;
    }

    // SHA256 접두 정규화 후 실제 값이 있는지 확인한다.
    const std::string normalized = NormalizeFingerprint(value);
    // "SHA256:" 접두 이후 실제 값이 있어야 함
    return normalized.size() > 8;
}
