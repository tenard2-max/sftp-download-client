// 내부 발표용 800x600 다이얼로그.
// sft_ser.exe 는 서버 기동과 서버 설정만, sft_cli.exe 는 다운로드와 진행도만 보여 준다.
// 다운로드 본체는 같은 폴더의 Client.exe 다.

#include <windows.h>
#include <tlhelp32.h>
#include <gdiplus.h>

#include <string>
#include <vector>
#include <cstdlib>

namespace {

const int kClientWidth = 800;
const int kClientHeight = 688;

const int IDC_BTN_DOWN = 101;
const int IDC_BTN_SERVER = 102;
const int IDC_BTN_EXIT = 103;
const int IDC_LOG = 104;
const int IDC_BTN_SETUP = 105;

const int IDC_SETUP_USER = 301;
const int IDC_SETUP_PASS = 302;
const int IDC_SETUP_FP = 303;
const int IDC_INPUT_EDIT = 311;
const int IDC_INPUT_SAVE = 312;
const int IDC_FP_EDIT = 321;
const int IDC_FP_MAKE = 322;

const UINT WM_APP_LOG = WM_APP + 1;
const UINT WM_APP_FP = WM_APP + 2;
const UINT_PTR kPollTimer = 1;

const COLORREF kFrame = RGB(0, 90, 160);
const COLORREF kTitle = RGB(0, 74, 140);
const COLORREF kOk = RGB(0, 122, 204);
const COLORREF kError = RGB(186, 28, 28);
const COLORREF kWarn = RGB(176, 96, 0);
const COLORREF kInfo = RGB(15, 76, 148);
const COLORREF kMuted = RGB(90, 110, 130);
const COLORREF kBrand = RGB(214, 0, 81);
const COLORREF kBrandFill = RGB(255, 240, 245);

struct LogLine {
    std::wstring text;
    COLORREF color;
};

HINSTANCE g_inst = NULL;
Gdiplus::Image* g_logo = NULL;
HWND g_wnd = NULL;
HWND g_log = NULL;
HWND g_btnDown = NULL;
HWND g_btnServer = NULL;
HWND g_btnExit = NULL;
HWND g_btnSetup = NULL;
HWND g_setupWnd = NULL;
HWND g_userWnd = NULL;
HWND g_passWnd = NULL;
HWND g_fpWnd = NULL;
HWND g_fpEdit = NULL;
HWND g_fpButton = NULL;
bool g_fpBusy = false;
bool g_popupClassReady = false;
HFONT g_font = NULL;
HFONT g_fontBold = NULL;
HFONT g_fontExit = NULL;

std::vector<LogLine> g_lines;
std::wstring g_exeDir;
std::string g_logCarry;
__int64 g_logPos = 0;
HANDLE g_process = NULL;
int g_percent = 0;
int g_itemPercent = 0;
int g_fileCount = 1;
int g_fileDone = 0;
bool g_failed = false;
bool g_busy = false;
bool g_serverRunning = false;
bool g_serverApp = false;

// ---------------------------------------------------------------------------
// 기능: UTF-8 문자열을 화면용 와이드 문자열로 바꾼다.
// 역할: Client.log 한 줄을 리스트에 넣기 전에 사용한다.
// ---------------------------------------------------------------------------
std::wstring Widen(const std::string& text)
{
    if (text.empty()) {
        return std::wstring();
    }
    const int need = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                         static_cast<int>(text.size()), NULL, 0);
    if (need <= 0) {
        const int ansi = MultiByteToWideChar(CP_ACP, 0, text.c_str(),
                                             static_cast<int>(text.size()), NULL, 0);
        std::wstring out(ansi, L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()),
                            &out[0], ansi);
        return out;
    }
    std::wstring out(need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        &out[0], need);
    return out;
}

// ---------------------------------------------------------------------------
// 기능: 발표 창 exe가 있는 폴더를 구한다.
// 역할: 같은 폴더의 Client.exe, Client.log, config.ini 를 찾는다.
// ---------------------------------------------------------------------------
std::wstring ExeDirectory()
{
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring full(path);
    const size_t pos = full.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return L".";
    }
    return full.substr(0, pos);
}

