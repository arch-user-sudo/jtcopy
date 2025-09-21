// cpx.c
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

#define BUFFER_SIZE 8192

static size_t total_files = 0;
static size_t copied_files = 0;

void count_files(const char *path) {
    struct stat st;
    if (stat(path, &st) == -1) return;

    if (S_ISREG(st.st_mode)) {
        total_files++;
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char full[PATH_MAX];
            int rc = snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
            if (rc < 0 || (size_t)rc >= sizeof(full)) continue;
            count_files(full);
        }
        closedir(d);
    }
}

void show_progress(void) {
    if (total_files == 0) return;
    double progress = (double)copied_files / (double)total_files * 100.0;
    int bar_width = 40;
    int pos = (int)((progress / 100.0) * bar_width);

    printf("\r[");
    for (int i = 0; i < bar_width; ++i) {
        if (i < pos) putchar('=');
        else if (i == pos) putchar('>');
        else putchar(' ');
    }
    printf("] %6.2f%% (%zu/%zu files)", progress, copied_files, total_files);
    fflush(stdout);
}

int copy_file(const char *src, const char *dst) {
    FILE *fsrc = fopen(src, "rb");
    if (!fsrc) {
        fprintf(stderr, "open source '%s': %s\n", src, strerror(errno));
        return -1;
    }

    // open destination (assumes parent directory exists)
    FILE *fdst = fopen(dst, "wb");
    if (!fdst) {
        fprintf(stderr, "open dest '%s': %s\n", dst, strerror(errno));
        fclose(fsrc);
        return -1;
    }

    char buf[BUFFER_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
        size_t wrote = fwrite(buf, 1, n, fdst);
        if (wrote != n) {
            fprintf(stderr, "write error to '%s'\n", dst);
            fclose(fsrc);
            fclose(fdst);
            return -1;
        }
    }

    fclose(fsrc);
    fclose(fdst);

    copied_files++;
    show_progress();
    return 0;
}

int copy_dir(const char *src, const char *dst) {
    // create dst directory (ignore if exists)
    if (mkdir(dst, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir '%s': %s\n", dst, strerror(errno));
        return -1;
    }

    DIR *d = opendir(src);
    if (!d) {
        fprintf(stderr, "opendir '%s': %s\n", src, strerror(errno));
        return -1;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;

        char srcpath[PATH_MAX];
        char dstpath[PATH_MAX];
        int r1 = snprintf(srcpath, sizeof(srcpath), "%s/%s", src, e->d_name);
        int r2 = snprintf(dstpath, sizeof(dstpath), "%s/%s", dst, e->d_name);
        if (r1 < 0 || r1 >= (int)sizeof(srcpath) || r2 < 0 || r2 >= (int)sizeof(dstpath)) {
            fprintf(stderr, "path too long\n");
            continue;
        }

        struct stat st;
        if (stat(srcpath, &st) == -1) {
            fprintf(stderr, "stat '%s': %s\n", srcpath, strerror(errno));
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            // recurse into subdirectory (this will create dstpath inside)
            copy_dir(srcpath, dstpath);
        } else if (S_ISREG(st.st_mode)) {
            // regular file: copy (dst parent should exist because we create dst when entering directory)
            copy_file(srcpath, dstpath);
        } else {
            // skip other types (symlinks, sockets, etc.) for simplicity
        }
    }

    closedir(d);
    return 0;
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

    const char *src = argv[1];
    const char *dst = argv[2];

    struct stat st;
    if (stat(src, &st) == -1) {
        fprintf(stderr, "stat '%s': %s\n", src, strerror(errno));
        return 1;
    }

    // First pass: count files
    count_files(src);
    if (total_files == 0) {
        printf("No files to copy.\n");
        return 0;
    }

    // If source is directory, create dst/<basename(src)> and recurse into it
    if (S_ISDIR(st.st_mode)) {
        char src_trim[PATH_MAX];
        trim_trailing_slashes(src, src_trim, sizeof(src_trim));

        const char *base = strrchr(src_trim, '/');
        base = (base ? base + 1 : src_trim);

        char newdst[PATH_MAX];
        int rc = snprintf(newdst, sizeof(newdst), "%s/%s", dst, base);
        if (rc < 0 || rc >= (int)sizeof(newdst)) {
            fprintf(stderr, "destination path too long\n");
            return 1;
        }

        // create top-level folder once
        if (mkdir(newdst, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "mkdir '%s': %s\n", newdst, strerror(errno));
            return 1;
        }

        // recursive copy into newdst
        copy_dir(src, newdst);
    } else if (S_ISREG(st.st_mode)) {
        // source is a regular file: copy it directly to dst (dst can be a file path or directory)
        struct stat dstst;
        char dstpath[PATH_MAX];
        if (stat(dst, &dstst) == 0 && S_ISDIR(dstst.st_mode)) {
            // if dst is directory, append basename of src
            const char *base = strrchr(src, '/');
            base = (base ? base + 1 : src);
            int rc = snprintf(dstpath, sizeof(dstpath), "%s/%s", dst, base);
            if (rc < 0 || rc >= (int)sizeof(dstpath)) {
                fprintf(stderr, "destination path too long\n");
                return 1;
            }
        } else {
            // dst is intended as file path
            int rc = snprintf(dstpath, sizeof(dstpath), "%s", dst);
            if (rc < 0 || rc >= (int)sizeof(dstpath)) {
                fprintf(stderr, "destination path too long\n");
                return 1;
            }
        }
        // copy single file
        copy_file(src, dstpath);
    } else {
        fprintf(stderr, "unsupported source type\n");
        return 1;
    }

    printf("\nDone.\n");
    return 0;
}
