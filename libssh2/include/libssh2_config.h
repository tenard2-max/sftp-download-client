/* Manual Windows/MSVC config for vendored libssh2 (WinCNG).
 * With _WIN32, libssh2_setup.h also provides defaults when HAVE_CONFIG_H
 * is not set; this header is available if a build defines HAVE_CONFIG_H.
 */
#ifndef LIBSSH2_CONFIG_H
#define LIBSSH2_CONFIG_H

#define HAVE_SELECT 1
#define HAVE_WINDOWS_H 1

#if defined(_MSC_VER) && (_MSC_VER >= 1900)
#define HAVE_SNPRINTF 1
#endif

#if defined(_MSC_VER) && (_MSC_VER >= 1800)
#define HAVE_INTTYPES_H 1
#define HAVE_STRTOLL 1
#else
#define HAVE_STRTOI64 1
#endif

#endif /* LIBSSH2_CONFIG_H */
