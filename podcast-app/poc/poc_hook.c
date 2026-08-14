/* poc_hook.c — route the About launcher tile to our own code.
 *
 * v1 pointed the tile's .data callback straight at a function in this shared
 * object and the launcher never came up. The audiobook mod's callback instead
 * targets 0x0075DAEC, which lives inside hiby_player's own .rodata — so the
 * launcher evidently will not accept a callback outside the executable image.
 *
 * v2 therefore follows the same shape: claim an unused cave inside .rodata,
 * write a MIPS trampoline there that jumps to us, and point the tile at the
 * cave. Both the cave write and the .data edit happen at runtime from this
 * constructor, so nothing in the firmware needs patching.
 *
 * Build:
 *   zig cc -target mipsel-linux-gnueabihf.2.22 -shared -fPIC -Os -s \
 *     -fvisibility=hidden -fno-common -o libpodcast_hook.so poc_hook.c
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>

/* Launcher tile records are 96 bytes: callback at +0x00, name at +0x18.
 * Two records are named launcher_apps_vg_about. */
#define ABOUT_CB_A      0x00892150u
#define ABOUT_CB_A_ORIG 0x0053BC20u
#define ABOUT_CB_B      0x00892570u
#define ABOUT_CB_B_ORIG 0x0053BC20u
#define DATA_PAGE       0x00892000u

/* Unused zeroed space in .rodata. The audiobook mod's own cave is at
 * 0x0075DAEC and its trailing string ends well before 0x0075DB2C, after which
 * 4412 bytes are zero. Sit clear of every offset its patcher documents
 * (0x35DAEC / 0x35DBC0 / 0x35DF40). */
#define CAVE_ADDR       0x0075E400u
#define CAVE_B_ADDR     0x0075E440u
#define CAVE_PAGE       0x0075E000u
#define PAGE_SPAN       0x2000u

#define LOG_PATH "/tmp/.podcast_hook.log"

static uint32_t orig_a = 0, orig_b = 0;

static void plog(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { write(fd, buf, n); close(fd); }
}

static int is_hiby_player(void) {
    int fd = open("/proc/self/comm", O_RDONLY);
    if (fd < 0) return 0;
    char buf[64];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return strstr(buf, "hiby_player") || strstr(buf, "system_main_thr");
}

/* Stands in for the podcast app. Chains to the stock About handler so the
 * device still behaves while we are only proving the route. */
static int podcast_entry(void *arg0, void *arg1) {
    plog("[podcast] TILE TAPPED via A arg0=%p arg1=%p\n", arg0, arg1);
    if (orig_a) {
        int (*stock)(void *, void *) = (int (*)(void *, void *))orig_a;
        return stock(arg0, arg1);
    }
    return 0;
}

static int podcast_entry_b(void *arg0, void *arg1) {
    plog("[podcast] TILE TAPPED via B arg0=%p arg1=%p\n", arg0, arg1);
    if (orig_b) {
        int (*stock)(void *, void *) = (int (*)(void *, void *))orig_b;
        return stock(arg0, arg1);
    }
    return 0;
}

/* lui t9, hi / addiu t9, t9, lo / jr t9 / nop */
static void build_trampoline(uint32_t target, uint32_t *out) {
    out[0] = 0x3C190000u | ((target + 0x8000) >> 16);
    out[1] = 0x27390000u | (target & 0xFFFF);
    out[2] = 0x03200008u;
    out[3] = 0x00000000u;
}

/* MIPS keeps separate I and D caches with no coherency between them, so freshly
 * written instructions are not visible to the fetcher until the caches are
 * flushed. A `sync` alone only orders stores. __NR_cacheflush is 4000+147 on
 * the o32 ABI; BCACHE (3) flushes both. */
#ifndef __NR_cacheflush
#define __NR_cacheflush 4147
#endif
#define ICACHE_FLAG 1
#define DCACHE_FLAG 2
#define BCACHE_FLAG 3

static int flush_icache(void *addr, int len) {
    return (int)syscall(__NR_cacheflush, addr, len, BCACHE_FLAG);
}

__attribute__((constructor))
static void podcast_init(void) {
    if (!is_hiby_player()) return;

    plog("[podcast] init pid=%d entry=%p\n", (int)getpid(), &podcast_entry);

    volatile uint32_t *cave = (volatile uint32_t *)CAVE_ADDR;

    /* Refuse to touch the cave unless it is still the zeroes we expect. */
    if (cave[0] != 0 || cave[1] != 0 || cave[2] != 0 || cave[3] != 0) {
        plog("[podcast] cave not free: %08X %08X %08X %08X\n",
             cave[0], cave[1], cave[2], cave[3]);
        return;
    }

    if (mprotect((void *)CAVE_PAGE, PAGE_SPAN,
                 PROT_READ | PROT_WRITE | PROT_EXEC) < 0) {
        plog("[podcast] cave mprotect failed\n");
        return;
    }
    uint32_t tramp[4];
    build_trampoline((uint32_t)&podcast_entry, tramp);
    cave[0] = tramp[0]; cave[1] = tramp[1];
    cave[2] = tramp[2]; cave[3] = tramp[3];
    __asm__ __volatile__("sync" ::: "memory");
    volatile uint32_t *caveb = (volatile uint32_t *)CAVE_B_ADDR;
    uint32_t trampb[4];
    build_trampoline((uint32_t)&podcast_entry_b, trampb);
    caveb[0] = trampb[0]; caveb[1] = trampb[1];
    caveb[2] = trampb[2]; caveb[3] = trampb[3];
    __asm__ __volatile__("sync" ::: "memory");
    int fc = flush_icache((void *)CAVE_ADDR, 0x80);
    plog("[podcast] cacheflush rc=%d\n", fc);
    if (mprotect((void *)CAVE_PAGE, PAGE_SPAN, PROT_READ | PROT_EXEC) < 0) {
        plog("[podcast] cave re-protect failed\n");
    }
    plog("[podcast] cave armed at 0x%08X: %08X %08X %08X %08X\n",
         CAVE_ADDR, cave[0], cave[1], cave[2], cave[3]);

    volatile uint32_t *a = (volatile uint32_t *)ABOUT_CB_A;
    if (*a != ABOUT_CB_A_ORIG) {
        plog("[podcast] unexpected About callback 0x%08X\n", *a);
        return;
    }
    if (mprotect((void *)DATA_PAGE, PAGE_SPAN, PROT_READ | PROT_WRITE) < 0) {
        plog("[podcast] data mprotect failed\n");
        return;
    }
    orig_a = *a;
    *a = CAVE_ADDR;

    volatile uint32_t *b = (volatile uint32_t *)ABOUT_CB_B;
    if (*b == ABOUT_CB_B_ORIG) {
        orig_b = *b;
        *b = CAVE_B_ADDR;
    } else {
        plog("[podcast] B unexpected 0x%08X\n", *b);
    }
    __asm__ __volatile__("sync" ::: "memory");

    plog("[podcast] A -> 0x%08X (was 0x%08X);  B -> 0x%08X (was 0x%08X)\n",
         *a, orig_a, *b, orig_b);
}
