#ifndef SFTP_DEMO_MSVC_COMPAT_H
#define SFTP_DEMO_MSVC_COMPAT_H

/* MSVC 전용 CRT 함수를 MinGW/gcc 직접 빌드에서도 쓸 수 있게 맞춘다. */
#ifndef _MSC_VER

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <errno.h>

#ifndef _stricmp
#define _stricmp strcasecmp
#endif

#ifndef _strnicmp
#define _strnicmp strncasecmp
#endif

/* WinLibs/MinGW UCRT already provides sprintf_s and fopen_s. */
#if !defined(__MINGW32__) && !defined(__MINGW64__)

/* 기능: 고정 크기 버퍼에 서식 문자열을 쓴다.
   역할: MSVC sprintf_s가 없는 컴파일러에서 포트 문자열 생성에 쓴다. */
template <size_t N>
int sprintf_s(char (&buffer)[N], const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(buffer, N, format, args);
    va_end(args);
    return written;
}

/* 기능: 파일을 열고 결과 코드를 반환한다.
   역할: MSVC fopen_s가 없는 컴파일러에서 로컬 다운로드 파일을 연다. */
inline int fopen_s(FILE** file, const char* path, const char* mode)
{
    if (file == NULL) {
        return EINVAL;
    }
    *file = fopen(path, mode);
    return (*file != NULL) ? 0 : errno;
}

#endif

#endif /* !_MSC_VER */

#endif
