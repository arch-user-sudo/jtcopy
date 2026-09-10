// jtcopy.c
// Simple recursive copy with global progress (single-line updating bar).
// Minimal dependencies: standard C library only.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <signal.h>

#define BUFFER_SIZE (64 * 1024)
#define PROGRESS_DELTA (4ULL * 1024 * 1024)

static size_t total_files = 0;
static size_t copied_files = 0;
static size_t special_files = 0;
static unsigned long long total_bytes = 0;
static unsigned long long copied_bytes = 0;
static int error_count = 0;
static int progress_enabled = 0;
static int progress_printed = 0;
static volatile sig_atomic_t interrupted = 0;
static volatile sig_atomic_t caught_signal = 0;
static const char hide_cursor[] = "\x1b[?25l";
static const char show_cursor[] = "\x1b[?25h";

static void format_bytes(unsigned long long n, char *buf, size_t sz) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double val = (double)n;
    int unit = 0;
    while (val >= 1024.0 && unit < 4) {
        val /= 1024.0;
        unit++;
    }
    if (unit == 0)
        snprintf(buf, sz, "%llu B", n);
    else
        snprintf(buf, sz, "%.1f %s", val, units[unit]);
}

static int join_path(const char *a, const char *b, char *out, size_t sz) {
    int rc = snprintf(out, sz, "%s/%s", a, b);
    return (rc < 0 || (size_t)rc >= sz) ? -1 : 0;
}

static int same_file(const char *a, const char *b) {
    struct stat sa, sb;
    return stat(a, &sa) == 0 && stat(b, &sb) == 0 &&
           sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static int within(const char *root, const char *child) {
    size_t n = strlen(root);
    if (n == 1 && root[0] == '/') return 0;
    if (strncmp(root, child, n) != 0) return 0;
    return child[n] == '\0' || child[n] == '/';
}

static int nearest_real_dir(const char *dst, char *out) {
    char tmp[PATH_MAX];
    struct stat st;

    if (snprintf(tmp, sizeof(tmp), "%s", dst) >= (int)sizeof(tmp)) return 0;
    if (stat(tmp, &st) == 0 && S_ISDIR(st.st_mode)) {
        return realpath(tmp, out) != NULL;
    }

    for (;;) {
        char *slash = strrchr(tmp, '/');
        if (!slash) {
            if (snprintf(tmp, sizeof(tmp), ".") >= (int)sizeof(tmp)) return 0;
            break;
        }
        if (slash == tmp) {
            if (snprintf(tmp, sizeof(tmp), "/") >= (int)sizeof(tmp)) return 0;
            break;
        }
        *slash = '\0';
        if (stat(tmp, &st) == 0) break;
    }
    return realpath(tmp, out) != NULL;
}

void count_files(const char *path) {
    struct stat st;
    if (lstat(path, &st) == -1) return;

    if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
        total_files++;
        total_bytes += (unsigned long long)st.st_size;
        return;
    }

    if (!S_ISDIR(st.st_mode)) {
        special_files++;
        return;
    }

    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while (!interrupted && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char full[PATH_MAX];
        if (join_path(path, e->d_name, full, sizeof(full)) != 0) continue;
        count_files(full);
    }
    closedir(d);
}