// ---------------------------------------------------------------------------
// 기능: 대괄호 토큰과 나머지 메시지를 로그 한 줄에서 분리한다.
// 역할: [LEVEL], [STEP=...] 를 읽어 중요 단계만 고른다.
// ---------------------------------------------------------------------------
bool ParseLogLine(const std::string& line, std::string& level, std::string& step, std::string& message)
{
    const size_t first = line.find("] [");
    if (first == std::string::npos) {
        return false;
    }
    const size_t levelBegin = first + 3;
    const size_t levelEnd = line.find(']', levelBegin);
    if (levelEnd == std::string::npos) {
        return false;
    }
    level = line.substr(levelBegin, levelEnd - levelBegin);
    while (!level.empty() && level[level.size() - 1] == ' ') {
        level.erase(level.size() - 1);
    }

    const std::string marker = "[STEP=";
    const size_t stepPos = line.find(marker, levelEnd);
    if (stepPos == std::string::npos) {
        return false;
    }
    const size_t stepBegin = stepPos + marker.size();
    const size_t stepEnd = line.find(']', stepBegin);
    if (stepEnd == std::string::npos) {
        return false;
    }
    step = line.substr(stepBegin, stepEnd - stepBegin);
    message = line.substr(stepEnd + 1);
    while (!message.empty() && (message[0] == ' ' || message[0] == '\t')) {
        message.erase(0, 1);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 기능: key=value 형태에서 값만 꺼낸다.
// 역할: 서버 주소, 파일명, 종료 코드를 화면 문장으로 만들 때 사용한다.
// ---------------------------------------------------------------------------
std::string Field(const std::string& message, const char* key)
{
    const std::string token = std::string(key) + "=";
    const size_t pos = message.find(token);
    if (pos == std::string::npos) {
        return std::string();
    }
    size_t begin = pos + token.size();
    size_t end = message.find(' ', begin);
    if (end == std::string::npos) {
        end = message.size();
    }
    return message.substr(begin, end - begin);
}

// ---------------------------------------------------------------------------
// 기능: 진행 막대 퍼센트를 바꾸고 다시 그린다.
// 역할: 접속 단계가 끝날 때마다 전체 진행도를 갱신한다.
// ---------------------------------------------------------------------------
void RefreshProgress()
{
    if (g_wnd != NULL) {
        RECT area = {18, 468, 782, 582};
        InvalidateRect(g_wnd, &area, TRUE);
    }
}

// ---------------------------------------------------------------------------
// 기능: 전체 진행도 막대 값을 바꾼다.
// 역할: 접속 단계가 끝날 때마다 아래쪽 막대를 갱신한다.
// ---------------------------------------------------------------------------
void SetPercent(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    g_percent = percent;
    RefreshProgress();
}

// ---------------------------------------------------------------------------
// 기능: 현재 파일 진행도와, 그 비율이 반영된 전체 진행도를 같이 갱신한다.
// 역할: 위쪽 막대는 파일 1개, 아래쪽 막대는 첫 파일 20%부터 전체 완료 100%까지 올린다.
// ---------------------------------------------------------------------------
void SetFileBars(int itemPercent)
{
    if (itemPercent < 0) {
        itemPercent = 0;
    }
    if (itemPercent > 100) {
        itemPercent = 100;
    }
    g_itemPercent = itemPercent;
    if (g_fileCount < 1) {
        g_fileCount = 1;
    }
    int overall = 20 + (80 * (g_fileDone * 100 + g_itemPercent)) / (g_fileCount * 100);
    if (overall > 100) {
        overall = 100;
    }
    g_percent = overall;
    RefreshProgress();
}

// ---------------------------------------------------------------------------
// 기능: 색이 있는 로그 한 줄을 목록 맨 아래에 추가한다.
// 역할: 중요 접속 로그만 스크롤 영역에 보여 준다.
// ---------------------------------------------------------------------------
void AddLog(const std::wstring& text, COLORREF color)
{
    LogLine line;
    line.text = text;
    line.color = color;
    g_lines.push_back(line);

    const int index = static_cast<int>(SendMessageW(g_log, LB_ADDSTRING, 0,
                                                    reinterpret_cast<LPARAM>(text.c_str())));
    if (index >= 0) {
        SendMessageW(g_log, LB_SETITEMDATA, index, static_cast<LPARAM>(color));
        SendMessageW(g_log, LB_SETTOPINDEX, index, 0);
    }
}

// ---------------------------------------------------------------------------
// 기능: 작업 중에는 실행 버튼을 잠근다.
// 역할: 다운로드가 도는 동안 중복 실행을 막는다.
// ---------------------------------------------------------------------------
void SetBusy(bool busy)
{
    g_busy = busy;
    if (!g_serverApp) {
        return;
    }
    if (g_btnDown != NULL) {
        EnableWindow(g_btnDown, busy ? FALSE : TRUE);
    }
    if (g_btnServer != NULL) {
        EnableWindow(g_btnServer, busy ? FALSE : TRUE);
    }
}

// ---------------------------------------------------------------------------
// 기능: Client.log 의 중요 단계를 짧은 한글 문장으로 바꾼다.
// 역할: 발표 화면에 접속, 검증, 로그인, 수신 결과만 남긴다.
// ---------------------------------------------------------------------------
void ConsumeImportantLine(const std::string& level, const std::string& step, const std::string& message)
{
    const bool ok = level == "OK";
    const bool error = level == "ERROR";
    const bool warn = level == "WARN";
    COLORREF color = kInfo;
    if (ok) {
        color = kOk;
    } else if (error) {
        color = kError;
        g_failed = true;
    } else if (warn) {
        color = kWarn;
    }

    if (step == "CONFIG_LOAD" && ok) {
        const std::string host = Field(message, "server_ip");
        const std::string port = Field(message, "server_port");
        const std::string files = Field(message, "file_count");
        g_fileCount = atoi(files.c_str());
        if (g_fileCount < 1) {
            g_fileCount = 1;
        }
        AddLog(L"설정 로드   서버 " + Widen(host) + L":" + Widen(port) +
               L"   파일 " + Widen(files) + L"개", kOk);
        SetPercent(3);
        return;
    }
    if (step == "TCP_CONNECT" && (ok || error)) {
        AddLog(ok ? L"TCP 연결 성공" : L"TCP 연결 실패   " + Widen(message), color);
        SetPercent(ok ? 7 : g_percent);
        return;
    }
    if (step == "SSH_HANDSHAKE" && (ok || error)) {
        AddLog(ok ? L"SSH 핸드셰이크 성공" : L"SSH 핸드셰이크 실패", color);
        SetPercent(ok ? 11 : g_percent);
        return;
    }
    if (step == "HOSTKEY_FINGERPRINT") {
        const std::string actual = Field(message, "actual");
        const std::string expected = Field(message, "expected");
        const bool same = !actual.empty() && actual == expected;
        AddLog(std::wstring(L"Fingerprint   ") + Widen(actual), same ? kInfo : kWarn);
        return;
    }
    if (step == "HOSTKEY_VERIFY" && (ok || error)) {
        AddLog(ok ? L"Fingerprint 검증 성공" : L"Fingerprint 검증 실패   연결 종료", color);
        SetPercent(ok ? 15 : g_percent);
        return;
    }
    if (step == "AUTH_PASSWORD" && (ok || error)) {
        AddLog(ok ? L"로그인 성공   " + Widen(Field(message, "user"))
                  : L"로그인 실패 (계정/비밀번호)", color);
        SetPercent(ok ? 18 : g_percent);
        return;
    }
    if (step == "SFTP_INIT" && (ok || error)) {
        AddLog(ok ? L"SFTP 세션 시작" : L"SFTP 세션 실패", color);
        SetPercent(ok ? 20 : g_percent);
        return;
    }
    if (step == "DOWNLOAD_ITEM") {
        SetFileBars(0);
        AddLog(L"수신 시작   " + Widen(Field(message, "index")) + L"   " + Widen(Field(message, "remote")), kInfo);
        return;
    }
    if (step == "DOWNLOAD_PROGRESS") {
        SetFileBars(atoi(Field(message, "percent").c_str()));
        return;
    }
    if (step == "DOWNLOAD_RESUME") {
        const std::string offset = Field(message, "offset");
        if (!offset.empty()) {
            AddLog(L"이어 받기   " + Widen(Field(message, "file")) +
                   L"   " + Widen(offset) + L" bytes", kInfo);
        }
        return;
    }
    if (step == "DOWNLOAD" && ok) {
        SetFileBars(100);
        ++g_fileDone;
        const std::wstring label = Field(message, "reason") == "already_complete"
            ? L"이미 받음   "
            : L"수신 완료   ";
        AddLog(label + Widen(Field(message, "file")) +
               L"   " + Widen(Field(message, "size")) + L" bytes", kOk);
        return;
    }
    if ((step == "DOWNLOAD" || step == "REMOTE_STAT" || step == "REMOTE_OPEN" || step == "DOWNLOAD_READ") && error) {
        AddLog(L"수신 실패   " + Widen(message), kError);
        return;
    }
    if (step == "RETRY" && !error) {
        const bool transfer = Field(message, "stage") == "transfer";
        const std::wstring kind = transfer ? L"파일 전송" : L"연결";
        const std::string attempt = Field(message, "attempt");
        if (!attempt.empty()) {
            g_fileDone = 0;
            g_itemPercent = 0;
            RefreshProgress();
            AddLog(kind + L" 시도   " + Widen(attempt), kInfo);
            return;
        }
        const std::string next = Field(message, "next_attempt");
        if (!next.empty()) {
            AddLog(kind + L" 실패   " + Widen(next) + L"회차 재시도", kWarn);
        }
        return;
    }
    if (step == "RUN_RESULT" && error) {
        const std::string reason = Field(message, "reason");
        const bool transfer = Field(message, "stage") == "transfer";
        if (reason == "transfer_time_exceeded") {
            AddLog(L"파일 전송 20분 초과   종료", kError);
        } else if (reason == "max_retry_exceeded") {
            const std::wstring kind = transfer ? L"파일 전송 " : L"연결 ";
            AddLog(kind + Widen(Field(message, "retry_count")) + L"회 실패   종료", kError);
        } else {
            AddLog(L"연결 중단   재시도 없이 종료", kError);
        }
        return;
    }
    if (step == "APP_EXIT") {
        const std::string code = Field(message, "exit_code");
        if (code == "0") {
            // 중간에 끊겨도 이어 받기로 끝나면 성공이다. 진행 막대 색도 성공으로 되돌린다.
            g_failed = false;
            AddLog(L"전체 다운로드 성공", kOk);
            g_itemPercent = 100;
            SetPercent(100);
        } else {
            AddLog(L"다운로드 실패   exit " + Widen(code), kError);
        }
    }
}

// ---------------------------------------------------------------------------
// 기능: Client.log 에 새로 쌓인 줄만 읽어 화면에 반영한다.
// 역할: 이번 실행의 접속 로그만 진행 막대와 함께 갱신한다.
// ---------------------------------------------------------------------------
void PollLog()
{
    const std::wstring path = g_exeDir + L"\\Client.log";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < g_logPos) {
        CloseHandle(file);
        return;
    }
    LARGE_INTEGER pos;
    pos.QuadPart = g_logPos;
    SetFilePointerEx(file, pos, NULL, FILE_BEGIN);

    char buffer[2048];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, NULL) && read > 0) {
        g_logCarry.append(buffer, buffer + read);
        g_logPos += read;
    }
    CloseHandle(file);

    size_t start = 0;
    for (;;) {
        const size_t nl = g_logCarry.find('\n', start);
        if (nl == std::string::npos) {
            g_logCarry.erase(0, start);
            break;
        }
        std::string one = g_logCarry.substr(start, nl - start);
        if (!one.empty() && one[one.size() - 1] == '\r') {
            one.erase(one.size() - 1);
        }
        std::string level;
        std::string step;
        std::string message;
        if (ParseLogLine(one, level, step, message)) {
            ConsumeImportantLine(level, step, message);
        }
        start = nl + 1;
        if (start >= g_logCarry.size()) {
            g_logCarry.clear();
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// 기능: 이번 실행 이전의 로그는 건너뛴다.
// 역할: 화면에는 버튼을 누른 뒤의 접속 로그만 나오게 한다.
// ---------------------------------------------------------------------------
void MarkLogEnd()
{
    const std::wstring path = g_exeDir + L"\\Client.log";
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        LARGE_INTEGER size;
        size.HighPart = info.nFileSizeHigh;
        size.LowPart = info.nFileSizeLow;
        g_logPos = size.QuadPart;
    } else {
        g_logPos = 0;
    }
}

// ---------------------------------------------------------------------------
// 기능: dist\Client.exe 를 창 없이 실행한다.
// 역할: SFTP DOWN 버튼이 실제 다운로드 클라이언트를 돌린다.
// ---------------------------------------------------------------------------
bool StartDownload()
{
    const std::wstring exe = g_exeDir + L"\\Client.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        AddLog(L"Client.exe 를 찾지 못했습니다.", kError);
        return false;
    }

    std::wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    const BOOL ok = CreateProcessW(NULL, &mutableCmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW,
                                   NULL, g_exeDir.c_str(), &si, &pi);
    if (!ok) {
        AddLog(L"Client.exe 실행 실패", kError);
        return false;
    }
    CloseHandle(pi.hThread);
    g_process = pi.hProcess;
    return true;
}

// ---------------------------------------------------------------------------
// 기능: sshd 서비스가 실행 중인지 확인한다.
// 역할: 서버기동 버튼의 현재 상태를 로그에 남긴다.
// ---------------------------------------------------------------------------
DWORD QuerySshdState()
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm == NULL) {
        return 0;
    }
    SC_HANDLE service = OpenServiceW(scm, L"sshd", SERVICE_QUERY_STATUS);
    DWORD state = 0;
    if (service != NULL) {
        SERVICE_STATUS status;
        if (QueryServiceStatus(service, &status)) {
            state = status.dwCurrentState;
        }
        CloseServiceHandle(service);
    }
    CloseServiceHandle(scm);
    return state;
}

