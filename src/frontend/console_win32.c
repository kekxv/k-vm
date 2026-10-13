/* SPDX-License-Identifier: MIT */
#include "console_win32.h"
#ifdef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include "image_update.h"

/* A small queue keeps multibyte characters and key sequences intact when the
 * guest offers only one byte of space. Console records never block the VM. */
typedef struct {
    HANDLE input, output;
    DWORD input_mode, output_mode;
    UINT input_cp, output_cp;
    BOOL input_console, output_console, allow_ctrlc, escaped, input_eof;
    int width, height, stdin_mode, stdout_mode;
    BOOL restored;
    HANDLE reader;
    CRITICAL_SECTION reader_lock;
    uint8_t reader_bytes[8192];
    int reader_len;
    volatile LONG reader_stopping;
    WCHAR surrogate;
    uint8_t pending[8192];
    int start, end;
} WinConsole;
static WinConsole *console;
static volatile LONG stopping, ctrlc_pending;

BOOL kv_console_stop_requested(void) { return InterlockedCompareExchange(&stopping, 0, 0) != 0; }
static void stop_console(void)
{
    InterlockedExchange(&stopping, 1);
    image_update_cancel();
}
static BOOL WINAPI control_handler(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
        if (console && !console->allow_ctrlc)
            InterlockedIncrement(&ctrlc_pending);
        else
            stop_console();
        return TRUE;
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        stop_console();
        return TRUE;
    default: return FALSE;
    }
}
void kv_console_restore(void)
{
    WinConsole *s = console;
    if (!s || s->restored) return;
    s->restored = TRUE;
    if (s->reader) {
        InterlockedExchange(&s->reader_stopping, 1);
        CancelSynchronousIo(s->reader);
        WaitForSingleObject(s->reader, 1000);
        CloseHandle(s->reader);
        s->reader = NULL;
    }
    if (s->input_console) {
        SetConsoleMode(s->input, s->input_mode);
        SetConsoleCP(s->input_cp);
    }
    if (s->output_console) {
        SetConsoleMode(s->output, s->output_mode);
        SetConsoleOutputCP(s->output_cp);
    }
    SetConsoleCtrlHandler(control_handler, FALSE);
    if (s->stdin_mode >= 0) _setmode(_fileno(stdin), s->stdin_mode);
    if (s->stdout_mode >= 0) _setmode(_fileno(stdout), s->stdout_mode);
}
static void console_write(void *opaque, const uint8_t *buf, int len)
{
    WinConsole *s = opaque;
    DWORD written;
    while (len > 0 && WriteFile(s->output, buf, len, &written, NULL) && written) {
        buf += written;
        len -= written;
    }
}
static void queue_byte(WinConsole *s, uint8_t ch)
{
    if (s->escaped) {
        s->escaped = FALSE;
        if (ch == 'x' || ch == 'X') { stop_console(); return; }
        if (ch == 'h') {
            const char *help = "\r\nC-a x   exit emulator\r\nC-a h   print this help\r\nC-a C-a send C-a\r\n";
            console_write(s, (const uint8_t *)help, strlen(help));
            return;
        }
        if (ch != 1) return;
    } else if (ch == 1) {
        s->escaped = TRUE;
        return;
    }
    if (ch == 3 && s->allow_ctrlc) { stop_console(); return; }
    if (s->end < (int)sizeof(s->pending)) s->pending[s->end++] = ch;
}
static void queue_text(WinConsole *s, const char *text)
{
    while (*text) queue_byte(s, (uint8_t)*text++);
}
static void queue_key(WinConsole *s, KEY_EVENT_RECORD *key)
{
    WCHAR wc = key->uChar.UnicodeChar, chars[2];
    char utf8[8], seq[32];
    const char *suffix = NULL;
    int n, repeat, modifier = 1;
    DWORD state = key->dwControlKeyState;
    if (!key->bKeyDown) return;
    if (state & SHIFT_PRESSED) modifier += 1;
    if (state & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) modifier += 2;
    if (state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) modifier += 4;
    for (repeat = 0; repeat < key->wRepeatCount; repeat++) {
        if (key->wVirtualKeyCode == VK_TAB && (state & SHIFT_PRESSED)) {
            queue_text(s, "\033[Z");
            continue;
        }
        if (!wc && (state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) &&
            (key->wVirtualKeyCode == VK_SPACE || key->wVirtualKeyCode == '2')) {
            queue_byte(s, 0);
            continue;
        }
        if (wc) {
            if (wc >= 0xd800 && wc <= 0xdbff) { s->surrogate = wc; continue; }
            n = 0;
            if (s->surrogate && wc >= 0xdc00 && wc <= 0xdfff) chars[n++] = s->surrogate;
            s->surrogate = 0;
            chars[n++] = wc;
            n = WideCharToMultiByte(CP_UTF8, 0, chars, n, utf8, sizeof(utf8), NULL, NULL);
            /* AltGr is text input; only a standalone Alt prefixes Escape. */
            if ((state & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) &&
                !(state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))) queue_byte(s, 27);
            for (int i = 0; i < n; i++) queue_byte(s, (uint8_t)utf8[i]);
            continue;
        }
        switch (key->wVirtualKeyCode) {
        case VK_UP: suffix = "A"; break;
        case VK_DOWN: suffix = "B"; break;
        case VK_RIGHT: suffix = "C"; break;
        case VK_LEFT: suffix = "D"; break;
        case VK_HOME: suffix = "H"; break;
        case VK_END: suffix = "F"; break;
        case VK_INSERT: suffix = "2~"; break;
        case VK_DELETE: suffix = "3~"; break;
        case VK_PRIOR: suffix = "5~"; break;
        case VK_NEXT: suffix = "6~"; break;
        case VK_F1: suffix = "11~"; break;
        case VK_F2: suffix = "12~"; break;
        case VK_F3: suffix = "13~"; break;
        case VK_F4: suffix = "14~"; break;
        case VK_F5: suffix = "15~"; break;
        case VK_F6: suffix = "17~"; break;
        case VK_F7: suffix = "18~"; break;
        case VK_F8: suffix = "19~"; break;
        case VK_F9: suffix = "20~"; break;
        case VK_F10: suffix = "21~"; break;
        case VK_F11: suffix = "23~"; break;
        case VK_F12: suffix = "24~"; break;
        default: continue;
        }
        if (modifier == 1) snprintf(seq, sizeof(seq), "\033[%s", suffix);
        else if (strlen(suffix) == 1) snprintf(seq, sizeof(seq), "\033[1;%d%s", modifier, suffix);
        else snprintf(seq, sizeof(seq), "\033[%.*s;%d~", (int)strlen(suffix) - 1, suffix, modifier);
        queue_text(s, seq);
    }
}
/* Wine's inherited Unix pipe handles support ReadFile but not PeekNamedPipe.
 * A dedicated reader also keeps anonymous pipes nonblocking on native Windows. */
