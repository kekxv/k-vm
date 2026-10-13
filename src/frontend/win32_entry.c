/* SPDX-License-Identifier: MIT */
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>
#include <stdio.h>
int kv_main(int argc, char **argv);
int main(void)
{
    int argc, result = 1;
    WCHAR **wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    char **argv;
    if (!wide) return 1;
    argv = calloc(argc + 1, sizeof(*argv));
    if (!argv) { LocalFree(wide); return 1; }
    for (int i = 0; i < argc; i++) {
        int len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, NULL, 0, NULL, NULL);
        if (!len || !(argv[i] = malloc(len)) ||
            !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, argv[i], len, NULL, NULL)) goto done;
    }
    result = kv_main(argc, argv);
done:
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv);
    LocalFree(wide);
    return result;
}
#endif