// ---------------------------------------------------------------------------
// 기능: sshd가 원하는 상태가 될 때까지 짧게 기다린다.
// 역할: 기동·종료 직후 버튼 문구를 실제 서비스 상태와 맞춘다.
// ---------------------------------------------------------------------------
bool WaitForSshdState(DWORD wanted, DWORD timeoutMs)
{
    const DWORD begin = GetTickCount();
    for (;;) {
        if (QuerySshdState() == wanted) {
            return true;
        }
        if (GetTickCount() - begin >= timeoutMs) {
            return false;
        }
        Sleep(200);
    }
}

// ---------------------------------------------------------------------------
// 기능: 서비스 상태에 맞춰 서버 버튼 문구를 바꾼다.
// 역할: 실행 중이면 서버종료, 멈춰 있으면 서버기동으로 보여 준다.
// ---------------------------------------------------------------------------
void UpdateServerButton()
{
    g_serverRunning = (QuerySshdState() == SERVICE_RUNNING);
    if (g_btnServer != NULL) {
        SetWindowTextW(g_btnServer, g_serverRunning ? L"서버종료" : L"서버기동");
        InvalidateRect(g_btnServer, NULL, FALSE);
    }
}

// ---------------------------------------------------------------------------
// 기능: 이름이 같은 프로세스 개수를 센다. skipPid 는 제외한다.
// 역할: 파일 전송 프로세스와, 서비스가 아닌 접속 세션을 구분한다.
// ---------------------------------------------------------------------------
int CountProcesses(const wchar_t* name, DWORD skipPid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return -1;
    }
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    int count = 0;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (entry.th32ProcessID == skipPid) {
                continue;
            }
            if (lstrcmpiW(entry.szExeFile, name) == 0) {
                ++count;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return count;
}

// ---------------------------------------------------------------------------
// 기능: 이름이 같은 프로세스를 끝낸다. skipPid 는 남긴다.
// 역할: sftp-server.exe 와 이미 붙은 sshd 세션을 멈춰 파일 데이터를 끊는다.
// ---------------------------------------------------------------------------
bool TerminateProcessesByName(const wchar_t* name, DWORD skipPid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            if (entry.th32ProcessID == skipPid || lstrcmpiW(entry.szExeFile, name) != 0) {
                continue;
            }
            HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID);
            if (process == NULL) {
                continue;
            }
            if (TerminateProcess(process, 1)) {
                WaitForSingleObject(process, 3000);
            }
            CloseHandle(process);
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return CountProcesses(name, skipPid) == 0;
}

// ---------------------------------------------------------------------------
// 기능: sshd 서비스 프로세스 번호를 구한다.
// 역할: 새 접속을 받는 프로세스는 남기고, 이미 붙은 세션만 끊기 위해 구분한다.
// ---------------------------------------------------------------------------
DWORD QuerySshdPid()
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (scm == NULL) {
        return 0;
    }
    SC_HANDLE service = OpenServiceW(scm, L"sshd", SERVICE_QUERY_STATUS);
    DWORD pid = 0;
    if (service != NULL) {
        SERVICE_STATUS_PROCESS status;
        DWORD bytes = 0;
        ZeroMemory(&status, sizeof(status));
        if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                 reinterpret_cast<LPBYTE>(&status), sizeof(status), &bytes)) {
            if (status.dwCurrentState != SERVICE_STOPPED) {
                pid = status.dwProcessId;
            }
        }
        CloseServiceHandle(service);
    }
    CloseServiceHandle(scm);
    return pid;
}

// ---------------------------------------------------------------------------
// 기능: 프로세스가 사라질 때까지 잠시 기다린다.
// 역할: 종료 직후 파일 전송이 실제로 멈췄는지 확인한다.
// ---------------------------------------------------------------------------
bool WaitUntilProcessGone(const wchar_t* name, DWORD skipPid, DWORD timeoutMs)
{
    const DWORD begin = GetTickCount();
    for (;;) {
        if (CountProcesses(name, skipPid) == 0) {
            return true;
        }
        if (GetTickCount() - begin >= timeoutMs) {
            return false;
        }
        Sleep(200);
    }
}

