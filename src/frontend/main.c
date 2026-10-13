/*
 * TinyEMU
 * 
 * Copyright (c) 2016-2018 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <getopt.h>
#ifndef _WIN32
#include <termios.h>
#include <sys/ioctl.h>
#endif
#include <sys/stat.h>
#include <signal.h>

#include "cutils.h"
#include "iomem.h"
#include "virtio.h"
#include "launch.h"
#include "update.h"
#ifdef CONFIG_FS_NET
#include "fs_utils.h"
#include "fs_wget.h"
#endif
#include "host_network.h"
#ifdef _WIN32
#include "console_win32.h"
#include "block_file.h"
#endif

#ifndef _WIN32

typedef struct {
    int stdin_fd;
    int console_esc_state;
    BOOL resize_pending;
} STDIODevice;

static struct termios oldtty;
static int old_fd0_flags;
static STDIODevice *global_stdio_device;
static volatile sig_atomic_t stop_requested;

static void request_stop(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static void term_exit(void)
{
    tcsetattr (0, TCSANOW, &oldtty);
    fcntl(0, F_SETFL, old_fd0_flags);
}

static void term_init(BOOL allow_ctrlc)
{
    struct termios tty;

    memset(&tty, 0, sizeof(tty));
    tcgetattr (0, &tty);
    oldtty = tty;
    old_fd0_flags = fcntl(0, F_GETFL);

    tty.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP
                          |INLCR|IGNCR|ICRNL|IXON);
    tty.c_oflag |= OPOST;
    tty.c_lflag &= ~(ECHO|ECHONL|ICANON|IEXTEN);
    if (!allow_ctrlc)
        tty.c_lflag &= ~ISIG;
    tty.c_cflag &= ~(CSIZE|PARENB);
    tty.c_cflag |= CS8;
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;

    tcsetattr (0, TCSANOW, &tty);

    atexit(term_exit);
}

static void console_write(void *opaque, const uint8_t *buf, int len)
{
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
}

static int console_read(void *opaque, uint8_t *buf, int len)
{
    STDIODevice *s = opaque;
    int ret, i, j;
    uint8_t ch;
    
    if (len <= 0)
        return 0;

    ret = read(s->stdin_fd, buf, len);
    if (ret < 0)
        return 0;
    if (ret == 0) {
        /* EOF */
        exit(1);
    }

    j = 0;
    for(i = 0; i < ret; i++) {
        ch = buf[i];
        if (s->console_esc_state) {
            s->console_esc_state = 0;
            switch(ch) {
            case 'x':
                printf("Terminated\n");
                exit(0);
            case 'h':
                printf("\n"
                       "C-a h   print this help\n"
                       "C-a x   exit emulator\n"
                       "C-a C-a send C-a\n"
                       );
                break;
            case 1:
                goto output_char;
            default:
                break;
            }
        } else {
            if (ch == 1) {
                s->console_esc_state = 1;
            } else {
            output_char:
                buf[j++] = ch;
            }
        }
    }
    return j;
}

static void term_resize_handler(int sig)
{
    if (global_stdio_device)
        global_stdio_device->resize_pending = TRUE;
}

