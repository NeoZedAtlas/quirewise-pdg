#include "pdg_internal.h"

#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
#endif

/* FILE* helper shared by the file entry points. Windows paths are passed as
 * UTF-8 by the public API, but the CRT fopen() resolves them against the ANSI
 * code page, so a directory such as "G.回族典藏全书" fails to open. */
FILE *pdg_fopen_utf8(const char *path, const char *mode)
{
    if (!path || !mode) {
        return NULL;
    }
#if defined(_WIN32)
    {
        wchar_t wide_path[2048];
        wchar_t wide_mode[16];
        if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide_path, 2048)) {
            return NULL;
        }
        if (!MultiByteToWideChar(CP_UTF8, 0, mode, -1, wide_mode, 16)) {
            return NULL;
        }
        return _wfopen(wide_path, wide_mode);
    }
#else
    return fopen(path, mode);
#endif
}