// ---------------------------------------------------------------------------
// 기능: 관리자 권한으로 프로그램을 실행하고 끝날 때까지 기다린다.
// 역할: 일반 권한으로 서비스나 파일 전송 프로세스를 멈추지 못할 때 승격 창을 띄운다.
// ---------------------------------------------------------------------------
bool RunElevated(const wchar_t* file, const wchar_t* parameters)
{
    SHELLEXECUTEINFOW info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = file;
    info.lpParameters = parameters;
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        return false;
    }
    if (info.hProcess != NULL) {
        WaitForSingleObject(info.hProcess, 20000);
        CloseHandle(info.hProcess);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 기능: 관리자 권한으로 net 명령을 실행하고 끝날 때까지 기다린다.
// 역할: 일반 권한으로 sshd를 켜거나 끄지 못할 때 승격 창을 띄운다.
// ---------------------------------------------------------------------------
bool RunElevatedNet(const wchar_t* parameters)
{
    return RunElevated(L"net", parameters);
}

// ---------------------------------------------------------------------------
// 기능: sshd 서비스를 시작한다. 권한이 없으면 관리자 승인을 띄운다.
// 역할: 서버기동 버튼 처리.
// ---------------------------------------------------------------------------
DWORD WINAPI StartServerThread(LPVOID)
{
    const DWORD state = QuerySshdState();
    if (state == SERVICE_RUNNING) {
        PostMessageW(g_wnd, WM_APP_LOG, kOk, reinterpret_cast<LPARAM>(new std::wstring(L"서버 이미 실행 중   sshd")));
        PostMessageW(g_wnd, WM_APP_LOG, 1, 0);
        return 0;
    }

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    SC_HANDLE service = NULL;
    if (scm != NULL) {
        service = OpenServiceW(scm, L"sshd", SERVICE_START | SERVICE_QUERY_STATUS);
    }
    BOOL started = FALSE;
    if (service != NULL) {
        started = StartServiceW(service, 0, NULL);
        if (!started && GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
            started = TRUE;
        }
    }
    if (service != NULL) {
        CloseServiceHandle(service);
    }
    if (scm != NULL) {
        CloseServiceHandle(scm);
    }

    if (!started && !RunElevatedNet(L"start sshd")) {
        PostMessageW(g_wnd, WM_APP_LOG, kError, reinterpret_cast<LPARAM>(new std::wstring(L"서버 기동 취소 또는 실패")));
        PostMessageW(g_wnd, WM_APP_LOG, 1, 0);
        return 0;
    }

    if (WaitForSshdState(SERVICE_RUNNING, 15000)) {
        PostMessageW(g_wnd, WM_APP_LOG, kOk, reinterpret_cast<LPARAM>(new std::wstring(L"서버 기동 완료   127.0.0.1:22")));
    } else {
        PostMessageW(g_wnd, WM_APP_LOG, kError, reinterpret_cast<LPARAM>(new std::wstring(L"서버 기동 실패   sshd 상태를 확인하세요")));
    }
    PostMessageW(g_wnd, WM_APP_LOG, 1, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// 기능: 진행 중인 파일 전송을 끊고 sshd 서비스를 멈춘다.
// 역할: 서버종료 버튼 처리. sftp-server.exe 를 끝내 파일 데이터만 막고, 새 접속은 서비스 종료로 막는다.
// ---------------------------------------------------------------------------
DWORD WINAPI StopServerThread(LPVOID)
{
    const DWORD servicePid = QuerySshdPid();
    const bool hadTransfer = CountProcesses(L"sftp-server.exe", 0) > 0;
    bool transferGone = TerminateProcessesByName(L"sftp-server.exe", 0) &&
                        TerminateProcessesByName(L"sshd.exe", servicePid);

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    SC_HANDLE service = NULL;
    if (scm != NULL) {
        service = OpenServiceW(scm, L"sshd", SERVICE_STOP | SERVICE_QUERY_STATUS);
    }
    BOOL accepted = FALSE;
    if (service != NULL) {
        SERVICE_STATUS status;
        ZeroMemory(&status, sizeof(status));
        if (ControlService(service, SERVICE_CONTROL_STOP, &status)) {
            accepted = TRUE;
        } else if (GetLastError() == ERROR_SERVICE_NOT_ACTIVE) {
            accepted = TRUE;
        }
        CloseServiceHandle(service);
    }
    if (scm != NULL) {
        CloseServiceHandle(scm);
    }

    if (!transferGone || !accepted) {
        if (!RunElevated(L"cmd.exe",
                         L"/c \"taskkill /F /IM sftp-server.exe & net stop sshd & taskkill /F /IM sshd.exe\"")) {
            PostMessageW(g_wnd, WM_APP_LOG, kError,
                         reinterpret_cast<LPARAM>(new std::wstring(L"서버 종료 취소 또는 실패")));
        }
    }

    const bool serviceStopped = WaitForSshdState(SERVICE_STOPPED, 15000);
    if (!WaitUntilProcessGone(L"sftp-server.exe", 0, 3000) ||
        !WaitUntilProcessGone(L"sshd.exe", 0, 3000)) {
        TerminateProcessesByName(L"sftp-server.exe", 0);
        TerminateProcessesByName(L"sshd.exe", 0);
    }
    transferGone = CountProcesses(L"sftp-server.exe", 0) == 0 &&
                   CountProcesses(L"sshd.exe", 0) == 0;
    if (hadTransfer && transferGone) {
        PostMessageW(g_wnd, WM_APP_LOG, kOk,
                     reinterpret_cast<LPARAM>(new std::wstring(L"파일 전송 중단   sftp-server")));
    } else if (hadTransfer) {
        PostMessageW(g_wnd, WM_APP_LOG, kError,
                     reinterpret_cast<LPARAM>(new std::wstring(L"파일 전송 중단 실패   sftp-server")));
    }

    if (serviceStopped) {
        PostMessageW(g_wnd, WM_APP_LOG, kOk, reinterpret_cast<LPARAM>(new std::wstring(L"서버 종료 완료   sshd")));
    } else {
        PostMessageW(g_wnd, WM_APP_LOG, kError, reinterpret_cast<LPARAM>(new std::wstring(L"서버 종료 실패   sshd 상태를 확인하세요")));
    }
    PostMessageW(g_wnd, WM_APP_LOG, 1, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// 기능: SFTP 다운로드를 시작하고 로그 감시 타이머를 켠다.
// 역할: SFTP DOWN 버튼 처리.
// ---------------------------------------------------------------------------
void BeginDownload()
{
    SendMessageW(g_log, LB_RESETCONTENT, 0, 0);
    g_lines.clear();
    g_failed = false;
    g_logCarry.clear();
    g_fileDone = 0;
    g_fileCount = 1;
    g_itemPercent = 0;
    SetPercent(1);
    AddLog(L"SFTP 다운로드 시작", kInfo);
    MarkLogEnd();
    if (!StartDownload()) {
        g_itemPercent = 0;
        SetPercent(0);
        return;
    }
    SetBusy(true);
    SetTimer(g_wnd, kPollTimer, 200, NULL);
}

// ---------------------------------------------------------------------------
// 기능: 자식 프로세스 종료를 확인하고 마지막 로그를 반영한다.
// 역할: 타이머에서 다운로드가 끝났는지 본다.
// ---------------------------------------------------------------------------
void PollProcess()
{
    if (g_process == NULL) {
        return;
    }
    PollLog();
    if (WaitForSingleObject(g_process, 0) == WAIT_OBJECT_0) {
        PollLog();
        CloseHandle(g_process);
        g_process = NULL;
        KillTimer(g_wnd, kPollTimer);
        SetBusy(false);
    }
}

// ---------------------------------------------------------------------------
// 기능: 파란 상자를 짧은 라운드와 입체 음영으로 그린다.
// 역할: 바깥 테두리, 로그 영역, 진행도 영역의 공통 상자.
// ---------------------------------------------------------------------------
void DrawRaisedRoundBox(HDC dc, int left, int top, int right, int bottom)
{
    const int radius = 14;

    HBRUSH shadowBrush = CreateSolidBrush(RGB(168, 194, 216));
    HPEN shadowPen = CreatePen(PS_SOLID, 1, RGB(150, 178, 204));
    HGDIOBJ oldPen = SelectObject(dc, shadowPen);
    HGDIOBJ oldBrush = SelectObject(dc, shadowBrush);
    RoundRect(dc, left + 2, top + 3, right + 2, bottom + 3, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(shadowBrush);
    DeleteObject(shadowPen);

    HBRUSH face = CreateSolidBrush(RGB(255, 255, 255));
    HPEN border = CreatePen(PS_SOLID, 2, kFrame);
    oldPen = SelectObject(dc, border);
    oldBrush = SelectObject(dc, face);
    RoundRect(dc, left, top, right, bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(face);
    DeleteObject(border);

    HPEN highlight = CreatePen(PS_SOLID, 1, RGB(214, 232, 246));
    oldPen = SelectObject(dc, highlight);
    MoveToEx(dc, left + 10, bottom - 8, NULL);
    LineTo(dc, left + 10, top + 10);
    LineTo(dc, right - 10, top + 10);
    SelectObject(dc, oldPen);
    DeleteObject(highlight);

    HPEN shade = CreatePen(PS_SOLID, 1, RGB(118, 156, 190));
    oldPen = SelectObject(dc, shade);
    MoveToEx(dc, left + 12, bottom - 8, NULL);
    LineTo(dc, right - 10, bottom - 8);
    LineTo(dc, right - 10, top + 12);
    SelectObject(dc, oldPen);
    DeleteObject(shade);
}

// ---------------------------------------------------------------------------
// 기능: ATEC 빨간색 버튼을 짧은 라운드와 입체 음영으로 그린다.
// 역할: 버튼 면, 테두리, 글자를 마크의 붉은색 계열로 맞춘다.
// ---------------------------------------------------------------------------
void DrawBrandButton(const DRAWITEMSTRUCT* item)
{
    const bool disabled = (item->itemState & ODS_DISABLED) != 0;
    RECT rc = item->rcItem;
    const int radius = 10;

    HBRUSH plate = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(item->hDC, &rc, plate);
    DeleteObject(plate);

    const COLORREF faceColor = disabled ? RGB(245, 245, 245) : kBrandFill;
    const COLORREF lightColor = disabled ? RGB(230, 230, 230) : RGB(255, 176, 206);
    const COLORREF shadeColor = disabled ? RGB(180, 180, 180) : RGB(132, 0, 48);
    const COLORREF edgeColor = disabled ? kMuted : kBrand;

    HBRUSH shadeBrush = CreateSolidBrush(shadeColor);
    HPEN shadePen = CreatePen(PS_SOLID, 1, shadeColor);
    HGDIOBJ oldPen = SelectObject(item->hDC, shadePen);
    HGDIOBJ oldBrush = SelectObject(item->hDC, shadeBrush);
    RoundRect(item->hDC, rc.left, rc.top, rc.right, rc.bottom, radius, radius);

    HBRUSH lightBrush = CreateSolidBrush(lightColor);
    HPEN lightPen = CreatePen(PS_SOLID, 1, lightColor);
    SelectObject(item->hDC, lightPen);
    SelectObject(item->hDC, lightBrush);
    RoundRect(item->hDC, rc.left, rc.top, rc.right - 2, rc.bottom - 2, radius, radius);

    HBRUSH faceBrush = CreateSolidBrush(faceColor);
    HPEN facePen = CreatePen(PS_SOLID, 1, faceColor);
    SelectObject(item->hDC, facePen);
    SelectObject(item->hDC, faceBrush);
    RoundRect(item->hDC, rc.left + 2, rc.top + 2, rc.right - 2, rc.bottom - 2, radius, radius);

    HPEN border = CreatePen(PS_SOLID, 1, edgeColor);
    SelectObject(item->hDC, border);
    SelectObject(item->hDC, GetStockObject(HOLLOW_BRUSH));
    RoundRect(item->hDC, rc.left, rc.top, rc.right, rc.bottom, radius, radius);

    SelectObject(item->hDC, oldBrush);
    SelectObject(item->hDC, oldPen);
    DeleteObject(shadeBrush);
    DeleteObject(shadePen);
    DeleteObject(lightBrush);
    DeleteObject(lightPen);
    DeleteObject(faceBrush);
    DeleteObject(facePen);
    DeleteObject(border);

    wchar_t text[64] = {0};
    GetWindowTextW(item->hwndItem, text, 64);
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, disabled ? kMuted : kBrand);
    const bool exitButton = GetDlgCtrlID(item->hwndItem) == IDC_BTN_EXIT;
    SelectObject(item->hDC, exitButton ? g_fontExit : g_fontBold);
    DrawTextW(item->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// ---------------------------------------------------------------------------
// 기능: 로그 한 줄을 지정 색으로 그린다.
// 역할: 성공과 진행은 파랑, 실패는 빨강으로 보이게 한다.
// ---------------------------------------------------------------------------
void DrawLogItem(const DRAWITEMSTRUCT* item)
{
    COLORREF color = kInfo;
    if (item->itemData != 0) {
        color = static_cast<COLORREF>(item->itemData);
    }
    HBRUSH fill = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(item->hDC, &item->rcItem, fill);
    DeleteObject(fill);

    wchar_t text[512] = {0};
    SendMessageW(item->hwndItem, LB_GETTEXT, item->itemID, reinterpret_cast<LPARAM>(text));
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, color);
    SelectObject(item->hDC, g_font);
    RECT textRc = item->rcItem;
    textRc.left += 8;
    DrawTextW(item->hDC, text, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

// ---------------------------------------------------------------------------
// 기능: 진행 막대 하나를 그린다.
// 역할: 개별 진행도와 전체 진행도가 같은 막대 모양을 쓴다.
// ---------------------------------------------------------------------------
void DrawMeter(HDC dc, int left, int top, int right, int bottom, int percent)
{
    RECT bar = {left, top, right, bottom};
    HBRUSH track = CreateSolidBrush(RGB(226, 238, 248));
    FillRect(dc, &bar, track);
    DeleteObject(track);

    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    RECT fill = bar;
    fill.right = bar.left + ((bar.right - bar.left) * percent / 100);
    if (fill.right > fill.left) {
        HBRUSH done = CreateSolidBrush(g_failed ? kError : kOk);
        FillRect(dc, &fill, done);
        DeleteObject(done);
    }

    HPEN barPen = CreatePen(PS_SOLID, 1, kFrame);
    HGDIOBJ oldPen = SelectObject(dc, barPen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, bar.left, bar.top, bar.right, bar.bottom);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(barPen);
}

// ---------------------------------------------------------------------------
// 기능: 발표 창의 고정 문구, 로그 테두리, 진행 막대를 그린다.
// 역할: 제목, 로그 상자, 개별/전체 진행도 상자를 그리고 종료 버튼 자리를 비워 둔다.
// ---------------------------------------------------------------------------
void PaintWindow(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);

    RECT client;
    GetClientRect(hwnd, &client);
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &client, white);
    DeleteObject(white);

    DrawRaisedRoundBox(dc, 10, 10, client.right - 12, client.bottom - 12);
    if (g_serverApp) {
        DrawRaisedRoundBox(dc, 22, 70, client.right - 22, 578);
    } else {
        DrawRaisedRoundBox(dc, 22, 70, client.right - 22, 454);
        DrawRaisedRoundBox(dc, 22, 470, client.right - 22, 578);

        SelectObject(dc, g_font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_failed ? kError : kTitle);
        wchar_t label[64];
        wsprintfW(label, L"개별 진행도 : %d%%", g_itemPercent);
        TextOutW(dc, 32, 482, label, lstrlenW(label));
        wsprintfW(label, L"전체 진행도 : %d%%", g_percent);
        TextOutW(dc, 32, 528, label, lstrlenW(label));
    }

    if (g_logo != NULL) {
        const int logoW = 154;
        const int logoH = 40;
        RECT plate = {22, 14, 22 + logoW, 14 + logoH};
        HBRUSH plateBrush = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(dc, &plate, plateBrush);
        DeleteObject(plateBrush);
        Gdiplus::Graphics graphics(dc);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.DrawImage(g_logo, plate.left, plate.top, logoW, logoH);
    }

    if (!g_serverApp) {
        DrawMeter(dc, 32, 502, client.right - 32, 518, g_itemPercent);
        DrawMeter(dc, 32, 548, client.right - 32, 564, g_percent);
    }

    EndPaint(hwnd, &ps);
}

const DWORD kPopupStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;

// ---------------------------------------------------------------------------
// 기능: 와이드 문자열을 시스템 ANSI로 바꾼다.
// 역할: config_ser.ini 에 User, Password, 지문을 기록할 때 사용한다.
// ---------------------------------------------------------------------------
std::string NarrowAcp(const std::wstring& text)
{
    if (text.empty()) {
        return std::string();
    }
    const int need = WideCharToMultiByte(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()),
                                         NULL, 0, NULL, NULL);
    if (need <= 0) {
        return std::string();
    }
    std::string out(need, '\0');
    WideCharToMultiByte(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()),
                        &out[0], need, NULL, NULL);
    return out;
}

// ---------------------------------------------------------------------------
// 기능: 키 이름이 같은지 대소문자 없이 비교한다.
// 역할: config_ser.ini 의 User, Password, HostFingerprint 줄을 찾는다.
// ---------------------------------------------------------------------------
bool KeyEquals(const std::string& left, const std::string& right)
{
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        char a = left[i];
        char b = right[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 기능: 설정 파일 전체를 읽는다.
// 역할: exe 옆 config_ser.ini 를 수정하기 전에 원문을 가져온다.
// ---------------------------------------------------------------------------
bool ReadWholeFile(const std::wstring& path, std::string& text)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        text.clear();
        return false;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return false;
    }
    text.assign(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = size.QuadPart == 0 ||
                    ReadFile(file, &text[0], static_cast<DWORD>(size.QuadPart), &read, NULL);
    CloseHandle(file);
    if (!ok) {
        return false;
    }
    text.resize(read);
    return true;
}

// ---------------------------------------------------------------------------
// 기능: 설정 파일 전체를 덮어쓴다.
// 역할: 바꾼 User, Password, HostFingerprint 를 디스크에 남긴다.
// ---------------------------------------------------------------------------
bool WriteWholeFile(const std::wstring& path, const std::string& text)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = text.empty() ||
                    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, NULL);
    CloseHandle(file);
    return ok && written == text.size();
}

// ---------------------------------------------------------------------------
// 기능: config_ser.ini 에서 키의 값을 읽는다.
// 역할: 입력 창과 지문 창에 현재 서버 설정을 보여 준다.
// ---------------------------------------------------------------------------
std::wstring ReadConfigValue(const char* key)
{
    std::string text;
    if (!ReadWholeFile(g_exeDir + L"\\config_ser.ini", text)) {
        return std::wstring();
    }
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        size_t contentEnd = end;
        if (contentEnd > pos && text[contentEnd - 1] == '\r') {
            --contentEnd;
        }
        const std::string line = text.substr(pos, contentEnd - pos);
        const size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string name = line.substr(0, eq);
            while (!name.empty() && (name[0] == ' ' || name[0] == '\t')) {
                name.erase(name.begin());
            }
            if (KeyEquals(name, key)) {
                return Widen(line.substr(eq + 1));
            }
        }
        if (end >= text.size()) {
            break;
        }
        pos = end + 1;
    }
    return std::wstring();
}

// ---------------------------------------------------------------------------
// 기능: config_ser.ini 의 키 한 줄을 바꾸거나 추가한다.
// 역할: 서버 설정 값을 저장한다. config.ini 는 변경하지 않는다.
// ---------------------------------------------------------------------------
bool WriteConfigValue(const char* key, const std::wstring& value)
{
    const std::string narrow = NarrowAcp(value);
    std::string text;
    ReadWholeFile(g_exeDir + L"\\config_ser.ini", text);
    size_t pos = 0;
    bool found = false;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        size_t contentEnd = end;
        if (contentEnd > pos && text[contentEnd - 1] == '\r') {
            --contentEnd;
        }
        const std::string line = text.substr(pos, contentEnd - pos);
        const size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string name = line.substr(0, eq);
            while (!name.empty() && (name[0] == ' ' || name[0] == '\t')) {
                name.erase(name.begin());
            }
            if (KeyEquals(name, key)) {
                text.replace(pos, contentEnd - pos, std::string(key) + "=" + narrow);
                found = true;
                break;
            }
        }
        if (end >= text.size()) {
            break;
        }
        pos = end + 1;
    }
    if (!found) {
        if (!text.empty() && text[text.size() - 1] != '\n') {
            text += "\r\n";
        }
        text += std::string(key) + "=" + narrow + "\r\n";
    }
    return WriteWholeFile(g_exeDir + L"\\config_ser.ini", text);
}

