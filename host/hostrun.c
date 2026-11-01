/*
 * Running host tools for the services (hostrun.h).
 * MIT, Copyright (c) 2026 AmiCradle Team.
 */
#define _GNU_SOURCE
#include "hostrun.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int hr_run(char *const argv[], int fd_out)
{
    posix_spawn_file_actions_t fa;
    pid_t pid;
    int st = -1, rc;

    posix_spawn_file_actions_init(&fa);
    if (fd_out >= 0)
        posix_spawn_file_actions_adddup2(&fa, fd_out, 1);
    else
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    rc = posix_spawnp(&pid, argv[0], &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc)
        return -1;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

char *hr_run_read(char *const argv[], size_t *len)
{
    FILE *f = tmpfile();
    char *out = NULL;
    long n;

    if (!f)
        return NULL;
    if (hr_run(argv, fileno(f)) == 0 && fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0) {
        rewind(f);
        if ((out = malloc(n + 1)) && fread(out, 1, n, f) == (size_t)n) {
            out[n] = 0;
            if (len)
                *len = n;
        } else {
            free(out);
            out = NULL;
        }
    }
    fclose(f);
    return out;
}

int hr_tempdir(char *out, size_t room)
{
    const char *base = getenv("TMPDIR");
    snprintf(out, room, "%s/openservice-XXXXXX", base && *base ? base : "/tmp");
    return mkdtemp(out) ? 0 : -1;
}

void hr_rmdir(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[1024];

    if (d) {
        while ((e = readdir(d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
                snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
                unlink(path);
            }
        closedir(d);
    }
    rmdir(dir);
}

int hr_write(const char *path, const uint8_t *d, uint32_t n)
{
    FILE *f = fopen(path, "wb");
    int ok;

    if (!f)
        return -1;
    ok = fwrite(d, 1, n, f) == n;
    return fclose(f) == 0 && ok ? 0 : -1;
}

int hr_move(const char *from, const char *to)
{
    char part[1100], buf[65536];
    FILE *in, *out;
    size_t n;
    int ok = 1;

    if (rename(from, to) == 0)
        return 0;
    if (errno != EXDEV)
        return -1;
    /* Copied beside the destination first, so no reader sees half a file. */
    snprintf(part, sizeof part, "%s.%d.part", to, (int)getpid());
    if (!(in = fopen(from, "rb")))
        return -1;
    if (!(out = fopen(part, "wb"))) {
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    if (ferror(in))
        ok = 0;
    fclose(in);
    if (fclose(out) != 0)
        ok = 0;
    if (!ok || rename(part, to) != 0) {
        unlink(part);
        return -1;
    }
    unlink(from);
    return 0;
}

uint8_t *hr_read(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *out = NULL;
    long n;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0) {
        rewind(f);
        if ((out = malloc(n)) && fread(out, 1, n, f) == (size_t)n)
            *len = n;
        else {
            free(out);
            out = NULL;
        }
    }
    fclose(f);
    return out;
}

int hr_cache_dir(const char *kind, char *out, size_t room)
{
    const char *base = getenv("OPENSERVICE_CACHE");
    char tmp[512];

    if (base)
        snprintf(out, room, "%s/%s", base, kind);
    else if ((base = getenv("XDG_CACHE_HOME")) && *base)
        snprintf(out, room, "%s/openservice/%s", base, kind);
    else if ((base = getenv("HOME")) && *base)
        snprintf(out, room, "%s/.cache/openservice/%s", base, kind);
    else
        snprintf(out, room, "/tmp/openservice-%s-%u", kind, (unsigned)getuid());
    /* mkdir -p */
    snprintf(tmp, sizeof tmp, "%s", out);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0700);
            *p = '/';
        }
    return mkdir(tmp, 0700) == 0 || errno == EEXIST ? 0 : -1;
}

void hr_key(const uint8_t *d, uint32_t n, char *out, size_t room)
{
    uint64_t h = 1469598103934665603ULL;
    uint32_t i;

    for (i = 0; i < n; i++) {
        h ^= d[i];
        h *= 1099511628211ULL;
    }
    snprintf(out, room, "%016llx-%u", (unsigned long long)h, (unsigned)n);
}
