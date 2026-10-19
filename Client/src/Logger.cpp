#include "msvc_compat.h"
#include "Logger.h"
#include "Util.h"

#include <windows.h>

#include <fstream>
#include <iostream>
#include <string>

namespace {

CRITICAL_SECTION g_logLock;
bool g_logReady = false;
std::ofstream g_logFile;

// ---------------------------------------------------------------------------
// 기능: 현재 시각을 로그용 타임스탬프 문자열로 만든다.
// 역할: 각 로그 줄 앞에 로컬 시각(밀리초)을 붙인다.
// ---------------------------------------------------------------------------
std::string NowTimestamp()
{
    SYSTEMTIME st;
    GetLocalTime(&st);

    char buffer[64];
    sprintf_s(buffer, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
              st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return std::string(buffer);
}

// ---------------------------------------------------------------------------
// 기능: 레벨/STEP/메시지를 한 줄로 만들어 콘솔과 파일에 동시에 쓴다.
// 역할: LogInfo/LogWarn/LogError/LogOk가 공통으로 쓰는 출력 경로.
// ---------------------------------------------------------------------------
void WriteLine(const char* level, const char* step, const std::string& message)
{
    const std::string line = std::string("[") + NowTimestamp() + "] [" + level + "] [STEP=" +
                             step + "] " + message;

    if (g_logReady) {
        EnterCriticalSection(&g_logLock);
        std::cout << line << std::endl;
        if (g_logFile.is_open()) {
            g_logFile << line << std::endl;
            g_logFile.flush();
        }
        LeaveCriticalSection(&g_logLock);
    } else {
        std::cout << line << std::endl;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// 기능: 로거를 초기화한다. 콘솔과 exe 옆 Client.log(추가 기록)를 연다.
// 역할: main() 시작 시 1회 호출. 이후 모든 단계 로그의 출력 대상을 준비한다.
// ---------------------------------------------------------------------------
void InitLogger()
{
    if (g_logReady) {
        return;
    }

    InitializeCriticalSection(&g_logLock);

    // exe와 같은 폴더에 Client.log 생성/추가 기록
    const std::string path = JoinPath(GetExecutableDirectory(), "Client.log");
    g_logFile.open(path.c_str(), std::ios::out | std::ios::app);
    g_logReady = true;

    WriteLine("INFO", "LOGGER_INIT",
              std::string("log_file=") + path);
}

// ---------------------------------------------------------------------------
// 기능: INFO 레벨 로그를 출력한다.
// 역할: 단계 시작과 진행 정보를 WriteLine으로 넘긴다.
// ---------------------------------------------------------------------------
void LogInfo(const char* step, const std::string& message)
{
    WriteLine("INFO", step, message);
}

// ---------------------------------------------------------------------------
// 기능: WARN 레벨 로그를 출력한다.
// 역할: 재시도처럼 치명적이지 않은 경고를 WriteLine으로 넘긴다.
// ---------------------------------------------------------------------------
void LogWarn(const char* step, const std::string& message)
{
    WriteLine("WARN", step, message);
}

// ---------------------------------------------------------------------------
// 기능: ERROR 레벨 로그를 출력한다.
// 역할: 실패 사유를 WriteLine으로 넘긴다.
// ---------------------------------------------------------------------------
void LogError(const char* step, const std::string& message)
{
    WriteLine("ERROR", step, message);
}

// ---------------------------------------------------------------------------
// 기능: OK 레벨 로그를 출력한다.
// 역할: 단계 성공을 WriteLine으로 넘긴다.
// ---------------------------------------------------------------------------
void LogOk(const char* step, const std::string& message)
{
    WriteLine("OK  ", step, message);
}
