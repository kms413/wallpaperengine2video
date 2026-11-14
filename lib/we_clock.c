/* we_clock.c —— 虚拟时钟垫片：为 wallpaperengine2video 提供确定性逐帧渲染
 *
 * 原理：LD_PRELOAD 注入后接管进程内的 clock_gettime/gettimeofday，
 * 让 linux-wallpaperengine（及 CEF/mpv/SDL/std::chrono）看到被冻结的
 * 虚拟时间。外部通过 FIFO 发送命令控制：
 *
 *   freeze        冻结时钟（以当前真实时刻为新基准）
 *   step [N]      虚拟时钟前进 N 纳秒（缺省 WE_CLOCK_STEP_NS，默认 1/60 秒）
 *   run           恢复实时透传
 *
 * 墙纸动画完全由时钟驱动，时钟冻结则画面静止；每步进一次即得到
 * 一帧确定的动画状态，与 GPU 速度无关——GPU 慢只影响导出耗时。
 *
 * 构建：gcc -O2 -fPIC -shared -o we_clock.so we_clock.c -lpthread
 */
#define _GNU_SOURCE
#include <time.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>

static long long g_base_mono;            /* 冻结时的真实单调时钟基准（ns） */
static long long g_base_real;            /* 冻结时的真实实时时钟基准（ns） */
static _Atomic long long g_off;          /* 冻结后累计步进量（ns） */
static _Atomic int g_frozen;             /* 1=冻结（虚拟时间），0=实时透传 */
static long long g_step_ns = 16666666;   /* 每次 step 的前进量 */
static char g_fifo[PATH_MAX] = "";

/* 直接走 syscall 取真实时钟，避免递归进入本文件的挂钩 */
static long long real_ns(clockid_t clk) {
    struct timespec ts;
    syscall(SYS_clock_gettime, clk, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static long long vmono(void) {
    return g_frozen ? g_base_mono + g_off : real_ns(CLOCK_MONOTONIC);
}

static long long vreal(void) {
    return g_frozen ? g_base_real + g_off : real_ns(CLOCK_REALTIME);
}

static void vfill(clockid_t clk, struct timespec *tp, long long v) {
    (void)clk;
    tp->tv_sec = (time_t)(v / 1000000000LL);
    tp->tv_nsec = (long)(v % 1000000000LL);
}

/* ------------------------------------------------------------------------- */
/* 控制线程：读 FIFO 命令                                                      */
/* ------------------------------------------------------------------------- */
static void handle_line(char *line) {
    while (*line == ' ' || *line == '\t') line++;
    if (!strncmp(line, "freeze", 6)) {
        g_base_mono = real_ns(CLOCK_MONOTONIC);
        g_base_real = real_ns(CLOCK_REALTIME);
        g_off = 0;
        g_frozen = 1;
    } else if (!strncmp(line, "run", 3)) {
        g_frozen = 0;
    } else if (!strncmp(line, "step", 4)) {
        long long n = g_step_ns;
        const char *p = line + 4;
        while (*p == ' ') p++;
        if (*p) {
            long long arg = atoll(p);
            if (arg > 0) n = arg;
        }
        if (g_frozen) g_off += n;
    }
    /* 未知命令忽略 */
}

static void *ctrl_thread(void *arg) {
    (void)arg;
    int fd = open(g_fifo, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return NULL;
    char buf[4096], carry[256] = "";
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        poll(&p, 1, 50);
        if (p.revents & (POLLERR | POLLHUP) && !(p.revents & POLLIN)) {
            /* 写端全部关闭：重开读端，避免 HUP 忙转 */
            close(fd);
            usleep(5000);
            fd = open(g_fifo, O_RDONLY | O_NONBLOCK);
            if (fd < 0) return NULL;
            continue;
        }
        if (!(p.revents & POLLIN)) continue;
        ssize_t n = read(fd, buf, sizeof buf - 1);
        if (n <= 0) continue;
        buf[n] = '\0';
        /* 按行拆分，处理跨 read 的半行 */
        char *start = buf;
        for (char *q = buf; ; q++) {
            if (*q == '\n') {
                *q = '\0';
                if (carry[0]) {
                    strncat(carry, start, sizeof carry - strlen(carry) - 1);
                    handle_line(carry);
                    carry[0] = '\0';
                } else {
                    handle_line(start);
                }
                start = q + 1;
            } else if (*q == '\0') {
                if (start < q && strlen(start) + strlen(carry) < sizeof carry - 1) {
                    strcat(carry, start);
                }
                break;
            }
        }
    }
}

__attribute__((constructor)) static void wec_init(void) {
    const char *s = getenv("WE_CLOCK_STEP_NS");
    if (s) {
        long long v = atoll(s);
        if (v >= 1000000) g_step_ns = v;   /* 下限 1ms，防止零步进 */
    }
    const char *f = getenv("WE_CLOCK_FIFO");
    if (f && *f) {
        snprintf(g_fifo, sizeof g_fifo, "%s", f);
        pthread_t th;
        if (pthread_create(&th, NULL, ctrl_thread, NULL) == 0)
            pthread_detach(th);
    }
    /* 无 FIFO 时保持实时透传，不影响正常使用 */
}

/* ------------------------------------------------------------------------- */
/* 挂钩：时间读取                                                              */
/* ------------------------------------------------------------------------- */
int clock_gettime(clockid_t clk, struct timespec *tp) {
    switch (clk) {
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
        vfill(clk, tp, vmono());
        return 0;
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
        vfill(clk, tp, vreal());
        return 0;
    default:
        struct timespec raw;
        syscall(SYS_clock_gettime, clk, &raw);
        *tp = raw;
        return 0;
    }
}

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if ((uintptr_t)tv != 0) {
        long long v = vreal();
        tv->tv_sec = (time_t)(v / 1000000000LL);
        tv->tv_usec = (long)((v % 1000000000LL) / 1000);
    }
    return 0;
}

/* sleep 类保持透传：lwe 以冻结时钟计算的相对间隔（如 16ms/66ms 帧间隔）
 * 仍会正常 sleep，节奏不变；绝对超时（cond_timedwait 等）因基准已冻结
 * 多数立即返回，属可接受的忙转代价，不参与虚拟时间换算以免出错死等。 */
