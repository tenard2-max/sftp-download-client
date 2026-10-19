#include "Sha256.h"

#include <windows.h>
#include <bcrypt.h>

#include <fstream>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// 기능: 바이트 배열을 소문자 hex 문자열로 변환한다.
// 역할: SHA256 digest 표시용.
// ---------------------------------------------------------------------------
std::string ToHexLower(const unsigned char* data, size_t length)
{
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.resize(length * 2);
    for (size_t i = 0; i < length; ++i) {
        out[i * 2] = kHex[(data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[data[i] & 0x0F];
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// 기능: 로컬 파일 전체 내용의 SHA-256을 hex로 계산한다.
// 역할: 다운로드 성공 후 무결성/감사 로그(sha256=...) 기록.
// ---------------------------------------------------------------------------
std::string Sha256FileHex(const std::string& filePath)
{
    // 대상 파일 바이너리 오픈
    std::ifstream input(filePath.c_str(), std::ios::binary);
    if (!input) {
        return std::string();
    }

    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    DWORD objectLength = 0;
    DWORD dataLength = 0;
    std::vector<unsigned char> hashObject;
    unsigned char digest[32];
    std::string result;

    // Windows CNG SHA256 알고리즘 준비
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) {
        return std::string();
    }

    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectLength),
                          sizeof(objectLength), &dataLength, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return std::string();
    }

    hashObject.resize(objectLength);
    if (BCryptCreateHash(alg, &hash, &hashObject[0], objectLength, NULL, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return std::string();
    }

    // 파일 청크 단위로 해시 갱신
    char buffer[64 * 1024];
    while (input) {
        input.read(buffer, sizeof(buffer));
        const std::streamsize got = input.gcount();
        if (got <= 0) {
            break;
        }
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer),
                           static_cast<ULONG>(got), 0) != 0) {
            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(alg, 0);
            return std::string();
        }
    }

    // 최종 digest 추출 후 hex 변환
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
        result = ToHexLower(digest, sizeof(digest));
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}
