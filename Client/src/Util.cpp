#include "Util.h"

#include <algorithm>
#include <cctype>
#include <windows.h>
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

// ---------------------------------------------------------------------------
// 기능: 문자열 앞뒤 공백/탭/개행을 제거한다.
// 역할: config.ini 키/값 파싱 전처리.
// ---------------------------------------------------------------------------
std::string Trim(const std::string& value)
{
    size_t begin = 0;
    while (begin < value.size() &&
           (value[begin] == ' ' || value[begin] == '\t' ||
            value[begin] == '\r' || value[begin] == '\n')) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin &&
           (value[end - 1] == ' ' || value[end - 1] == '\t' ||
            value[end - 1] == '\r' || value[end - 1] == '\n')) {
        --end;
    }

    return value.substr(begin, end - begin);
}

// ---------------------------------------------------------------------------
// 기능: ASCII 알파벳을 소문자로 변환한다.
// 역할: fingerprint/설정 키 대소문자 무시 비교.
// ---------------------------------------------------------------------------
std::string ToLowerAscii(const std::string& value)
{
    std::string result = value;
    for (size_t i = 0; i < result.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(result[i]);
        result[i] = static_cast<char>(tolower(ch));
    }
    return result;
}

// ---------------------------------------------------------------------------
// 기능: 디렉터리를 부모부터 재귀 생성한다.
// 역할: EnsureParentDirectory의 내부 구현.
// ---------------------------------------------------------------------------
static bool CreateDirectoryRecursive(const std::string& directory)
{
    if (directory.empty()) {
        return false;
    }

    // 이미 존재하면 디렉터리인지 확인
    if (PathFileExistsA(directory.c_str())) {
        return PathIsDirectoryA(directory.c_str()) ? true : false;
    }

    // 부모 폴더 먼저 생성
    const size_t lastSlash = directory.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        const std::string parent = directory.substr(0, lastSlash);
        if (!parent.empty() && !CreateDirectoryRecursive(parent)) {
            return false;
        }
    }

    if (CreateDirectoryA(directory.c_str(), NULL)) {
        return true;
    }

    return GetLastError() == ERROR_ALREADY_EXISTS;
}

// ---------------------------------------------------------------------------
// 기능: 파일 전체 경로에서 부모 폴더만 추출해 없으면 생성한다.
// 역할: LocalFile 쓰기 전 경로 준비.
// ---------------------------------------------------------------------------
bool EnsureParentDirectory(const std::string& filePath)
{
    const size_t pos = filePath.find_last_of("\\/");
    if (pos == std::string::npos) {
        return true;
    }

    const std::string directory = filePath.substr(0, pos);
    if (directory.empty() ||
        (directory.size() == 2 && directory[1] == ':')) {
        return true;
    }

    // 부모 폴더 재귀 생성
    return CreateDirectoryRecursive(directory);
}

// ---------------------------------------------------------------------------
// 기능: 실행 중 exe의 디렉터리 절대경로를 반환한다.
// 역할: config.ini, Client.log 위치 결정.
// ---------------------------------------------------------------------------
std::string GetExecutableDirectory()
{
    char buffer[MAX_PATH] = {0};
    const DWORD length = GetModuleFileNameA(NULL, buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return ".";
    }

    std::string path(buffer);
    const size_t pos = path.find_last_of("\\/");
    if (pos == std::string::npos) {
        return ".";
    }
    return path.substr(0, pos);
}

// ---------------------------------------------------------------------------
// 기능: 디렉터리 경로와 파일명을 백슬래시로 결합한다.
// 역할: config.ini와 Client.log처럼 exe와 같은 폴더의 파일 경로를 만든다.
// ---------------------------------------------------------------------------
std::string JoinPath(const std::string& directory, const std::string& fileName)
{
    if (directory.empty()) {
        return fileName;
    }
    const char last = directory[directory.size() - 1];
    if (last == '\\' || last == '/') {
        return directory + fileName;
    }
    return directory + "\\" + fileName;
}