static void console_get_size(STDIODevice *s, int *pw, int *ph)
{
    struct winsize ws;
    int width, height;
    /* default values */
    width = 80;
    height = 25;
    if (ioctl(s->stdin_fd, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_col >= 4 && ws.ws_row >= 4) {
        width = ws.ws_col;
        height = ws.ws_row;
    }
    *pw = width;
    *ph = height;
}

CharacterDevice *console_init(BOOL allow_ctrlc)
{
    CharacterDevice *dev;
    STDIODevice *s;
    struct sigaction sig;

    term_init(allow_ctrlc);

    dev = mallocz(sizeof(*dev));
    s = mallocz(sizeof(*s));
    s->stdin_fd = 0;
    /* Note: the glibc does not properly tests the return value of
       write() in printf, so some messages on stdout may be lost */
    fcntl(s->stdin_fd, F_SETFL, O_NONBLOCK);

    s->resize_pending = TRUE;
    global_stdio_device = s;
    
    /* use a signal to get the host terminal resize events */
    sig.sa_handler = term_resize_handler;
    sigemptyset(&sig.sa_mask);
    sig.sa_flags = 0;
    sigaction(SIGWINCH, &sig, NULL);
    
    dev->opaque = s;
    dev->write_data = console_write;
    dev->read_data = console_read;
    return dev;
}

#endif /* !_WIN32 */

#define MAX_EXEC_CYCLE 500000
#define MAX_SLEEP_TIME 10 /* in ms */

void virt_machine_run(VirtMachine *m)
{
    fd_set rfds, wfds, efds;
    int fd_max, ret, delay;
    struct timeval tv;
#ifndef _WIN32
    int stdin_fd;
#endif
    
    delay = virt_machine_get_sleep_duration(m, MAX_SLEEP_TIME);
    
    /* wait for an event */
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    fd_max = -1;
#ifndef _WIN32
    if (m->console_dev && virtio_console_can_write_data(m->console_dev)) {
        STDIODevice *s = m->console->opaque;
        stdin_fd = s->stdin_fd;
        FD_SET(stdin_fd, &rfds);
        fd_max = stdin_fd;

        if (s->resize_pending) {
            int width, height;
            console_get_size(s, &width, &height);
            virtio_console_resize_event(m->console_dev, width, height);
            s->resize_pending = FALSE;
        }
    }
#endif
    if (m->net) {
        m->net->select_fill(m->net, &fd_max, &rfds, &wfds, &efds, &delay);
    }
#ifdef CONFIG_FS_NET
    fs_net_set_fdset(&fd_max, &rfds, &wfds, &efds, &delay);
#endif
    tv.tv_sec = delay / 1000;
    tv.tv_usec = (delay % 1000) * 1000;
    #ifdef _WIN32
    if (!rfds.fd_count && !wfds.fd_count && !efds.fd_count) {
        Sleep(delay);
        ret = 0;
    } else {
        ret = select(0, &rfds, &wfds, &efds, &tv);
    }
    kv_console_poll(m);
#else
    ret = select(fd_max + 1, &rfds, &wfds, &efds, &tv);
#endif
    if (m->net) {
        m->net->select_poll(m->net, &rfds, &wfds, &efds, ret);
    }
    if (ret > 0) {
#ifndef _WIN32
        if (m->console_dev && FD_ISSET(stdin_fd, &rfds)) {
            uint8_t buf[128];
            int ret, len;
            len = virtio_console_get_write_len(m->console_dev);
            len = min_int(len, sizeof(buf));
            ret = m->console->read_data(m->console->opaque, buf, len);
            if (ret > 0) {
                virtio_console_write_data(m->console_dev, buf, ret);
            }
        }
#endif
    }

#ifdef CONFIG_SDL
    sdl_refresh(m);
#endif
    
    virt_machine_interp(m, MAX_EXEC_CYCLE);
}

/*******************************************************/

static struct option options[] = {
    { "help", no_argument, NULL, 'h' },
    { "ctrlc", no_argument },
    { "rw", no_argument },
    { "ro", no_argument },
    { "append", required_argument },
    { "build-preload", required_argument },
    { NULL },
};

void help(void)
{
    printf("k-vm version " CONFIG_VERSION ", Copyright (c) 2016-2018 Fabrice Bellard\n"
           "usage: k-vm [options] config_file\n"
           "       k-vm sandbox --image DIR_OR_CFG [--map TAG=HOST[:ro|rw]]\n"
           "       k-vm sandbox --help\n"
           "       k-vm update [--image DIR_OR_CFG] [--version VERSION]\n"
           "options are:\n"
           "-m ram_size       set the RAM size in MB\n"
           "-rw               allow write access to the disk image (default=snapshot)\n"
           "-ctrlc            the C-c key stops the emulator instead of being sent to the\n"
           "                  emulated software\n"
           "-append cmdline   append cmdline to the kernel command line\n"
           "\n"
           "Console keys:\n"
           "Press C-a x to exit the emulator, C-a h to get some help.\n");
    exit(1);
}

#ifdef CONFIG_FS_NET
static BOOL net_completed;

static void net_start_cb(void *arg)
{
    net_completed = TRUE;
}

static BOOL net_poll_cb(void *arg)
{
    return net_completed;
}

#endif

#ifdef _WIN32
#define main kv_main
#endif
int main(int argc, char **argv)
{
    VirtMachine *s;
    const char *path, *cmdline, *build_preload_file;
    int c, option_index, i, ram_size;
    BOOL allow_ctrlc;
    BlockDeviceModeEnum drive_mode;
    VirtMachineParams p_s, *p = &p_s;

#ifdef _WIN32
    BlockDevice *opened_drives[MAX_DRIVE_DEVICE];
    FSDevice *opened_fs[MAX_FS_DEVICE];
    int opened_drive_count, opened_fs_count;
#endif
    ram_size = -1;
    allow_ctrlc = FALSE;
    (void)allow_ctrlc;
    drive_mode = BF_MODE_SNAPSHOT;
    cmdline = NULL;
    build_preload_file = NULL;
    BOOL update = argc > 1 && !strcmp(argv[1], "update");
    if (argc > 1 && (!strcmp(argv[1], "sandbox") || update)) {
        if (sandbox_prepare(argc - 1, argv + 1, p, &drive_mode, &allow_ctrlc))
            return 1;
        if (update) {
            int status = sandbox_update_run(p);
            if (status >= 0) return status;
        }
    } else {
        for(;;) {
            c = getopt_long_only(argc, argv, "hm:", options, &option_index);
            if (c == -1)
                break;
            switch(c) {
            case 0:
                switch(option_index) {
                case 1: /* ctrlc */
                    allow_ctrlc = TRUE;
                    break;
                case 2: /* rw */
                    drive_mode = BF_MODE_RW;
                    break;
                case 3: /* ro */
                    drive_mode = BF_MODE_RO;
                    break;
                case 4: /* append */
                    cmdline = optarg;
                    break;
                case 5: /* build-preload */
                    build_preload_file = optarg;
                    break;
                default:
                    fprintf(stderr, "unknown option index: %d\n", option_index);
                    exit(1);
                }
                break;
            case 'h':
                help();
                break;
            case 'm':
                ram_size = strtoul(optarg, NULL, 0);
                break;
            default:
                exit(1);
            }
        }

        if (optind >= argc) {
            help();
        }

        path = argv[optind++];

        virt_machine_set_defaults(p);
#ifdef CONFIG_FS_NET
        fs_wget_init();
#endif
        virt_machine_load_config_file(p, path, NULL, NULL);
#ifdef CONFIG_FS_NET
        fs_net_event_loop(NULL, NULL);
#endif

    }

    /* override some config parameters */

    if (ram_size > 0) {
        p->ram_size = (uint64_t)ram_size << 20;
    }
    if (cmdline) {
        vm_add_cmdline(p, cmdline);
    }
    
#ifndef _WIN32
    struct sigaction stop_action = {0};
    stop_action.sa_handler = request_stop;
    sigemptyset(&stop_action.sa_mask);
    if (allow_ctrlc)
        sigaction(SIGINT, &stop_action, NULL);
    sigaction(SIGTERM, &stop_action, NULL);
#endif

    /* open the files & devices */
    for(i = 0; i < p->drive_count; i++) {
        BlockDevice *drive;
        char *fname;
        fname = get_file_path(p->cfg_filename, p->tab_drive[i].filename);
#ifdef CONFIG_FS_NET
        if (is_url(fname)) {
            net_completed = FALSE;
            drive = block_device_init_http(fname, 128 * 1024,
                                           net_start_cb, NULL);
            /* wait until the drive is initialized */
            fs_net_event_loop(net_poll_cb, NULL);
        } else
#endif
        {
            drive = block_file_init(fname, drive_mode);
            if (!drive) {
                fprintf(stderr, "%s: cannot open raw disk: %s\n", fname, strerror(errno));
                exit(1);
            }
        }
        free(fname);
        p->tab_drive[i].block_dev = drive;
    }

    for(i = 0; i < p->fs_count; i++) {
        FSDevice *fs;
        const char *path;
        path = p->tab_fs[i].filename;
#ifdef CONFIG_FS_NET
        if (is_url(path)) {
            fs = fs_net_init(path, NULL, NULL);
            if (!fs)
                exit(1);
            if (build_preload_file)
                fs_dump_cache_load(fs, build_preload_file);
            fs_net_event_loop(NULL, NULL);
        } else
#endif
        {
            char *fname;
            fname = get_file_path(p->cfg_filename, path);
#ifdef _WIN32
            fs = fs_confined_init(fname, p->tab_fs[i].read_only);
#else
            fs = p->tab_fs[i].confined ? fs_confined_init(fname, p->tab_fs[i].read_only) : fs_disk_init(fname);
#endif
            if (!fs) {
                fprintf(stderr, "%s: cannot open host mapping: %s\n", fname, strerror(errno));
                exit(1);
            }
            free(fname);
        }
        p->tab_fs[i].fs_dev = fs;
    }

    for(i = 0; i < p->eth_count; i++) {
#ifdef CONFIG_SLIRP
        if (!strcmp(p->tab_eth[i].driver, "user")) {
            p->tab_eth[i].net = slirp_open();
            if (!p->tab_eth[i].net)
                exit(1);
        } else
#endif
#ifndef _WIN32
        if (!strcmp(p->tab_eth[i].driver, "tap")) {
            p->tab_eth[i].net = tun_open(p->tab_eth[i].ifname);
            if (!p->tab_eth[i].net)
                exit(1);
        } else
#endif
        {
            fprintf(stderr, "Unsupported network driver '%s'\n",
                    p->tab_eth[i].driver);
            exit(1);
        }
    }
    
#ifdef CONFIG_SDL
    if (p->display_device) {
        sdl_init(p->width, p->height);
    } else
#endif
    {
#ifdef _WIN32
        p->console = kv_console_init(allow_ctrlc);
#else
        p->console = console_init(allow_ctrlc);
#endif
    }
    p->rtc_real_time = TRUE;

    s = virt_machine_init(p);
    if (!s)
        exit(1);
    
#ifdef _WIN32
    opened_drive_count = p->drive_count;
    opened_fs_count = p->fs_count;
    for (i = 0; i < opened_drive_count; i++) opened_drives[i] = p->tab_drive[i].block_dev;
    for (i = 0; i < opened_fs_count; i++) opened_fs[i] = p->tab_fs[i].fs_dev;
#endif
    virt_machine_free_config(p);

    if (s->net) {
        s->net->device_set_carrier(s->net, TRUE);
    }
    
#ifdef _WIN32
    while (!kv_console_stop_requested() && !s->shutdown_requested) {
#else
    while (!stop_requested) {
#endif
        virt_machine_run(s);
    }
    virt_machine_end(s);
#ifdef _WIN32
    for (i = 0; i < opened_drive_count; i++) block_file_end(opened_drives[i]);
    for (i = 0; i < opened_fs_count; i++) fs_end(opened_fs[i]);
    kv_console_restore();
    if (update) return sandbox_update_complete(0);
#endif
    return 0;
}
