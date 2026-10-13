/* SPDX-License-Identifier: MIT */
/* Native Win32 key encoding and pipe backpressure regression tests. */
#include <assert.h>
#include "../src/frontend/console_win32.c"
void *mallocz(size_t n) { return calloc(1, n); }
static int cancellations;
void image_update_cancel(void) { cancellations++; }
void virtio_console_resize_event(VIRTIODevice *s, int width, int height) { (void)s; (void)width; (void)height; }
BOOL virtio_console_can_write_data(VIRTIODevice *s) { (void)s; return FALSE; }
int virtio_console_get_write_len(VIRTIODevice *s) { (void)s; return 0; }
int virtio_console_write_data(VIRTIODevice *s, const uint8_t *buf, int len) { (void)s; (void)buf; return len; }
static void key(WinConsole *s, WORD vk, WCHAR wc, DWORD modifiers)
{
    KEY_EVENT_RECORD event = { .bKeyDown = TRUE, .wRepeatCount = 1, .wVirtualKeyCode = vk,
        .uChar.UnicodeChar = wc, .dwControlKeyState = modifiers };
    queue_key(s, &event);
}
int main(void)
{
    WinConsole s = {0};
    uint8_t buf[64];
    HANDLE write_end;
    DWORD written;
    queue_byte(&s, 3);
    assert(s.pending[s.end - 1] == 3 && !kv_console_stop_requested());
    queue_byte(&s, 1); queue_byte(&s, 1);
    assert(s.pending[s.end - 1] == 1);
    s.end = 0;
    key(&s, VK_UP, 0, 0); key(&s, VK_LEFT, 0, LEFT_CTRL_PRESSED);
    key(&s, VK_TAB, '\t', SHIFT_PRESSED);
    assert(s.end == 12 && !memcmp(s.pending, "\033[A\033[1;5D\033[Z", 12));
    s.end = 0;
    key(&s, 0, 0x4e2d, 0); key(&s, 0, 0xd83d, 0); key(&s, 0, 0xde00, 0);
    assert(s.end == 7 && !memcmp(s.pending, "\xe4\xb8\xad\xf0\x9f\x98\x80", 7));
    s.end = 0;
    assert(CreatePipe(&s.input, &write_end, NULL, 0));
    assert(WriteFile(write_end, "a\001\001\003b", 5, &written, NULL));
    assert(console_read(&s, buf, 1) == 1 && buf[0] == 'a');
    assert(console_read(&s, buf, sizeof(buf)) == 3 && !memcmp(buf, "\001\003b", 3));
    assert(console_read(&s, buf, sizeof(buf)) == 0);
    CloseHandle(write_end);
    assert(console_read(&s, buf, sizeof(buf)) == 0 && s.input_eof);
    CloseHandle(s.input);
    memset(&s, 0, sizeof(s));
    assert(CreatePipe(&s.input, &write_end, NULL, 0));
    InitializeCriticalSection(&s.reader_lock);
    s.reader = CreateThread(NULL, 0, pipe_reader, &s, 0, NULL); assert(s.reader);
    assert(WriteFile(write_end, "thread\003", 7, &written, NULL));
    int count = 0;
    for (int i = 0; i < 100 && !count; i++) { count = console_read(&s, buf, sizeof(buf)); if (!count) Sleep(1); }
    assert(count == 7 && !memcmp(buf, "thread\003", 7));
    CloseHandle(write_end);
    assert(WaitForSingleObject(s.reader, 1000) == WAIT_OBJECT_0);
    CloseHandle(s.reader); CloseHandle(s.input); DeleteCriticalSection(&s.reader_lock);
    s.reader = NULL;
    queue_byte(&s, 1); queue_byte(&s, 'x');
    assert(kv_console_stop_requested() && cancellations == 1);
    puts("Win32 console tests passed");
    return 0;
}