static DWORD WINAPI pipe_reader(void *opaque)
{
    WinConsole *s = opaque;
    uint8_t bytes[256]; DWORD n;
    while (!InterlockedCompareExchange(&s->reader_stopping, 0, 0)) {
        if (!ReadFile(s->input, bytes, sizeof(bytes), &n, NULL) || !n) break;
        int copied = 0;
        while (copied < (int)n && !InterlockedCompareExchange(&s->reader_stopping, 0, 0)) {
            EnterCriticalSection(&s->reader_lock);
            int count = min_int(n - copied, sizeof(s->reader_bytes) - s->reader_len);
            memcpy(s->reader_bytes + s->reader_len, bytes + copied, count);
            s->reader_len += count; copied += count;
            LeaveCriticalSection(&s->reader_lock);
            if (!count) Sleep(10);
        }
    }
    return 0;
}
static void collect_input(WinConsole *s)
{
    DWORD available, got;
    if (s->start) {
        memmove(s->pending, s->pending + s->start, s->end - s->start);
        s->end -= s->start;
        s->start = 0;
    }
    while (InterlockedCompareExchange(&ctrlc_pending, 0, 0) > 0 && s->end < (int)sizeof(s->pending)) {
        InterlockedDecrement(&ctrlc_pending);
        queue_byte(s, 3);
    }
    if (s->reader) {
        EnterCriticalSection(&s->reader_lock);
        int count = min_int(s->reader_len, sizeof(s->pending) - s->end);
        for (int i = 0; i < count; i++) queue_byte(s, s->reader_bytes[i]);
        memmove(s->reader_bytes, s->reader_bytes + count, s->reader_len - count);
        s->reader_len -= count;
        LeaveCriticalSection(&s->reader_lock);
        return;
    }
    if (s->input_eof) return;
    if (s->input_console) {
        INPUT_RECORD record;
        /* Reserve enough space for repeated key records without consuming
         * events when guest backpressure has filled the queue. */
        while (s->end < (int)sizeof(s->pending) - 1024 &&
               GetNumberOfConsoleInputEvents(s->input, &available) && available &&
               ReadConsoleInputW(s->input, &record, 1, &got) && got) {
            if (record.EventType == KEY_EVENT) queue_key(s, &record.Event.KeyEvent);
        }
    } else {
        uint8_t bytes[256];
        DWORD kind = GetFileType(s->input);
        if (kind == FILE_TYPE_PIPE) {
            if (!PeekNamedPipe(s->input, NULL, 0, NULL, &available, NULL)) {
                if (GetLastError() == ERROR_BROKEN_PIPE) s->input_eof = TRUE;
                return;
            }
            if (!available) return;
        } else if (kind == FILE_TYPE_DISK) available = sizeof(bytes);
        else return;
        available = min_int(available, min_int(sizeof(bytes), sizeof(s->pending) - s->end));
        if (available && ReadFile(s->input, bytes, available, &got, NULL)) {
            if (!got) s->input_eof = TRUE;
            for (DWORD i = 0; i < got; i++) queue_byte(s, bytes[i]);
        }
    }
}
static int console_read(void *opaque, uint8_t *buf, int len)
{
    WinConsole *s = opaque;
    collect_input(s);
    len = min_int(len, s->end - s->start);
    memcpy(buf, s->pending + s->start, len);
    s->start += len;
    return len;
}
CharacterDevice *kv_console_init(BOOL allow_ctrlc)
{
    CharacterDevice *dev = mallocz(sizeof(*dev));
    WinConsole *s = mallocz(sizeof(*s));
    s->input = GetStdHandle(STD_INPUT_HANDLE);
    s->output = GetStdHandle(STD_OUTPUT_HANDLE);
    s->allow_ctrlc = allow_ctrlc;
    s->input_console = GetConsoleMode(s->input, &s->input_mode);
    s->output_console = GetConsoleMode(s->output, &s->output_mode);
    s->input_cp = GetConsoleCP();
    s->output_cp = GetConsoleOutputCP();
    if (s->input_console) {
        DWORD mode = (s->input_mode | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS) &
            ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_QUICK_EDIT_MODE | ENABLE_VIRTUAL_TERMINAL_INPUT);
        if (!allow_ctrlc) mode &= ~ENABLE_PROCESSED_INPUT;
        else mode |= ENABLE_PROCESSED_INPUT;
        SetConsoleMode(s->input, mode);
        SetConsoleCP(CP_UTF8);
    }
    if (s->output_console) {
        SetConsoleMode(s->output, s->output_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        SetConsoleOutputCP(CP_UTF8);
    }
    s->stdin_mode = _setmode(_fileno(stdin), _O_BINARY);
    s->stdout_mode = _setmode(_fileno(stdout), _O_BINARY);
    if (!s->input_console && GetFileType(s->input) == FILE_TYPE_PIPE) {
        InitializeCriticalSection(&s->reader_lock);
        s->reader = CreateThread(NULL, 0, pipe_reader, s, 0, NULL);
        if (!s->reader) {
            fprintf(stderr, "k-vm: cannot start console pipe reader\n");
            exit(1);
        }
    }
    console = s;
    SetConsoleCtrlHandler(control_handler, TRUE);
    atexit(kv_console_restore);
    dev->opaque = s;
    dev->write_data = console_write;
    dev->read_data = console_read;
    return dev;
}
void kv_console_poll(VirtMachine *m)
{
    WinConsole *s = console;
    CONSOLE_SCREEN_BUFFER_INFO info;
    uint8_t bytes[256];
    int width = 80, height = 25, n;
    if (!s || !m->console_dev) return;
    if (GetConsoleScreenBufferInfo(s->output, &info)) {
        width = info.srWindow.Right - info.srWindow.Left + 1;
        height = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
    if (virtio_console_can_write_data(m->console_dev) &&
        width >= 4 && height >= 4 && (width != s->width || height != s->height)) {
        virtio_console_resize_event(m->console_dev, width, height);
        s->width = width; s->height = height;
    }
    if (virtio_console_can_write_data(m->console_dev)) {
        n = min_int(sizeof(bytes), virtio_console_get_write_len(m->console_dev));
        n = console_read(s, bytes, n);
        if (n > 0) virtio_console_write_data(m->console_dev, bytes, n);
    }
}
#endif