// ---------------------------------------------------------------------------
// 기능: 프로그램을 숨김 실행하고 종료 코드를 받는다.
// 역할: net user 와 지문 생성 스크립트를 일반 권한 또는 관리자 권한으로 돌린다.
// ---------------------------------------------------------------------------
bool RunProcess(const wchar_t* file, const std::wstring& params, bool elevate, DWORD timeoutMs, DWORD& exitCode)
{
    exitCode = 1;
    if (!elevate) {
        std::wstring cmd = std::wstring(file) + L" " + params;
        std::vector<wchar_t> buffer(cmd.begin(), cmd.end());
        buffer.push_back(L'\0');
        STARTUPINFOW si;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi;
        ZeroMemory(&pi, sizeof(pi));
        if (!CreateProcessW(NULL, &buffer[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            return false;
        }
        WaitForSingleObject(pi.hProcess, timeoutMs);
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return exitCode != STILL_ACTIVE;
    }

    SHELLEXECUTEINFOW info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = file;
    info.lpParameters = params.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        return false;
    }
    if (info.hProcess != NULL) {
        WaitForSingleObject(info.hProcess, timeoutMs);
        GetExitCodeProcess(info.hProcess, &exitCode);
        CloseHandle(info.hProcess);
    }
    return exitCode != STILL_ACTIVE;
}

// ---------------------------------------------------------------------------
// 기능: 창을 부모 창 가운데로 옮긴다.
// 역할: 서버 설정 팝업이 발표 창 위에 보이게 한다.
// ---------------------------------------------------------------------------
void CenterOnParent(HWND hwnd, HWND parent)
{
    RECT rc;
    RECT parentRc;
    GetWindowRect(hwnd, &rc);
    GetWindowRect(parent, &parentRc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int x = parentRc.left + ((parentRc.right - parentRc.left) - width) / 2;
    const int y = parentRc.top + ((parentRc.bottom - parentRc.top) - height) / 2;
    SetWindowPos(hwnd, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
}

// ---------------------------------------------------------------------------
// 기능: 입력값에 줄바꿈이나 따옴표가 있는지 본다.
// 역할: 설정 파일과 서버 계정 명령을 깨뜨리는 문자를 거른다.
// ---------------------------------------------------------------------------
bool HasUnsafeSettingChar(const std::wstring& text)
{
    return text.find_first_of(L"\r\n\"") != std::wstring::npos;
}

// ---------------------------------------------------------------------------
// 기능: net user 출력을 읽고 성공 여부를 돌려준다.
// 역할: 계정 반영 실패 사유를 화면 로그에 남긴다.
// ---------------------------------------------------------------------------
bool RunNet(const std::wstring& args, bool elevate, std::wstring& detail)
{
    const std::wstring outPath = g_exeDir + L"\\net_user_out.txt";
    DeleteFileW(outPath.c_str());
    const std::wstring params = L"/c net " + args + L" > \"" + outPath + L"\" 2>&1";
    DWORD code = 1;
    const bool ok = RunProcess(L"cmd", params, elevate, elevate ? 30000 : 20000, code) && code == 0;
    std::string text;
    if (ReadWholeFile(outPath, text)) {
        const size_t lineEnd = text.find_first_of("\r\n");
        detail = Widen(lineEnd == std::string::npos ? text : text.substr(0, lineEnd));
    }
    DeleteFileW(outPath.c_str());
    return ok;
}

// ---------------------------------------------------------------------------
// 기능: Windows 계정이 없으면 만들고, 있으면 비밀번호를 바꾼다.
// 역할: sshd 가 받아들이는 서버 계정 비밀번호를 입력값과 맞춘다.
// ---------------------------------------------------------------------------
bool ApplyAccountPassword(const std::wstring& user, const std::wstring& password, std::wstring& detail)
{
    detail.clear();
    std::wstring ignored;
    const bool exists = RunNet(L"user \"" + user + L"\"", false, ignored);
    std::wstring args = L"user \"" + user + L"\" \"" + password + L"\"";
    if (!exists) {
        args += L" /add";
        AddLog(L"서버 계정 없음   새로 만듭니다   " + user, kInfo);
    }
    if (!RunNet(args, false, detail) && !RunNet(args, true, detail)) {
        return false;
    }
    if (!exists) {
        wchar_t systemDir[MAX_PATH] = {0};
        GetSystemDirectoryW(systemDir, MAX_PATH);
        const std::wstring powershell = std::wstring(systemDir) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        DWORD code = 1;
        const std::wstring expireArgs =
            L"-NoProfile -Command \"Set-LocalUser -Name '" + user + L"' -PasswordNeverExpires $true\"";
        if (!(RunProcess(powershell.c_str(), expireArgs, false, 20000, code) && code == 0)) {
            RunProcess(powershell.c_str(), expireArgs, true, 30000, code);
        }
        if (GetFileAttributesW(L"C:\\data") != INVALID_FILE_ATTRIBUTES) {
            std::wstring grant = L"/c icacls C:\\data /grant \"" + user + L":(OI)(CI)RX\"";
            DWORD grantCode = 1;
            if (!(RunProcess(L"cmd", grant, false, 20000, grantCode) && grantCode == 0)) {
                RunProcess(L"cmd", grant, true, 30000, grantCode);
            }
        }
    }
    detail.clear();
    return true;
}

// ---------------------------------------------------------------------------
// 기능: 서버 Windows 계정 이름을 새 User 로 바꾼다.
// 역할: 계정 이름이 바뀐 뒤에만 config_ser.ini 의 User 를 저장하게 한다.
// ---------------------------------------------------------------------------
bool ApplyAccountUser(const std::wstring& newUser)
{
    const std::wstring current = ReadConfigValue("User");
    if (!current.empty() && CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE,
                                           current.c_str(), -1, newUser.c_str(), -1) == CSTR_EQUAL) {
        return true;
    }

    DWORD code = 1;
    if (current.empty()) {
        const std::wstring params = L"user \"" + newUser + L"\"";
        if (RunProcess(L"net", params, false, 20000, code) && code == 0) {
            return true;
        }
        return RunProcess(L"net", params, true, 30000, code) && code == 0;
    }

    std::wstring escapedCurrent;
    std::wstring escapedNew;
    for (size_t i = 0; i < current.size(); ++i) {
        if (current[i] == L'\'') {
            escapedCurrent += L"''";
        } else {
            escapedCurrent += current[i];
        }
    }
    for (size_t i = 0; i < newUser.size(); ++i) {
        if (newUser[i] == L'\'') {
            escapedNew += L"''";
        } else {
            escapedNew += newUser[i];
        }
    }

    const std::wstring scriptPath = g_exeDir + L"\\rename_user.ps1";
    std::string script;
    script += "Rename-LocalUser -Name '";
    script += NarrowAcp(escapedCurrent);
    script += "' -NewName '";
    script += NarrowAcp(escapedNew);
    script += "'\r\n";
    script += "if (-not $?) { exit 1 }\r\n";
    if (!WriteWholeFile(scriptPath, script)) {
        return false;
    }

    wchar_t systemDir[MAX_PATH] = {0};
    GetSystemDirectoryW(systemDir, MAX_PATH);
    const std::wstring powershell = std::wstring(systemDir) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    const std::wstring params = L"-NoProfile -ExecutionPolicy Bypass -File \"" + scriptPath + L"\"";
    bool ok = RunProcess(powershell.c_str(), params, false, 20000, code) && code == 0;
    if (!ok) {
        ok = RunProcess(powershell.c_str(), params, true, 30000, code) && code == 0;
    }
    DeleteFileW(scriptPath.c_str());
    return ok;
}

// ---------------------------------------------------------------------------
// 기능: RSA 호스트 키를 새로 만들고 SHA256 지문을 돌려준다.
// 역할: HostFingerprint 생성 버튼이 서버 지문을 바꾸고 파일로 받는다.
// ---------------------------------------------------------------------------
bool GenerateHostFingerprint(std::string& fingerprint, std::wstring& error)
{
    std::wstring keygen = L"C:\\OpenSSH\\ssh-keygen.exe";
    if (GetFileAttributesW(keygen.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wchar_t systemDir[MAX_PATH] = {0};
        GetSystemDirectoryW(systemDir, MAX_PATH);
        keygen = std::wstring(systemDir) + L"\\OpenSSH\\ssh-keygen.exe";
    }
    if (GetFileAttributesW(keygen.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = L"ssh-keygen.exe 를 찾지 못했습니다.";
        return false;
    }

    const std::wstring keyPath = g_exeDir + L"\\tmp_host_rsa";
    const std::wstring pubPath = keyPath + L".pub";
    const std::wstring fpPath = g_exeDir + L"\\hostkey_new.txt";
    const std::wstring batPath = g_exeDir + L"\\generate_hostkey.bat";
    DeleteFileW(keyPath.c_str());
    DeleteFileW(pubPath.c_str());
    DeleteFileW(fpPath.c_str());

    std::string bat;
    bat += "@echo off\r\n";
    bat += "\"" + NarrowAcp(keygen) + "\" -q -t rsa -b 3072 -f \"" + NarrowAcp(keyPath) + "\" -N \"\"\r\n";
    bat += "if errorlevel 1 exit /b 1\r\n";
    bat += "\"" + NarrowAcp(keygen) + "\" -lf \"" + NarrowAcp(pubPath) + "\" -E sha256 > \"" + NarrowAcp(fpPath) + "\"\r\n";
    if (!WriteWholeFile(batPath, bat)) {
        error = L"키 생성 스크립트를 만들지 못했습니다.";
        return false;
    }

    DWORD code = 1;
    const bool generated = RunProcess(L"cmd", L"/c \"" + batPath + L"\"", false, 30000, code);
    DeleteFileW(batPath.c_str());
    std::string text;
    const bool haveText = ReadWholeFile(fpPath, text);
    DeleteFileW(fpPath.c_str());
    if (!generated || code != 0 || !haveText) {
        DeleteFileW(keyPath.c_str());
        DeleteFileW(pubPath.c_str());
        error = L"새 호스트 키를 만들지 못했습니다.";
        return false;
    }
    const size_t mark = text.find("SHA256:");
    if (mark == std::string::npos) {
        DeleteFileW(keyPath.c_str());
        DeleteFileW(pubPath.c_str());
        error = L"새 지문 형식을 확인하지 못했습니다.";
        return false;
    }
    size_t end = text.find_first_of(" \r\n", mark);
    if (end == std::string::npos) {
        end = text.size();
    }
    fingerprint = text.substr(mark, end - mark);

    const bool restart = QuerySshdState() == SERVICE_RUNNING;
    const std::wstring scriptPath = g_exeDir + L"\\install_hostkey.ps1";
    std::string script;
    script += "$ErrorActionPreference = 'Continue'\r\n";
    script += "Copy-Item -LiteralPath '" + NarrowAcp(keyPath) + "' -Destination 'C:\\ProgramData\\ssh\\ssh_host_rsa_key' -Force\r\n";
    script += "Copy-Item -LiteralPath '" + NarrowAcp(pubPath) + "' -Destination 'C:\\ProgramData\\ssh\\ssh_host_rsa_key.pub' -Force\r\n";
    script += "icacls 'C:\\ProgramData\\ssh\\ssh_host_rsa_key' /inheritance:r /grant *S-1-5-32-544:F /grant *S-1-5-18:F | Out-Null\r\n";
    script += "icacls 'C:\\ProgramData\\ssh\\ssh_host_rsa_key.pub' /inheritance:r /grant *S-1-5-32-544:F /grant *S-1-5-18:F | Out-Null\r\n";
    script += "if (-not (Test-Path -LiteralPath 'C:\\ProgramData\\ssh\\ssh_host_rsa_key.pub')) { exit 1 }\r\n";
    if (restart) {
        script += "try { Restart-Service -Name sshd -Force } catch { exit 2 }\r\n";
    }
    if (!WriteWholeFile(scriptPath, script)) {
        DeleteFileW(keyPath.c_str());
        DeleteFileW(pubPath.c_str());
        error = L"서버 키 설치 스크립트를 만들지 못했습니다.";
        return false;
    }

    wchar_t systemDir[MAX_PATH] = {0};
    GetSystemDirectoryW(systemDir, MAX_PATH);
    const std::wstring powershell = std::wstring(systemDir) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    const bool installed = RunProcess(powershell.c_str(),
                                      L"-NoProfile -ExecutionPolicy Bypass -File \"" + scriptPath + L"\"",
                                      true, 60000, code);
    DeleteFileW(scriptPath.c_str());
    DeleteFileW(keyPath.c_str());
    DeleteFileW(pubPath.c_str());
    if (!installed || code != 0) {
        error = L"새 키를 서버에 설치하지 못했습니다.";
        return false;
    }
    return !fingerprint.empty();
}

// ---------------------------------------------------------------------------
// 기능: 호스트 키를 만들고 결과를 발표 창으로 보낸다.
// 역할: 지문 생성 동안 설정 창의 X 버튼이 멈추지 않게 한다.
// ---------------------------------------------------------------------------
DWORD WINAPI GenerateFingerprintThread(LPVOID)
{
    std::string fingerprint;
    std::wstring error;
    if (GenerateHostFingerprint(fingerprint, error)) {
        PostMessageW(g_wnd, WM_APP_FP, 1, reinterpret_cast<LPARAM>(new std::wstring(Widen(fingerprint))));
    } else {
        PostMessageW(g_wnd, WM_APP_FP, 0, reinterpret_cast<LPARAM>(new std::wstring(error)));
    }
    return 0;
}

LRESULT CALLBACK SetupProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK InputProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK FingerprintProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void OpenUserDialog();
void OpenPasswordDialog();
void OpenFingerprintDialog();

// ---------------------------------------------------------------------------
// 기능: 팝업 창 클래스를 한 번만 등록한다.
// 역할: 서버 설정, 입력, 지문 창이 제목 표시줄의 X 로 닫히게 한다.
// ---------------------------------------------------------------------------
void EnsurePopupClasses()
{
    if (g_popupClassReady) {
        return;
    }
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    wc.lpfnWndProc = SetupProc;
    wc.lpszClassName = L"AtecSetupPopup";
    RegisterClassW(&wc);
    wc.lpfnWndProc = InputProc;
    wc.lpszClassName = L"AtecInputPopup";
    RegisterClassW(&wc);
    wc.lpfnWndProc = FingerprintProc;
    wc.lpszClassName = L"AtecFingerprintPopup";
    RegisterClassW(&wc);
    g_popupClassReady = true;
}

// ---------------------------------------------------------------------------
// 기능: 지정한 크기의 팝업을 만들고 부모 가운데에 둔다.
// 역할: 서버 설정 계열 창의 공통 생성.
// ---------------------------------------------------------------------------
HWND OpenPopup(const wchar_t* className, const wchar_t* title, int width, int height, LPVOID param)
{
    RECT rect = {0, 0, width, height};
    AdjustWindowRect(&rect, kPopupStyle, FALSE);
    HWND hwnd = CreateWindowW(className, title, kPopupStyle,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              rect.right - rect.left, rect.bottom - rect.top,
                              g_wnd, NULL, g_inst, param);
    if (hwnd != NULL) {
        CenterOnParent(hwnd, g_wnd);
        SetForegroundWindow(hwnd);
    }
    return hwnd;
}

// ---------------------------------------------------------------------------
// 기능: 서버 설정 팝업 메시지를 처리한다.
// 역할: User, Password, HostFingerprint 창을 열고 X 로 닫는다.
// ---------------------------------------------------------------------------
LRESULT CALLBACK SetupProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        CreateWindowW(L"BUTTON", L"User 설정",
                      WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                      24, 20, 252, 36, hwnd,
                      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_SETUP_USER)), g_inst, NULL);
        CreateWindowW(L"BUTTON", L"Password 설정",
                      WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                      24, 68, 252, 36, hwnd,
                      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_SETUP_PASS)), g_inst, NULL);
        CreateWindowW(L"BUTTON", L"HostFingerprint 설정",
                      WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                      24, 116, 252, 36, hwnd,
                      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_SETUP_FP)), g_inst, NULL);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_SETUP_USER:
            OpenUserDialog();
            return 0;
        case IDC_SETUP_PASS:
            OpenPasswordDialog();
            return 0;
        case IDC_SETUP_FP:
            OpenFingerprintDialog();
            return 0;
        default:
            break;
        }
        break;
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item->CtlType == ODT_BUTTON) {
            DrawBrandButton(item);
        }
        return TRUE;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_setupWnd == hwnd) {
            g_setupWnd = NULL;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 기능: User 또는 Password 입력 창 메시지를 처리한다.
// 역할: 서버 계정 반영이 성공한 뒤에만 config_ser.ini 에 기록한다.
// ---------------------------------------------------------------------------
LRESULT CALLBACK InputProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        const CREATESTRUCTW* created = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        const bool password = created->lpCreateParams != NULL;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, password ? 1 : 0);
        const std::wstring current = password ? ReadConfigValue("Password") : ReadConfigValue("User");
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", current.c_str(),
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | (password ? ES_PASSWORD : 0),
                                    20, 48, 380, 28, hwnd,
                                    reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_INPUT_EDIT)), g_inst, NULL);
        SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        CreateWindowW(L"BUTTON", L"저장",
                      WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                      280, 96, 120, 36, hwnd,
                      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_INPUT_SAVE)), g_inst, NULL);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_INPUT_SAVE && HIWORD(wParam) == BN_CLICKED) {
            const bool password = GetWindowLongPtrW(hwnd, GWLP_USERDATA) != 0;
            wchar_t buffer[256] = {0};
            GetWindowTextW(GetDlgItem(hwnd, IDC_INPUT_EDIT), buffer, 256);
            const std::wstring value(buffer);
            if (value.empty() || HasUnsafeSettingChar(value)) {
                AddLog(password ? L"Password 를 확인하세요." : L"User 를 확인하세요.", kError);
                return 0;
            }
            if (password) {
                const std::wstring user = ReadConfigValue("User");
                std::wstring detail;
                if (user.empty() || !ApplyAccountPassword(user, value, detail)) {
                    AddLog(detail.empty() ? L"Password 서버 계정 반영 실패"
                                          : L"Password 서버 계정 반영 실패   " + detail, kError);
                    return 0;
                }
                if (!WriteConfigValue("Password", value)) {
                    AddLog(L"Password 는 서버 계정에 반영됨. config_ser.ini 저장 실패", kError);
                    return 0;
                }
                AddLog(L"Password 저장   계정 " + user + L"   config_ser.ini", kOk);
            } else {
                if (!ApplyAccountUser(value)) {
                    AddLog(L"User 서버 계정 반영 실패", kError);
                    return 0;
                }
                if (!WriteConfigValue("User", value)) {
                    AddLog(L"User 는 서버 계정에 반영됨. config_ser.ini 저장 실패", kError);
                    return 0;
                }
                AddLog(L"User 저장   config_ser.ini   " + value, kOk);
            }
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kTitle);
        SelectObject(dc, g_font);
        const bool password = GetWindowLongPtrW(hwnd, GWLP_USERDATA) != 0;
        const wchar_t* label = password ? L"Password" : L"User";
        TextOutW(dc, 20, 18, label, lstrlenW(label));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item->CtlType == ODT_BUTTON) {
            DrawBrandButton(item);
        }
        return TRUE;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (GetWindowLongPtrW(hwnd, GWLP_USERDATA) != 0) {
            if (g_passWnd == hwnd) {
                g_passWnd = NULL;
            }
        } else if (g_userWnd == hwnd) {
            g_userWnd = NULL;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 기능: 지문 창 메시지를 처리한다.
// 역할: 현재 지문을 보여 주고 생성 버튼으로 새 지문 작업을 시작한다.
// ---------------------------------------------------------------------------
LRESULT CALLBACK FingerprintProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE: {
        const std::wstring current = ReadConfigValue("HostFingerprint");
        g_fpEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", current.c_str(),
                                   WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                                   20, 48, 500, 28, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_FP_EDIT)), g_inst, NULL);
        SendMessageW(g_fpEdit, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        g_fpButton = CreateWindowW(L"BUTTON", L"생성",
                                   WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                   20, 100, 120, 36, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_FP_MAKE)), g_inst, NULL);
        if (g_fpBusy) {
            EnableWindow(g_fpButton, FALSE);
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_FP_MAKE && HIWORD(wParam) == BN_CLICKED && !g_fpBusy) {
            g_fpBusy = true;
            EnableWindow(g_fpButton, FALSE);
            AddLog(L"HostFingerprint 생성 요청", kInfo);
            HANDLE thread = CreateThread(NULL, 0, GenerateFingerprintThread, NULL, 0, NULL);
            if (thread == NULL) {
                g_fpBusy = false;
                EnableWindow(g_fpButton, TRUE);
                AddLog(L"HostFingerprint 생성을 시작하지 못했습니다.", kError);
            } else {
                CloseHandle(thread);
            }
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kTitle);
        SelectObject(dc, g_font);
        const wchar_t* label = L"현재 HostFingerprint";
        TextOutW(dc, 20, 18, label, lstrlenW(label));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item->CtlType == ODT_BUTTON) {
            DrawBrandButton(item);
        }
        return TRUE;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_fpWnd == hwnd) {
            g_fpWnd = NULL;
            g_fpEdit = NULL;
            g_fpButton = NULL;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 기능: 서버 설정 팝업을 연다.
// 역할: 종료 버튼 옆 서버 설정 버튼의 첫 화면.
// ---------------------------------------------------------------------------
void OpenSetupDialog()
{
    if (g_setupWnd != NULL) {
        SetForegroundWindow(g_setupWnd);
        return;
    }
    EnsurePopupClasses();
    g_setupWnd = OpenPopup(L"AtecSetupPopup", L"서버 설정", 300, 176, NULL);
}

// ---------------------------------------------------------------------------
// 기능: User 입력 창을 연다.
// 역할: 현재 사용자를 보여 주고 저장하면 설정 파일에 남긴다.
// ---------------------------------------------------------------------------
void OpenUserDialog()
{
    if (g_userWnd != NULL) {
        SetForegroundWindow(g_userWnd);
        return;
    }
    EnsurePopupClasses();
    g_userWnd = OpenPopup(L"AtecInputPopup", L"User 설정", 420, 156, NULL);
}

// ---------------------------------------------------------------------------
// 기능: Password 입력 창을 연다.
// 역할: 비밀번호를 받아 설정 파일과 서버 계정에 저장한다.
// ---------------------------------------------------------------------------
void OpenPasswordDialog()
{
    if (g_passWnd != NULL) {
        SetForegroundWindow(g_passWnd);
        return;
    }
    EnsurePopupClasses();
    g_passWnd = OpenPopup(L"AtecInputPopup", L"Password 설정", 420, 156, reinterpret_cast<LPVOID>(1));
}

// ---------------------------------------------------------------------------
// 기능: HostFingerprint 창을 연다.
// 역할: 현재 지문과 생성 버튼을 보여 준다.
// ---------------------------------------------------------------------------
void OpenFingerprintDialog()
{
    if (g_fpWnd != NULL) {
        SetForegroundWindow(g_fpWnd);
        return;
    }
    EnsurePopupClasses();
    g_fpWnd = OpenPopup(L"AtecFingerprintPopup", L"HostFingerprint 설정", 540, 160, NULL);
}

// ---------------------------------------------------------------------------
// 기능: 제목, 버튼, 로그 목록을 배치한다.
// 역할: 스케치의 조작 버튼과 정보 영역을 만든다.
// ---------------------------------------------------------------------------
void CreateControls(HWND hwnd)
{
    if (g_serverApp) {
        g_btnServer = CreateWindowW(L"BUTTON", L"서버기동",
                                    WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                    602, 22, 120, 36, hwnd,
                                    reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_BTN_SERVER)), g_inst, NULL);
        g_btnSetup = CreateWindowW(L"BUTTON", L"서버 설정",
                                   WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                   434, 602, 156, 47, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_BTN_SETUP)), g_inst, NULL);
    } else {
        g_btnDown = CreateWindowW(L"BUTTON", L"SFTP DOWN",
                                  WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                  470, 22, 120, 36, hwnd,
                                  reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_BTN_DOWN)), g_inst, NULL);
    }
    const int logHeight = g_serverApp ? 484 : 360;
    g_log = CreateWindowExW(0, L"LISTBOX", L"",
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                            34, 82, 732, logHeight, hwnd,
                            reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_LOG)), g_inst, NULL);
    SendMessageW(g_log, LB_SETITEMHEIGHT, 0, 24);
    g_btnExit = CreateWindowW(L"BUTTON", L"종료",
                              WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                              602, 602, 156, 47, hwnd,
                              reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_BTN_EXIT)), g_inst, NULL);
    SendMessageW(g_log, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
}

