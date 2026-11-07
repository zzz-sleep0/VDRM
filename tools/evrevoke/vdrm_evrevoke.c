#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef EVIOCREVOKE
#define EVIOCREVOKE _IOW('E', 0x91, int)
#endif

static int is_vdrm_event(const char *path)
{
    return strstr(path, "/dev/input/event") == path;
}

int main(void)
{
    DIR *proc = opendir("/proc");
    struct dirent *pe;
    int revoked = 0;

    if (!proc)
        return 1;
    while ((pe = readdir(proc))) {
        char fddir[64];
        DIR *fds;
        struct dirent *fe;

        if (pe->d_name[0] < '0' || pe->d_name[0] > '9')
            continue;
        snprintf(fddir, sizeof(fddir), "/proc/%s/fd", pe->d_name);
        fds = opendir(fddir);
        if (!fds)
            continue;
        while ((fe = readdir(fds))) {
            char link[128], path[160], namep[160], name[128];
            ssize_t n;
            int fd, i;
            FILE *f;

            if (fe->d_name[0] == '.')
                continue;
            snprintf(path, sizeof(path), "%s/%s", fddir, fe->d_name);
            n = readlink(path, link, sizeof(link) - 1);
            if (n <= 0)
                continue;
            link[n] = 0;
            if (strncmp(link, "/dev/input/event", 16) != 0)
                continue;
            i = atoi(link + 16);
            snprintf(namep, sizeof(namep),
                     "/sys/class/input/event%d/device/name", i);
            f = fopen(namep, "r");
            if (!f)
                continue;
            if (!fgets(name, sizeof(name), f)) {
                fclose(f);
                continue;
            }
            fclose(f);
            if (strncmp(name, "vdrm-", 5) != 0)
                continue;

            fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0)
                continue;
            if (ioctl(fd, EVIOCREVOKE, 0) == 0) {
                printf("revoked %s (pid %s)\n", link, pe->d_name);
                revoked++;
            }
            close(fd);
        }
        closedir(fds);
    }
    closedir(proc);
    printf("revoked %d fd(s)\n", revoked);
    return 0;
}