static int term_width(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

void show_progress(void) {
    if (total_files == 0 || !progress_enabled) return;
    double progress;
    if (total_bytes > 0)
        progress = (double)copied_bytes / (double)total_bytes * 100.0;
    else
        progress = (double)copied_files / (double)total_files * 100.0;
    if (progress > 100.0) progress = 100.0;

    char a[32], b[32];
    format_bytes(copied_bytes, a, sizeof(a));
    format_bytes(total_bytes, b, sizeof(b));

    char info[96];
    int info_len = snprintf(info, sizeof(info), "%6.2f%% (%zu/%zu files, %s/%s)",
                            progress, copied_files, total_files, a, b);

    int bar_width = term_width() - (3 + info_len);
    if (bar_width > 0) {
        int pos = (int)((progress / 100.0) * bar_width);
        if (pos > bar_width - 1) pos = bar_width - 1;
        printf("\r[");
        for (int i = 0; i < bar_width; ++i) {
            if (i < pos) putchar('=');
            else if (i == pos) putchar('>');
            else putchar(' ');
        }
        printf("] %s\x1b[K", info);
    } else {
        printf("\r%s\x1b[K", info);
    }
    fflush(stdout);
    progress_printed = 1;
}

static void handle_signal(int sig) {
    if (interrupted) {
        if (progress_enabled) {
            ssize_t a = write(STDOUT_FILENO, show_cursor, sizeof(show_cursor) - 1);
            ssize_t b = write(STDOUT_FILENO, "\r\n", 2);
            (void)a;
            (void)b;
        }
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    caught_signal = sig;
    interrupted = 1;
    if (progress_enabled) {
        ssize_t ignored = write(STDOUT_FILENO, "\r\n", 2);
        (void)ignored;
    }
}

static void restore_cursor_handler(void) {
    if (progress_enabled) {
        ssize_t ignored = write(STDOUT_FILENO, show_cursor, sizeof(show_cursor) - 1);
        (void)ignored;
    }
}

static int interrupted_exit(void) {
    printf("Interrupted.\n");
    return 128 + (int)caught_signal;
}

int copy_file(const char *src, const char *dst) {
    struct stat st;
    if (lstat(src, &st) == -1) {
        fprintf(stderr, "stat '%s': %s\n", src, strerror(errno));
        error_count++;
        return -1;
    }

    if (S_ISLNK(st.st_mode)) {
        char target[PATH_MAX];
        ssize_t n = readlink(src, target, sizeof(target) - 1);
        if (n == -1) {
            fprintf(stderr, "readlink '%s': %s\n", src, strerror(errno));
            error_count++;
            return -1;
        }
        target[n] = '\0';
        if (symlink(target, dst) == -1) {
            if (errno == EEXIST) {
                if (unlink(dst) == 0 && symlink(target, dst) == 0) {
                    copied_bytes += (unsigned long long)st.st_size;
                    copied_files++;
                    show_progress();
                    return 0;
                }
            }
            fprintf(stderr, "symlink '%s': %s\n", dst, strerror(errno));
            error_count++;
            return -1;
        }
        copied_bytes += (unsigned long long)st.st_size;
        copied_files++;
        show_progress();
        return 0;
    }

    if (same_file(src, dst)) {
        fprintf(stderr, "'%s' and '%s' are the same file\n", src, dst);
        error_count++;
        return -1;
    }

    FILE *fsrc = fopen(src, "rb");
    if (!fsrc) {
        fprintf(stderr, "open source '%s': %s\n", src, strerror(errno));
        error_count++;
        return -1;
    }

    FILE *fdst = fopen(dst, "wb");
    if (!fdst) {
        fprintf(stderr, "open dest '%s': %s\n", dst, strerror(errno));
        fclose(fsrc);
        error_count++;
        return -1;
    }

    char buf[BUFFER_SIZE];
    size_t n;
    unsigned long long last_refresh = copied_bytes;
    while (!interrupted && (n = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
        size_t wrote = fwrite(buf, 1, n, fdst);
        if (wrote != n) {
            fprintf(stderr, "write error to '%s'\n", dst);
            fclose(fsrc);
            fclose(fdst);
            error_count++;
            return -1;
        }
        copied_bytes += n;
        if (copied_bytes - last_refresh >= PROGRESS_DELTA) {
            last_refresh = copied_bytes;
            show_progress();
        }
    }

    int failed = 0;
    if (interrupted) failed = 1;
    if (ferror(fsrc)) {
        fprintf(stderr, "read error from '%s': %s\n", src, strerror(errno));
        failed = 1;
    }
    if (fclose(fsrc) != 0) {
        fprintf(stderr, "close source '%s': %s\n", src, strerror(errno));
        failed = 1;
    }
    if (fclose(fdst) != 0) {
        fprintf(stderr, "close dest '%s': %s\n", dst, strerror(errno));
        failed = 1;
    }
    if (failed) {
        if (!interrupted) error_count++;
        return -1;
    }

    chmod(dst, st.st_mode & 07777);
    struct timespec times[2];
    times[0] = st.st_atim;
    times[1] = st.st_mtim;
    utimensat(AT_FDCWD, dst, times, 0);

    copied_files++;
    show_progress();
    return 0;
}

int copy_dir(const char *src, const char *dst) {
    struct stat st;
    if (lstat(src, &st) == -1) {
        fprintf(stderr, "stat '%s': %s\n", src, strerror(errno));
        error_count++;
        return -1;
    }

    if (mkdir(dst, st.st_mode & 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir '%s': %s\n", dst, strerror(errno));
        error_count++;
        return -1;
    }
    chmod(dst, st.st_mode & 07777);

    DIR *d = opendir(src);
    if (!d) {
        fprintf(stderr, "opendir '%s': %s\n", src, strerror(errno));
        error_count++;
        return -1;
    }

    struct dirent *e;
    int failed = 0;
    while (!interrupted && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;

        char srcpath[PATH_MAX];
        char dstpath[PATH_MAX];
        if (join_path(src, e->d_name, srcpath, sizeof(srcpath)) != 0 ||
            join_path(dst, e->d_name, dstpath, sizeof(dstpath)) != 0) {
            fprintf(stderr, "path too long\n");
            error_count++;
            failed = 1;
            continue;
        }

        struct stat st2;
        if (lstat(srcpath, &st2) == -1) {
            fprintf(stderr, "stat '%s': %s\n", srcpath, strerror(errno));
            error_count++;
            failed = 1;
            continue;
        }

        if (S_ISDIR(st2.st_mode)) {
            if (copy_dir(srcpath, dstpath) != 0) failed = 1;
        } else if (S_ISREG(st2.st_mode) || S_ISLNK(st2.st_mode)) {
            if (copy_file(srcpath, dstpath) != 0) failed = 1;
        } else {
            fprintf(stderr, "skipping '%s' (special file)\n", srcpath);
        }
    }

    closedir(d);
    return failed ? -1 : 0;
}

/* Trim trailing slashes from a path copy; result fits in buf (bufsz) */
void trim_trailing_slashes(const char *in, char *buf, size_t bufsz) {
    size_t len = strlen(in);
    while (len > 1 && in[len - 1] == '/') len--;
    if (len >= bufsz) len = bufsz - 1;
    memcpy(buf, in, len);
    buf[len] = '\0';
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <source> <destination>\n", argv[0]);
        return 1;
    }

    progress_enabled = isatty(fileno(stdout));

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (progress_enabled) {
        ssize_t ignored = write(STDOUT_FILENO, hide_cursor, sizeof(hide_cursor) - 1);
        (void)ignored;
        atexit(restore_cursor_handler);
    }

    const char *src = argv[1];
    const char *dst = argv[2];

    struct stat st;
    if (lstat(src, &st) == -1) {
        fprintf(stderr, "stat '%s': %s\n", src, strerror(errno));
        return 1;
    }

    // First pass: count files
    count_files(src);
    if (interrupted) return interrupted_exit();
    if (total_files == 0) {
        if (special_files > 0)
            fprintf(stderr, "No regular files to copy (%zu special file(s) skipped).\n", special_files);
        else
            printf("No files to copy.\n");
        return 0;
    }

    int rc = 0;
    if (S_ISDIR(st.st_mode)) {
        char src_trim[PATH_MAX];
        trim_trailing_slashes(src, src_trim, sizeof(src_trim));

        const char *base = strrchr(src_trim, '/');
        base = (base && base[1] ? base + 1 : src_trim);

        char newdst[PATH_MAX];
        struct stat dstst;
        size_t dl = strlen(dst);
        int dst_is_dir = (stat(dst, &dstst) == 0 && S_ISDIR(dstst.st_mode));
        int trailing = dl > 0 && dst[dl - 1] == '/';

        if (trailing || dst_is_dir) {
            // destination is (or is treated as) a directory: nest inside it
            char dst_trim[PATH_MAX];
            trim_trailing_slashes(dst, dst_trim, sizeof(dst_trim));
            if (join_path(dst_trim, base, newdst, sizeof(newdst)) != 0) {
                fprintf(stderr, "destination path too long\n");
                return 1;
            }
        } else if (stat(dst, &dstst) == 0) {
            fprintf(stderr, "destination '%s' exists and is not a directory\n", dst);
            return 1;
        } else {
            // nonexistent destination: use it as the copy root
            if (strlen(dst) >= sizeof(newdst)) {
                fprintf(stderr, "destination path too long\n");
                return 1;
            }
            strcpy(newdst, dst);
        }

        char src_real[PATH_MAX];
        char nd_real[PATH_MAX];
        if (realpath(src_trim, src_real) == NULL) {
            fprintf(stderr, "cannot resolve source '%s': %s\n", src, strerror(errno));
            return 1;
        }
        if (!nearest_real_dir(newdst, nd_real) || within(src_real, nd_real)) {
            fprintf(stderr, "destination '%s' is inside the source directory '%s'\n", newdst, src);
            return 1;
        }

        // create top-level folder once
        if (mkdir(newdst, st.st_mode & 0777) != 0 && errno != EEXIST) {
            fprintf(stderr, "mkdir '%s': %s\n", newdst, strerror(errno));
            return 1;
        }
        chmod(newdst, st.st_mode & 07777);

        // recursive copy into newdst
        if (copy_dir(src, newdst) != 0) rc = 1;
    } else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
        // source is a regular file or symlink: copy it directly to dst
        // (dst can be a file path or directory)
        struct stat dstst;
        char dstpath[PATH_MAX];
        if (stat(dst, &dstst) == 0 && S_ISDIR(dstst.st_mode)) {
            // if dst is directory, append basename of src
            const char *base = strrchr(src, '/');
            base = (base ? base + 1 : src);
            if (join_path(dst, base, dstpath, sizeof(dstpath)) != 0) {
                fprintf(stderr, "destination path too long\n");
                return 1;
            }
        } else {
            // dst is intended as file path
            if (snprintf(dstpath, sizeof(dstpath), "%s", dst) >= (int)sizeof(dstpath)) {
                fprintf(stderr, "destination path too long\n");
                return 1;
            }
        }
        if (copy_file(src, dstpath) != 0) rc = 1;
    } else {
        fprintf(stderr, "unsupported source type\n");
        return 1;
    }

    if (interrupted) return interrupted_exit();

    if (rc || error_count) {
        if (progress_printed) putchar('\n');
        printf("Done with %d error(s).\n", error_count);
        return 1;
    }
    if (progress_printed) putchar('\n');
    printf("Done.\n");
    return 0;
}