// ---------------------------------------------------------------------------
// 기능: 발표 창 메시지를 처리한다.
// 역할: 버튼, 타이머, 색 로그, 진행 막대, 종료를 연결한다.
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        g_wnd = hwnd;
        CreateControls(hwnd);
        if (g_serverApp) {
            UpdateServerButton();
            if (g_serverRunning) {
                AddLog(L"서버 실행 중   서버종료로 멈출 수 있습니다.", kOk);
            } else {
                AddLog(L"서버기동 또는 서버 설정을 누르세요.", kMuted);
            }
        } else {
            BeginDownload();
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_DOWN:
            return 0;
        case IDC_BTN_SERVER:
            if (!g_busy) {
                const bool stop = g_serverRunning;
                AddLog(stop ? L"서버 종료 요청   sshd" : L"서버 기동 요청   sshd", kInfo);
                SetBusy(true);
                HANDLE thread = CreateThread(NULL, 0, stop ? StopServerThread : StartServerThread, NULL, 0, NULL);
                if (thread != NULL) {
                    CloseHandle(thread);
                }
            }
            return 0;
        case IDC_BTN_SETUP:
            OpenSetupDialog();
            return 0;
        case IDC_BTN_EXIT:
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;
    case WM_APP_FP: {
        std::wstring* text = reinterpret_cast<std::wstring*>(lParam);
        g_fpBusy = false;
        if (g_fpButton != NULL) {
            EnableWindow(g_fpButton, TRUE);
        }
        if (wParam == 1 && text != NULL) {
            if (WriteConfigValue("HostFingerprint", *text)) {
                if (g_fpEdit != NULL) {
                    SetWindowTextW(g_fpEdit, text->c_str());
                }
                AddLog(L"HostFingerprint 생성 및 저장   config_ser.ini   " + *text, kOk);
                UpdateServerButton();
            } else {
                AddLog(L"새 지문을 만들었지만 저장에 실패했습니다.", kError);
            }
        } else if (text != NULL) {
            AddLog(*text, kError);
        }
        delete text;
        return 0;
    }
    case WM_APP_LOG:
        if (lParam != 0) {
            std::wstring* text = reinterpret_cast<std::wstring*>(lParam);
            AddLog(*text, static_cast<COLORREF>(wParam));
            delete text;
        } else {
            UpdateServerButton();
            SetBusy(false);
        }
        return 0;
    case WM_TIMER:
        PollProcess();
        return 0;
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item->CtlType == ODT_BUTTON) {
            DrawBrandButton(item);
        } else if (item->CtlType == ODT_LISTBOX) {
            DrawLogItem(item);
        }
        return TRUE;
    }
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        measure->itemHeight = 24;
        return TRUE;
    }
    case WM_PAINT:
        PaintWindow(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        if (g_process != NULL) {
            CloseHandle(g_process);
            g_process = NULL;
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

// ---------------------------------------------------------------------------
// 기능: 실행 파일 이름이 sft_ser.exe 인지 확인한다.
// 역할: 같은 화면 코드로 서버 창과 클라 창을 가른다.
// ---------------------------------------------------------------------------
bool IsServerExecutable()
{
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, path, MAX_PATH);
    const wchar_t* name = path;
    for (const wchar_t* cursor = path; *cursor != L'\0'; ++cursor) {
        if (*cursor == L'\\' || *cursor == L'/') {
            name = cursor + 1;
        }
    }
    return lstrcmpiW(name, L"sft_ser.exe") == 0;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show)
{
    g_inst = instance;
    g_exeDir = ExeDirectory();
    g_serverApp = IsServerExecutable();

    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, NULL);
    const std::wstring logoPath = g_exeDir + L"\\atec_logo.png";
    g_logo = Gdiplus::Image::FromFile(logoPath.c_str());
    if (g_logo != NULL && g_logo->GetLastStatus() != Gdiplus::Ok) {
        delete g_logo;
        g_logo = NULL;
    }
    g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, HANGUL_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Malgun Gothic");
    g_fontBold = CreateFontW(-18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, HANGUL_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Malgun Gothic");
    g_fontExit = CreateFontW(-22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, HANGUL_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Malgun Gothic");

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    wc.lpszClassName = L"AtecSftpDownApp";
    RegisterClassW(&wc);

    RECT rect = {0, 0, kClientWidth, kClientHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    const wchar_t* title = g_serverApp ? L"ATEC SFTP SERVER" : L"ATEC SFTP DOWN APP";
    HWND hwnd = CreateWindowW(L"AtecSftpDownApp", title,
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              rect.right - rect.left, rect.bottom - rect.top,
                              NULL, NULL, instance, NULL);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (g_logo != NULL) {
        delete g_logo;
        g_logo = NULL;
    }
    if (gdiplusToken != 0) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
    }
    if (g_font != NULL) {
        DeleteObject(g_font);
    }
    if (g_fontBold != NULL) {
        DeleteObject(g_fontBold);
    }
    if (g_fontExit != NULL) {
        DeleteObject(g_fontExit);
    }
    return 0;
}
