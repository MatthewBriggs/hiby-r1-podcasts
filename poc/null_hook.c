#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
__attribute__((constructor))
static void null_init(void) {
    int fd = open("/proc/self/comm", O_RDONLY);
    char c[64] = {0};
    if (fd >= 0) { read(fd, c, 63); close(fd); }
    int l = open("/tmp/.podcast_hook.log", O_WRONLY|O_CREAT|O_APPEND, 0644);
    if (l >= 0) {
        char b[128];
        int n = snprintf(b, sizeof b, "[null] loaded in %.*s pid=%d\n",
                         (int)strcspn(c, "\n"), c, (int)getpid());
        write(l, b, n); close(l);
    }
}
