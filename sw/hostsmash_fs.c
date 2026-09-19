/*
 * This is free and unencumbered software released into the public domain.
 * See the LICENSE file for additional details.
 *
 * Designed by Chris Hooper in 2026.
 *
 * ---------------------------------------------------------------------
 *
 * Hostsmash File services for the Amiga.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/time.h>
#ifdef __MINGW32__
#include <sys/utime.h>
#define handle_t winhandle_t
#include <shlwapi.h>
#undef handle_t
typedef unsigned int uint;
#else
#include <sys/statvfs.h>
#endif
#include <sys/file.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <inttypes.h>
#include <dirent.h>

#include "../fw/smash_cmd.h"
#include "../amiga/host_cmd.h"
#include "hostsmash.h"
#include "hostsmash_fs.h"

/* AmigaOS FileInfoBlock Permissions */
#define FIBF_OTR_READ      0x00008000  // Other: file is readable
#define FIBF_OTR_WRITE     0x00004000  // Other: file is writable
#define FIBF_OTR_EXECUTE   0x00002000  // Other: file is executable
#define FIBF_OTR_DELETE    0x00001000  // Other: file may not be deleted
#define FIBF_GRP_READ      0x00000800  // Group: file is readable
#define FIBF_GRP_WRITE     0x00000400  // Group: file is writable
#define FIBF_GRP_EXECUTE   0x00000200  // Group: file is executable
#define FIBF_GRP_DELETE    0x00000100  // Group: file may not be deleted
#define FIBF_HOLD          0x00000080  // Keep pure module resident
#define FIBF_SCRIPT        0x00000040  // Executable script
#define FIBF_PURE          0x00000020  // Reentrant and re-executable
#define FIBF_ARCHIVE       0x00000010  // File has been archived
#define FIBF_READ          0x00000008  // Owner: file is readable
#define FIBF_WRITE         0x00000004  // Owner: file is writable
#define FIBF_EXECUTE       0x00000002  // Owner: file is executable
#define FIBF_DELETE        0x00000001  // Owner: file may not be deleted

#define SEEK_OFFSET_BEGINNING    (-1)
#define SEEK_OFFSET_CURRENT       (0)
#define SEEK_OFFSET_END           (1)

#define KS_PATH_MAX              4096  // Maximum length of file pathname

#ifdef OSX
#define lseek64 lseek
#define off64_t off_t

static void
timespec_to_timeval(struct timeval *tv, struct timespec *ts)
{
    tv->tv_sec = ts->tv_sec;
    tv->tv_usec = ts->tv_nsec / 1000;
}
#endif

#ifdef __MINGW32__
#define AT_FDCWD 0
#define AT_SYMLINK_NOFOLLOW 0

#define S_ISUID 0
#define S_ISGID 0
#define S_ISVTX 0

#define mkdir(path, mode) mkdir(path)
#define lstat(path, st) stat64(path, st)
#define stat(path, st) stat64(path, st)
#undef stat
#define stat stat64

enum {
    DT_UNKNOWN = 0,
    DT_FIFO = 1,
    DT_CHR = 2,
    DT_DIR = 4,
    DT_BLK = 6,
    DT_REG = 8,
    DT_LNK = 10,
    DT_SOCK = 12,
    DT_WHT = 14
};

char *realpath(const char *path, char *resolved_path);

ssize_t
readlink(const char *name, char *buf, size_t bufsize)
{
    (void) name;
    (void) buf;
    (void) bufsize;
    return (-1);
}

struct statvfs {
    unsigned long f_bsize;    // Filesystem block size
    unsigned long f_frsize;   // Fragment size
    unsigned long f_blocks;   // Size of fs in f_frsize units
    unsigned long f_bfree;    // Number of free blocks
    unsigned long f_bavail;   // Number of free blocks for unprivileged users
    unsigned long f_files;    // Number of inodes
    unsigned long f_ffree;    // Number of free inodes
    unsigned long f_favail;   // Number of free inodes for unprivileged users
    unsigned long f_fsid;     // Filesystem ID
    unsigned long f_flag;     // Mount flags
    unsigned long f_namemax;  // Maximum filename length
};

int
statvfs(const char *path, struct statvfs *st)
{
    int rc;
    ULARGE_INTEGER bytesfree_caller;
    ULARGE_INTEGER totalbytes;
    ULARGE_INTEGER bytesfree;

    rc = GetDiskFreeSpaceExA(path, &bytesfree_caller, &totalbytes, &bytesfree);
    if (rc == 0) {
#define BSIZE_SHIFT 20
        DWORD bsize = 1U << BSIZE_SHIFT;  // 1 MB
        uint64_t u_totalbytes = totalbytes.QuadPart;
        uint64_t u_bytesfree = bytesfree.QuadPart;
        uint64_t u_bytesfree_caller = bytesfree_caller.QuadPart;

        st->f_bsize = bsize;
        st->f_frsize = bsize;
        st->f_blocks = u_totalbytes >> BSIZE_SHIFT;
        st->f_bfree = u_bytesfree >> BSIZE_SHIFT;
        st->f_bavail = u_bytesfree_caller >> BSIZE_SHIFT;
    } else {
        DWORD csectors, ssize, bfree, blocks, bsize;
        rc = GetDiskFreeSpace(path, &csectors, &ssize, &bfree, &blocks);
        bsize = csectors * ssize;
        st->f_bsize = bsize;
        st->f_frsize = bsize;
        st->f_blocks = blocks;
        st->f_bfree = bfree;
        st->f_bavail = bfree;
    }
    if (rc != 0)
        return (rc);

    st->f_files = 0;           // inodes in use
    st->f_ffree = 0;           // free inodes
    st->f_favail = 0;          // free inodes for users
    st->f_fsid = 0;            // filesystem Id
    st->f_flag = 0;            // mount flags
    st->f_namemax = PATH_MAX;  // maximum filename length
    return (0);
}
#endif

static handle_ent_t *handle_list_head = NULL;
static handle_t      handle_unique = 0;
static handle_t      handle_default = 0;  // Volume directory is default

amiga_vol_t         *amiga_vol_head = NULL;

#define FILE_DEBUG
#ifndef FILE_DEBUG
#define fsprintf(args...) do { } while (0)
#endif

#ifdef FILE_DEBUG
ATTRIBUTE_PRINTF
int
fsprintf(const char *fmt, ...)
{
    int rc = 0;
    va_list args;

    if (debug_fs) {
        va_start(args, fmt);
        rc = vprintf(fmt, args);
        va_end(args);
    }

    return (rc);
}
#endif

static handle_ent_t *
handle_get(handle_t handle)
{
    handle_ent_t *node;
    if (handle == 0xffffffff)  // default can be specified with -M switch
        handle = handle_default;
    if (handle == 0)
        return (NULL);
    for (node = handle_list_head; node != NULL; node = node->he_next)
        if (node->he_handle == handle)
            return (node);
    fsprintf("Failed to find %x in handle list\n", handle);
    return (NULL);
}

static handle_ent_t *
handle_get_name(const char *name)
{
    handle_ent_t *node;
    for (node = handle_list_head; node != NULL; node = node->he_next)
        if ((node->he_name != NULL) && (strcmp(node->he_name, name) == 0))
            return (node);
    fsprintf("Failed to find \"%s\" in handle list\n", name);
    return (NULL);
}

static void
show_handle_count(const char *prefix)
{
#ifdef DEBUG_HANDLE_COUNT
    handle_ent_t *cur;
    uint count = 0;
    for (cur = handle_list_head; cur != NULL; cur = cur->he_next)
        count++;
    fsprintf("%s: handle count=%u\n", prefix, count);
#endif
}

static handle_ent_t *
handle_new(const char *name, const char *path, handle_ent_t *parent,
           uint type, uint mode)
{
    handle_t handle;
    handle_ent_t *node = malloc(sizeof (*node));
    if (node == NULL) {
        fsprintf("alloc %zu bytes failed\n", sizeof (*node));
        return (0);
    }
    memset(node, 0, sizeof (*node));
    handle = ++handle_unique;

    node->he_handle  = handle;
    node->he_name    = strdup(name);
    node->he_path    = strdup(path);
    node->he_type    = type;
    node->he_mode    = mode;
    node->he_count   = 1;
    node->he_entnum  = 0;
    node->he_dir     = NULL;
    node->he_next    = handle_list_head;
    handle_list_head = node;

    if (type == HM_TYPE_VOLUME) {
        node->he_volume  = node;  // This is the root of the volume
        node->he_avolume = NULL;  // Will be assigned later?
    } else if (parent != NULL) {
        /* Parent can be NULL if referencing the Volume Directory */
        node->he_volume  = parent->he_volume;
        node->he_avolume = parent->he_avolume;
    }
    show_handle_count("New");
    return (node);
}

#if 0
static int
is_same_path(const char *path1, const char *path2)
{
    size_t len1;
    size_t len2;

    /* Trim paths */
    path1 = trim_path(path1, &len1);
    path2 = trim_path(path2, &len2);

    fsprintf("compare '%.*s' %zu with '%.*s' %zu\n",
             (int)len1, path1, len1, (int)len2, path2, len2);
    if ((len1 == len2) && (strncmp(path1, path2, len1) == 0))
        return (1);
    return (0);
}
#endif


static uint
errno_to_km_status(void)
{
    switch (errno) {
        case EACCES:
        case EBUSY:
        case EFAULT:
        case EPERM:
        case EROFS:
            return (KM_STATUS_PERM);
        case EBADF:
        case EINVAL:
        case EISDIR:
            return (KM_STATUS_INVALID);
        case EEXIST:
            return (KM_STATUS_EXIST);
        case ENOENT:
            return (KM_STATUS_NOEXIST);
        case ENOTEMPTY:
            return (KM_STATUS_NOTEMPTY);
        default:
            fsprintf("errno=%d\n", errno);
            return (KM_STATUS_FAIL);
    }
}

static char *
make_amiga_relpath(handle_ent_t **parent, const char *name)
{
    char pathname[4096];
    const char *nptr = name;
    const char *nstart;
    char *tptr;
    char *colon;
    char *slash;

    if ((*parent != NULL) && ((*parent)->he_type == HM_TYPE_VOLDIR)) {
        /* Root of all volumes (Volume Directory) */
        *parent = NULL;
    }
    if ((nptr[0] == ':') && (nptr[1] == ':')) {
        /* Root of all volumes (Volume Directory) */
        *parent = NULL;
        nptr += 2;
    }
    nstart = nptr;
    if (nptr[0] == ':') {
        /* Root of current volume */
        if (*parent != NULL)
            *parent = (*parent)->he_volume;
        nptr++;
        nstart++;
    } else {
        colon = strchr(nstart, ':');
        slash = strchr(nstart, '/');
        if ((colon != NULL) && ((slash == NULL) || (colon < slash))) {
            /* Got a volume name ending in colon */
            uint len = colon - nstart + 1;
            memcpy(pathname, nstart, colon - nstart + 1);
            pathname[len] = '\0';
#undef REL_PATH_DEBUG
#ifdef REL_PATH_DEBUG
            fsprintf("find vol name %s\n", pathname);
#endif
            *parent = handle_get_name(pathname);
            if (*parent != NULL) {
                nptr += len;
            }
        }
    }

    /* Parent path */
    tptr = pathname;
    if ((*parent != NULL) && ((*parent)->he_type != HM_TYPE_VOLUME)) {
        strcpy(tptr, (*parent)->he_name);
        tptr += strlen(tptr);
        if ((tptr > pathname) && (tptr[-1] != '/') && (*nptr != '\0'))
            *(tptr++) = '/';
    }

    /* Build the new path */
    while (1) {
        if ((*nptr == '/') || (*nptr == '\0')) {
            /* End of a path element */
            if (*parent == NULL) {
                /* Seek volume name */
                tptr[0] = ':';
                tptr[1] = '\0';
                if (((pathname[0] == ':') && (pathname[1] == '\0')) ||
                    ((pathname[0] == '.') && (pathname[1] == ':') &&
                     (pathname[2] == '\0')) ||
                    ((pathname[0] == '.') && (pathname[1] == '.') &&
                     (pathname[2] == ':') && (pathname[3] == '\0'))) {
                    /* Got "" or "." or ".." at volume directory */
                    goto get_rel_restart;
                }
#ifdef REL_PATH_DEBUG
                fsprintf("Seek vol name %s\n", pathname);
#endif
                *parent = handle_get_name(pathname);
                if (*parent == NULL)
                    return (NULL);
                /* Got volume; start again with path name */
get_rel_restart:
                tptr = pathname;
get_rel_next:
                if (*nptr == '\0')
                    break;
                nptr++;
                continue;
            } else if ((*nptr == '/') && (tptr == pathname)) {
                /* Consume leading / */
#ifdef REL_PATH_DEBUG
                fsprintf("saw /\n");
#endif
                goto get_rel_next;
            } else if ((tptr > pathname) && (tptr[-1] == '.') &&
                       ((tptr == pathname + 1) || (tptr[-2] == '/'))) {
                /* Consume ./ meaning "same directory" */
#ifdef REL_PATH_DEBUG
                fsprintf("saw ./\n");
#endif
                tptr--;
                goto get_rel_next;
            } else if ((tptr > pathname) && (tptr[-1] == '/') &&
                                               (*nptr == '/')) {
                /* Consume // meaning "up a directory" for Amiga */
#ifdef REL_PATH_DEBUG
                fsprintf("saw //\n");
#endif
                goto get_rel_trim_dotdot;
            } else if ((tptr > pathname + 1) &&
                       (tptr[-1] == '.') && (tptr[-2] == '.') &&
                       ((tptr == pathname + 2) || (tptr[-3] == '/'))) {
                /* Consume .. and previous path element */
#ifdef REL_PATH_DEBUG
                fsprintf("saw ..\n");
#endif
                tptr -= 2;
get_rel_trim_dotdot:
                if ((tptr > pathname) && (--tptr > pathname)) {
                    for (tptr--; tptr > pathname; tptr--) {
                        if (*tptr == '/')
                            break;
                    }
                    if ((*tptr == '/') && (tptr > pathname))
                        tptr++;  // went too far
                }
                goto get_rel_next;
            }
        }
        if (*nptr == '\0')
            break;
        *(tptr++) = *(nptr++);
    }
    *tptr = '\0';

#ifdef REL_PATH_DEBUG
    fsprintf("got relative path %s from '%s' and '%s'\n",
             pathbuf, name, *parent ? (*parent)->he_name : "NULL");
#endif
    return (strdup(pathname));
}

/*
 * merge_host_paths is used to build a final path for file open.
 * It takes two paths and simply merges them together, inserting
 * a slash in the middle as appropriate.
 */
char *
merge_host_paths(const char *base, const char *append)
{
    char pathbuf[KS_PATH_MAX];
    uint len = strlen(base);
    if (len == 0)                   // no base
        return (strdup(append));
    if (strlen(append) == 0)        // no append
        return (strdup(base));
    if (append[0] == '/')           // append starts at root directory
        return (strdup(append));
    strcpy(pathbuf, base);
    if (base[len - 1] != '/') {
        if (base[len - 1] != ':') {
            /* Volume name plus some part of path; add a slash */
            pathbuf[len++] = '/';
        } else {
            /* Possiblly just the volume name */
            const char *ptr;
            /* Is the name just the volume root? */
            for (ptr = &base[len - 2]; ptr > base; ptr--)
                if (*ptr == ':') {
                    /* More than one colon present; add a slash */
                    pathbuf[len++] = '/';
                }
        }
    }
    strcpy(pathbuf + len, append);

#ifdef __MINGW32__
    /* Windows does not like to stat directory paths which end in '/' */
    char *ptr = pathbuf + strlen(pathbuf) - 1;
    if (*ptr == '/')
        *ptr = '\0';
#endif

    return (strdup(pathbuf));
}

/*
 * make_host_path is used to build a final path for file open.
 * It takes the volume path and simply concatenates the file
 * path, inserting a slash in the middle as appropriate.
 */
char *
make_host_path(amiga_vol_t *vol, const char *append)
{
    if (vol == NULL)
        return (strdup(append));

    return (merge_host_paths(vol->av_path, append));
}

char *
merge_amiga_paths(const char *base, const char *append)
{
    char pathbuf[2048];
    uint len;

    if (base == NULL)
        return (strdup(append));

    len = strlen(base);
    if (len == 0)
        return (strdup(append));
    if (strlen(append) == 0)
        return (strdup(base));
    strcpy(pathbuf, base);
    if ((base[len - 1] != '/') && (base[len - 1] != ':'))
        pathbuf[len++] = '/';
    strcpy(pathbuf + len, append);
    return (strdup(pathbuf));
}

static void
convert_host_path_to_amiga_path(char *path)
{
    char *ptr_s;
    char *ptr_e;
    char *ptr_copy = path;

    ptr_s = path;
    ptr_e = strchr(ptr_s, '/');
    while (ptr_e != NULL) {
        uint elen = ptr_e - ptr_s;
        if (strncmp(ptr_s, "..", elen) == 0) {
            *(ptr_copy++) = '/';
        } else {
            strncpy(ptr_copy, ptr_s, elen + 1);
            ptr_copy += elen + 1;
        }
        ptr_s = ptr_e + 1;
        ptr_e = strchr(ptr_s, '/');
    }
    if (ptr_s != ptr_copy)
        strcpy(ptr_copy, ptr_s);
}

/*
 * realpath_parent() will return an absolute path to the specified file,
 * even if it does not exist. The parent directory must exist, however.
 */
static char *
realpath_parent(char *path)
{
    char *eptr = path + strlen(path);
    char *rp;
    char *tmp;
    int   tlen;

    do {
        while (--eptr > path) {
            if (*eptr == '/')
                break;
        }
        if (eptr == path)
            return (strdup(path));

        *eptr = '\0';
        rp = realpath(path, NULL);
        *eptr = '/';
    } while (rp == NULL);

    tlen = strlen(rp);
    tmp = (char *) malloc(tlen + strlen(eptr) + 1);
    memcpy(tmp, rp, tlen);
    strcpy(tmp + tlen, eptr);
    free(rp);
    return (tmp);
}

static char *
make_host_relative_path(char *target_path, char *link_path)
{
    char         *target_path_save;
    char         *link_path_save;
    char         *target_next;
    char         *link_next = NULL;
    uint          pcount;

    fsprintf("start: tpath=%s lpath=%s\n", target_path, link_path);
    /*
     * realpath_parent() will convert the specified path to a
     * an absolute OS path. This is used to deal with cases
     * where new links which connect backward through symlinks might
     * result in a bad link.
     */
    target_path = realpath_parent(target_path);
    link_path   = realpath_parent(link_path);

    target_path_save = target_path;  // for later deallocate
    link_path_save = link_path;      // for later deallocate

    /*
     * link_path is the new link's location in the local filesystem.
     * target_path is the target file's path in the local filesystem.
     *
     * Now that these are known, trim target_path to be relative to
     * where link_path is in the local filesystem.
     * Common path elements are eliminated.
     */
    while (((target_next = strchr(target_path, '/')) != NULL) &&
           ((link_next = strchr(link_path, '/')) != NULL)) {
        uint tlen = target_next - target_path;
        uint llen = link_next - link_path;

        if (tlen != llen)
            break;  // Name lengths do not match
        if (strncmp(target_path, link_path, tlen) != 0)
            break;  // Path elements do not match

        target_path = target_next + 1;
        link_path = link_next + 1;
    }
    fsprintf("part 1: tpath=%s lpath=%s\n", target_path, link_path);

    /*
     * Count the number of additional path elements in the target path.
     */
    for (pcount = 0; link_next != NULL; pcount++) {
        link_next = strchr(link_path, '/');
        if (link_next == NULL)
            break;
        link_path = link_next + 1;
    }
    if (pcount > 0) {
        /*
         * For each additional path element in the target path,
         * prepend the link path with "../"
         */
        char *tp = malloc(strlen(target_path) + 1 + 3 * pcount);
        char *tp_tmp = tp;
        fsprintf("path elements to add: %d\n", pcount);
        while (pcount-- > 0) {
            strcpy(tp_tmp, "../");
            tp_tmp += 3;
        }
        strcpy(tp_tmp, target_path);
        target_path = tp;
    } else {
        target_path = strdup(target_path);
    }
    free(target_path_save);
    free(link_path_save);

    return (target_path);
}

char *
amiga_link_to_host_path(handle_ent_t *phandle, const char *apath,
                        char *link_path)
{
    handle_ent_t *handle = phandle;
    char         *target_path;
    char         *name;

    if ((name = make_amiga_relpath(&handle, apath)) == NULL) {
        fsprintf("link relative path failed for %s\n", apath);
        return (NULL);
    }
    target_path = make_host_path(phandle->he_avolume, name);
    free(name);

    name = make_host_relative_path(target_path, link_path);
    free(target_path);
    return (name);
}

static amiga_vol_t *
volume_get_by_path(const char *path, uint partial)
{
    amiga_vol_t *node;
    uint         len;

    for (node = amiga_vol_head; node != NULL; node = node->av_next) {
        if (partial) {
            len = strlen(node->av_path);
            if ((strncmp(node->av_path, path, len) == 0) &&
                ((path[len] == '\0') || (path[len] == '/')))
                return (node);

            len = strlen(node->av_realpath);
            if ((strncmp(node->av_realpath, path, len) == 0) &&
                ((path[len] == '\0') || (path[len] == '/')))
                return (node);
        } else {
            if ((strcmp(node->av_path, path) == 0) ||
                (strcmp(node->av_realpath, path) == 0))
                return (node);
        }
    }
    return (NULL);
}


/*
 * host_to_amiga_path() converts the specified host path in a
 * format which is acceptable as an Amiga path.
 *
 * Links which resolve to outside the exported volume will be mangled.
 * They could possibly be located if within another exported volume,
 * but that could be very complicated.
 */
static char *
host_to_amiga_path(char *hpath, char *npath, char *lpath)
{
    amiga_vol_t *vol_lpath;
    amiga_vol_t *vol_hpath;
    char *end;
    char *full_lpath;
    char *real_lpath;
    char *real_hpath;

    /*
     * Example:
     *    hpath = /home/cdh/projects/amiga_sdmac/k/ool
     *    lpath = ../../amiga_sdmac/z2
     *
     * This specific example is difficult because "k" is actually
     * a symlink outside the local filesystem. When "ool" was
     * created, it was a link through "k" which made its link
     * target also outside the local filesystem.
     *
     * If the link is within the exported volume, then simply
     * convert the host path elements ("../" to "/") and return
     * just that.
     *
     * Otherwise, if the link is outside the exported volume, then
     * do the following.
     * Resolve lpath
     *    hpath = /home/cdh/projects/amiga_sdmac/k      /ool
     *    lpath = /home/cdh/projects/amiga_sdmac/k/../../amiga_sdmac
     * to a real path.
     *    /home/cdh/projects/amiga_sdmac
     *
     * Then select the volume which best matches the link.
     * There could be multiple volumes which match, so choose the
     * one with the longest match.
     */

    for (end = hpath + strlen(hpath); end != hpath; end--)
        if (*end == '/')
            break;

    *end = '\0';
    full_lpath = merge_host_paths(hpath, lpath);
    *end = '/';
    real_lpath = realpath_parent(full_lpath);
    free(full_lpath);
    if (real_lpath == NULL) {
unresolved_link:
        /* Give up and just return the relative path of the unresolved link */
        return (strdup(lpath));
    }
    vol_lpath = volume_get_by_path(real_lpath, 1);
    if (vol_lpath == NULL) {
        /* Did not find path inside any exported volume */
        fsprintf("Link path %s not in any exported volume\n", real_lpath);
        free(real_lpath);
        goto unresolved_link;
    }

    /*
     * At this point, real_lpath is the host path to the destination file,
     * whether it exists or not.
     */

    real_hpath = realpath_parent(hpath);
    vol_hpath = volume_get_by_path(real_hpath, 1);

    if (vol_hpath == NULL) {
        /*
         * Did not find source link path inside any exported volume
         * This should not happen.
         */
        printf("BUG: Did not find source path for link %s\n", real_hpath);
        free(real_lpath);
        free(real_hpath);
        goto unresolved_link;
    }

    if (vol_lpath == vol_hpath) {
        /*
         * Within the same Amiga volume; create a relative path
         *
         * Start by using hpath to get the real path to the source link.
         */
        char *newpath;
        newpath = make_host_relative_path(real_lpath, real_hpath);
        fsprintf("newpath=%s\n", newpath);
        convert_host_path_to_amiga_path(newpath);
        return (newpath);
    }

    /*
     * Link target is not on the same volume.
     * Return a link target which is relative to the target volume root.
     */
    uint rlen = strlen(vol_lpath->av_realpath) + 1;
    char *merged = merge_amiga_paths(vol_lpath->av_volume, real_lpath + rlen);

    free(real_lpath);
    free(real_hpath);
    return (merged);
}

static uint
st_mode_to_hm_type(uint st_mode)
{
    uint hm_type;
    switch (st_mode & S_IFMT) {
        case S_IFBLK:
            hm_type = HM_TYPE_BDEV;
            break;
        case S_IFCHR:
            hm_type = HM_TYPE_CDEV;
            break;
        case S_IFDIR:
            hm_type = HM_TYPE_DIR;
            break;
        case S_IFIFO:
            hm_type = HM_TYPE_FIFO;
            break;
        case S_IFREG:
            hm_type = HM_TYPE_FILE;
            break;
#ifndef __MINGW32__
        case S_IFLNK:
            hm_type = HM_TYPE_LINK;
            break;
        case S_IFSOCK:
            hm_type = HM_TYPE_SOCKET;
            break;
#endif
        default:
            fsprintf("unknown dir type(%x)\n", st_mode & S_IFMT);
            hm_type = HM_TYPE_UNKNOWN;
            break;
    }
    return (hm_type);
}

static uint32_t
amiga_perms_from_host(uint host_perms)
{
    uint32_t perms;

    perms = ((host_perms & S_IRUSR) ? 0 : FIBF_READ) |
            ((host_perms & S_IWUSR) ? 0 : FIBF_WRITE |
                                          FIBF_DELETE) |
            ((host_perms & S_IXUSR) ? 0 : FIBF_EXECUTE) |

            ((host_perms & S_IRGRP) ? FIBF_GRP_READ    : 0) |
            ((host_perms & S_IWGRP) ? FIBF_GRP_WRITE |
                                      FIBF_GRP_DELETE  : 0) |
            ((host_perms & S_IXGRP) ? FIBF_GRP_EXECUTE : 0) |

            ((host_perms & S_IROTH) ? FIBF_OTR_READ    : 0) |
            ((host_perms & S_IWOTH) ? FIBF_OTR_WRITE |
                                      FIBF_OTR_DELETE  : 0) |
            ((host_perms & S_IXOTH) ? FIBF_OTR_EXECUTE : 0) |

            ((host_perms & S_ISUID) ? FIBF_HOLD   : 0) |  // SUID -> HOLD
            ((host_perms & S_ISGID) ? FIBF_PURE   : 0) |  // SGID -> PURE
            ((host_perms & S_ISVTX) ? FIBF_SCRIPT : 0);   // VTX  -> SCRIPT

    /*
     * Only the base R W E D bits are set = 1 to disable.
     * The rest of the Amiga bits are set = 1 to enable.
     *
     * There are not enough UNIX mode bits to support AMIGA_PERMS_ARCHIVE.
     *
     * Will Map:
     *     Set UID      -> HOLD (resident pure module stays in RAM)
     *     Set GID      -> PURE (re-entrant / re-executable program)
     *     VTX (sticky) -> SCRIPT
     *
     * chmod u+s - set uid (SUID) for HOLD (keep resident modules in memory)
     * chmod g+s - set group id (SGID) for PURE (re-entrant/re-executable)
     * chmod +t  - set sticky (VTX) for SCRIPT
     */

    return (perms);
}

static uint
host_perms_from_amiga(uint amiga_perms)
{
    /* See comments in amiga_perms_from_host() for more information */

    uint32_t perms;

    perms = ((amiga_perms & FIBF_READ)        ? 0 : S_IRUSR) |
            ((amiga_perms & FIBF_WRITE)       ? 0 : S_IWUSR) |
            ((amiga_perms & FIBF_EXECUTE)     ? 0 : S_IXUSR) |

            ((amiga_perms & FIBF_GRP_READ)    ? S_IRGRP : 0) |
            ((amiga_perms & FIBF_GRP_WRITE)   ? S_IWGRP : 0) |
            ((amiga_perms & FIBF_GRP_EXECUTE) ? S_IXGRP : 0) |

            ((amiga_perms & FIBF_OTR_READ)    ? S_IROTH : 0) |
            ((amiga_perms & FIBF_OTR_WRITE)   ? S_IWOTH : 0) |
            ((amiga_perms & FIBF_OTR_EXECUTE) ? S_IXOTH : 0) |

            ((amiga_perms & FIBF_HOLD)        ? S_ISUID : 0) |
            ((amiga_perms & FIBF_PURE)        ? S_ISGID : 0) |
            ((amiga_perms & FIBF_SCRIPT)      ? S_ISVTX : 0);

    /* No place to capture AMIGA_PERMS_ARCHIVE */
    return (perms);
}

static uint
amiga_perms_from_str(const char *aperms)
{
    const char *ptr;
    uint mask = 0;

    /* Check for "hsparwed" | "x" SetProtect format */
    for (ptr = aperms; *ptr != '\0'; ptr++) {
        if (*ptr != '-') {
            static const char permstr[] = "hsparwedx";
            char *pos = strchr(permstr, *ptr);
            uint  bit;
            if (pos == NULL)
                return (0xffffffff);
            bit = pos - permstr;
            if (bit == 8)
                bit = 1;  // 'x' is the same as 'e'
            else
                bit = 7 - bit;
            mask |= BIT(bit);
        }
    }
    mask ^= (FIBF_READ | FIBF_WRITE | FIBF_EXECUTE | FIBF_DELETE);
    return (mask);
}


void
volume_add(const char *volume_name, const char *local_path, uint is_default)
{
    handle_ent_t *handle;
    amiga_vol_t  *node = malloc(sizeof (amiga_vol_t));
    char *vpos;
    char volnamebuf[128];
    uint volnamelen = strlen(volume_name);
    uint flags = 0;
    int  bootpri = 0;

    if (volnamelen > sizeof (volnamebuf) - 2)
        errx(EXIT_FAILURE, "Volume name '%s' too long\n", volume_name);
    if (strchr(volume_name, '/') != NULL)
        errx(EXIT_FAILURE, "Volume name '%s' may not contain '/'\n",
             volume_name);

    strcpy(volnamebuf, volume_name);
    vpos = strchr(volnamebuf, ':');
    if ((vpos != NULL) && ((vpos - volume_name) < volnamelen - 1)) {
        /* Volume Flags were specified */
        char *start = vpos + 1;
        char *end;
        while (*start != '\0') {
            char ch;
            end = strchr(start, ',');
            if (end == NULL)
                end = strchr(start, '\0');
            ch = *end;
            *end = '\0';
            fsprintf("flag '%s'\n", start);
            if (strncmp(start, "bootpri=", 8) == 0) {
                if ((sscanf(start + 8, "%i", &bootpri) != 1) ||
                    (bootpri > 127) || (bootpri < -128))  {
                    errx(EXIT_FAILURE,
                         "Invalid boot priority %s", start + 8);
                }
                flags |= AV_FLAG_BOOTABLE;
            } else {
                errx(EXIT_FAILURE,
                     "\"-m %s\" unknown flag \"%s\"\n"
                     "Use one of\n"
                     "    bootpri=<num> - make partition bootable",
                     volume_name, start);
            }
            *end = ch;
            if (ch == '\0')
                start = end;
            else
                start = end + 1;
        }
        vpos[1] = '\0';
    }

    if (vpos == NULL) {
        /* Append colon */
        volnamebuf[volnamelen++] = ':';
        volnamebuf[volnamelen] = '\0';
    }

    handle = handle_new(volnamebuf, "", NULL, HM_TYPE_VOLUME, HM_MODE_READ);

    node->av_volume    = strdup(volnamebuf);
    node->av_path      = local_path;
    node->av_realpath  = realpath(local_path, NULL);
    node->av_handle    = handle;
    node->av_next      = amiga_vol_head;
    node->av_flags     = flags;
    node->av_bootpri   = bootpri;
    amiga_vol_head     = node;

    handle->he_avolume = node;
    handle->he_volume  = handle;  // This is the volume's root handle

    if (is_default) {
        /*
         * This is the optional default parent handle when the Amiga
         * specifies a handle of 0xffffffff.
         */
        handle_default = handle->he_handle;
    }

    fsprintf("add volume %s = %s\n", volnamebuf, local_path);
}

static amiga_vol_t *
volume_get_by_handle(handle_ent_t *handle)
{
    amiga_vol_t *node;
    for (node = amiga_vol_head; node != NULL; node = node->av_next)
        if (node->av_handle == handle)
            return (node);
    fsprintf("Could not locate handle %x in volume list\n", handle->he_handle);
    return (NULL);
}

static amiga_vol_t *
volume_get_by_index(uint index)
{
    amiga_vol_t *node;
    uint count = 0;
    for (node = amiga_vol_head; node != NULL; node = node->av_next)
        if (count++ == index)
            return (node);
    return (NULL);
}

#if 0
static void
handle_list_show(void)
{
    char *type;
    handle_ent_t *node;
    fsprintf("    Type Handle FD D Path               APath              "
             "HPath\n");
    for (node = handle_list_head; node != NULL; node = node->he_next) {
        switch (node->he_type) {
            default:
            case HM_TYPE_UNKNOWN:
                type = "UNKNOWN";
                break;
            case HM_TYPE_FILE:
                type = "FILE";
                break;
            case HM_TYPE_DIR:
                type = "DIR";
                break;
            case HM_TYPE_LINK:
                type = "LINK";
                break;
            case HM_TYPE_BDEV:
                type = "BDEV";
                break;
            case HM_TYPE_CDEV:
                type = "CDEV";
                break;
            case HM_TYPE_FIFO:
                type = "FIFO";
                break;
            case HM_TYPE_SOCKET:
                type = "SOCKET";
                break;
            case HM_TYPE_WHTOUT:
                type = "WHTOUT";
                break;
            case HM_TYPE_VOLUME:
                type = "VOLUME";
                break;
            case HM_TYPE_VOLDIR:
                type = "VOLDIR";
                break;
        }
        fsprintf(" %7s %6x %2d %s %-18s\n",
                 type, node->he_handle, node->he_fd,
                 node->he_dir ? "Y" : " ", node->he_name);
    }
}
#endif


static void
handle_free(handle_t handle)
{
    handle_ent_t *parent = NULL;
    handle_ent_t *node;
    for (node = handle_list_head; node != NULL; node = node->he_next) {
        if (node->he_handle == handle) {
            node->he_count--;
            if (node->he_count != 0)
                return;
            if (parent == NULL)
                handle_list_head = node->he_next;
            else
                parent->he_next = node->he_next;
            free(node->he_path);
            free(node->he_name);
            free(node);
            show_handle_count("Free");
            return;
        }
        parent = node;
    }
    fsprintf("Failed to find %x in handle list for free\n", handle);
}


uint
sm_fopen(hm_fopenhandle_t *hm, uint *status)
{
    char         *hm_name = (char *)(hm + 1);
    handle_ent_t *phandle = handle_get(hm->hm_handle);
    handle_ent_t *handle;
    char         *host_path = NULL;
    char         *name = NULL;
    uint16_t      hm_type;
    uint16_t      hm_mode = SWAP16(hm->hm_mode);
    uint          oflags;
    int           fd;
    struct stat   st;

    fsprintf("fopen(%s %x) in %x\n", hm_name, hm_mode, hm->hm_handle);

    hm->hm_hdr.km_op |= KM_OP_REPLY;
    hm->hm_hdr.km_status = KM_STATUS_OK;

#ifdef FOPEN_DEBUG
    fsprintf("parent handle=%x type=%x path=%s\n",
             hm->hm_handle, phandle ? phandle->he_type: 0,
             phandle ? phandle->he_name : NULL);
#endif
    if ((hm_name[0] == '\0') && (hm_mode == 0)) {
        /* Special case -- want to reopen handle with same mode */
//      hm_mode = phandle->he_mode;
        hm_mode = HM_MODE_READ;
        // XXX: I'm not sure this is enough because for the new file,
        //      phandle could end up being a file, and this might break
        //      future reopens. Need to test that.
    }
    if ((name = make_amiga_relpath(&phandle, hm_name)) == NULL) {
        fsprintf("fopen(%s) relative path failed\n", hm_name);
reply_open_fail:
        if (name != NULL)
            free(name);
        hm->hm_handle = 0;
        if (hm->hm_hdr.km_status == KM_STATUS_OK)
            hm->hm_hdr.km_status = KM_STATUS_NOEXIST;
        return (send_msg(hm, sizeof (*hm), status));
    }

    if (phandle == NULL) {
        /* Opening the volume directory */
        if ((hm_mode & HM_MODE_READ) == 0) {
            fsprintf("Did not open volume directory for read (%x)\n",
                     hm_mode);
            hm->hm_hdr.km_status = KM_STATUS_INVALID;
            goto reply_open_fail;
        }
        hm->hm_type = SWAP16(HM_TYPE_DIR);

        handle = handle_new(name, "", NULL, HM_TYPE_VOLDIR, hm_mode);
        /* Volume directory has no he_volume or he_avolume pointers */
        free(name);
        goto open_success;
    }
    host_path = make_host_path(phandle->he_avolume, name);

    fsprintf("host_path=%s\n", host_path);

    hm_type = SWAP16(hm->hm_type);
    if (hm_mode & HM_MODE_READ) {
        /* File is opened for read; attempt to figure out file type */
        if (hm_mode & HM_MODE_NOFOLLOW) {
            if (lstat(host_path, &st) != 0) {
                if (hm_mode & HM_MODE_CREATE)
                    goto could_not_stat;
                fsprintf("fopen(%s) lstat fail errno=%d\n", host_path, errno);
                goto reply_open_fail;
            }
        } else {
            if (stat(host_path, &st) != 0) {
                if (hm_mode & HM_MODE_CREATE)
                    goto could_not_stat;
                fsprintf("fopen(%s) stat fail errno=%d\n", host_path, errno);
                goto reply_open_fail;
            }
        }
        hm_type = st_mode_to_hm_type(st.st_mode);
        hm->hm_type = SWAP16(hm_type);
    }

could_not_stat:
    if ((hm_mode & HM_MODE_LINK) ||
        (hm_mode & HM_MODE_DIR)) {
        /*
         * Special open mode which returns directory or link target
         * information for a single file.
         */
        if ((hm_mode & HM_MODE_RDWR) != HM_MODE_READ) {
            fsprintf("Did not open dirent %s for read (%x)\n",
                     host_path, hm_mode);
            hm->hm_hdr.km_status = KM_STATUS_INVALID;
            goto reply_open_fail;
        }
        fsprintf("handle_new name='%s' hm_name='%s' host_path='%s'\n",
                 name, hm_name, host_path);
        handle = handle_new(name, host_path, phandle, hm_type, hm_mode);
        fsprintf("dirmode phandle %x handle %x avolume=%s name=%s\n",
                 (phandle != NULL) ? phandle->he_handle : 0,
                 handle->he_handle,
                 (handle->he_avolume != NULL) ? handle->he_avolume->av_volume :
                 "(NULL)", name);
        free(name);
        goto open_success;
    } else if (hm_type == HM_TYPE_DIR) {
        DIR *dir;
        if ((hm_mode & ~HM_MODE_DIR) != HM_MODE_READ) {
            fsprintf("Did not open dir %s for read (%x)\n",
                     host_path, hm_mode);
            hm->hm_hdr.km_status = KM_STATUS_INVALID;
            goto reply_open_fail;
        }
        dir = opendir(host_path);
        if (dir == NULL) {
            fsprintf("opendir(%s) failed\n", host_path);
            goto reply_open_fail;
        }

        handle = handle_new(name, "", phandle, hm_type, hm_mode);
        handle->he_dir = dir;
        fsprintf("  opendir(%s \"%s\") = %x %p\n",
                 host_path, name, handle->he_handle, (void *) dir);
        free(name);
        goto open_success;
    }
    switch (hm_mode & HM_MODE_RDWR) {
        case HM_MODE_READ:
            oflags = O_RDONLY;
            break;
        case HM_MODE_WRITE:
            oflags = O_WRONLY;
            break;
        case HM_MODE_RDWR:
            oflags = O_RDWR;
            break;
        default:
            oflags = 0;
            break;
    }
    if (hm_mode & HM_MODE_APPEND)
        oflags |= O_APPEND;
    if (hm_mode & HM_MODE_CREATE)
        oflags |= O_CREAT;
    if (hm_mode & HM_MODE_TRUNC)
        oflags |= O_TRUNC;
#ifdef __MINGW32__
    oflags |= O_BINARY;
#endif

    if (oflags & O_CREAT) {
        uint32_t aperms = SWAP32(hm->hm_aperms);
        uint mode = host_perms_from_amiga(aperms);
        fsprintf("O_CREAT %s oflags=%x mode=%x\n", host_path, oflags, mode);
        fd = open(host_path, oflags, mode);
    } else {
        fd = open(host_path, oflags);
        if ((fd == -1) && (oflags & HM_MODE_WRITE)) {
            fd = open(host_path, oflags | O_CREAT, 0777);
        }
    }
    if (fd == -1) {
        fsprintf("File open %s fail: %d\n", host_path, errno);
        hm->hm_hdr.km_status = errno_to_km_status();
        goto reply_open_fail;
    }

    handle = handle_new(name, "", phandle, hm_type, hm_mode);
    handle->he_fd = fd;
    free(name);

open_success:
    hm->hm_hdr.km_status = KM_STATUS_OK;
    hm->hm_handle = handle->he_handle;
    hm->hm_mode   = 0;
    fsprintf("  handle=%x\n", hm->hm_handle);
    return (send_msg(hm, sizeof (*hm), status));
}

uint
sm_fclose(hm_fopenhandle_t *hm, uint *status)
{
    handle_ent_t *handle = handle_get(hm->hm_handle);

    if (handle == NULL) {
        fsprintf("Handle %x not open for close\n", hm->hm_handle);
        hm->hm_hdr.km_status = KM_STATUS_FAIL;
        return (send_msg(hm, sizeof (*hm), status));
    }

#define DEBUG_CLOSE
#ifdef DEBUG_CLOSE
    fsprintf("fclose(%x): ", hm->hm_handle);
#endif
    hm->hm_hdr.km_status = KM_STATUS_OK;
    hm->hm_hdr.km_op |= KM_OP_REPLY;

    if (handle->he_mode & HM_MODE_LINK) {
#ifdef DEBUG_CLOSE
        fsprintf("close readlink '%s'\n", handle->he_name);
#endif
        goto sm_fclose_end;
    }
    if (handle->he_mode & HM_MODE_DIR) {
#ifdef DEBUG_CLOSE
        fsprintf("close STAT '%s'\n", handle->he_name);
#endif
        goto sm_fclose_end;
    }
    switch (handle->he_type) {
        case HM_TYPE_VOLDIR:
#ifdef DEBUG_CLOSE
            fsprintf("close volume directory\n");
#endif
            break;
        case HM_TYPE_VOLUME:
#ifdef DEBUG_CLOSE
            fsprintf("close volume '%s'\n", handle->he_name);
#endif
            break;
        case HM_TYPE_DIR:
            if (handle->he_dir == NULL) {
                fsprintf("BUG: attempt close of NULL dir: %s\n",
                         handle->he_name);
                break;
            }
#ifdef DEBUG_CLOSE
            fsprintf("close dir %p\n", (void *) handle->he_dir);
#endif
            if (handle->he_dir != NULL)
                closedir(handle->he_dir);
            break;
        default:
#ifdef DEBUG_CLOSE
            fsprintf("close file '%s'\n", handle->he_name);
#endif
            close(handle->he_fd);
            break;
    }
sm_fclose_end:
    handle_free(hm->hm_handle);
    return (send_msg(hm, sizeof (*hm), status));
}

static uint64_t
get_fs_size(const char *path, uint64_t *used, uint *blksize)
{
    struct statvfs buf;
    statvfs(path, &buf);
    *used = buf.f_blocks - buf.f_bavail;
    *blksize = buf.f_bsize;
#ifdef DEBUG_STATVFS
    fsprintf("statvfs path='%s' blks=%u used=%u blksize=%u\n",
             path, (uint) buf.f_blocks, (uint) *used, (uint) *blksize);
#endif
    return (buf.f_blocks);
}

uint
sm_fread(hm_freadwrite_t *hm, uint *status)
{
    hm_freadwrite_t *hmr;
    uint             rc;
    uint             pos;
    uint             len;
    uint             hm_length = SWAP32(hm->hm_length);
    uint             hm_flag = SWAP16(hm->hm_flag);
    handle_ent_t    *handle = handle_get(hm->hm_handle);
    uint             pathlen = 0;
    char             pathbuf[2048];
    ssize_t          readcnt = 0;

    hm->hm_hdr.km_op |= KM_OP_REPLY;

#ifdef FILE_DEBUG
    off64_t          fpos = 0;
    if ((hm_flag & HM_FLAG_SEEK0) == 0) {
        if (handle != NULL) {
            fpos = lseek64(handle->he_fd, 0, SEEK_CUR);
            if (fpos < 0)
                fpos = 0;
        }
    }
    fsprintf("fread(%x p=%jx l=%x)\n",
             hm->hm_handle, (intmax_t) fpos, hm_length);
#endif
    if (handle == NULL) {
        fsprintf("handle get %x failed\n", hm->hm_handle);
reply_read_fail:
        if (hm->hm_hdr.km_status == KM_STATUS_OK)
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
        return (send_msg(hm, sizeof (*hm), status));
    }
    if ((handle->he_mode & HM_MODE_READ) == 0) {
        fsprintf("%s not opened for read mode: %x\n",
                 handle->he_name, handle->he_mode);
        hm->hm_hdr.km_status = KM_STATUS_INVALID;
        goto reply_read_fail;
    }

    if (handle->he_type == HM_TYPE_VOLDIR) {
        if (hm_flag & HM_FLAG_SEEK0)
            handle->he_entnum = 0;
#ifdef DEBUG_READ
        fsprintf("VOLDIR %s\n", handle->he_name);
#endif
        goto dir_read_common;
    } else if (handle->he_type == HM_TYPE_DIR) {
#ifdef DEBUG_READ
        fsprintf("DIR %s\n", handle->he_name);
#endif
        if (hm_flag & HM_FLAG_SEEK0) {
            if (handle->he_dir != NULL)
                rewinddir(handle->he_dir);
        }
        goto dir_read_common;
    } else if (handle->he_mode & HM_MODE_DIR) {
        if (hm_flag & HM_FLAG_SEEK0)
            handle->he_entnum = 0;
#ifdef DEBUG_READ
        fsprintf("STAT %s\n", handle->he_name);
#endif
dir_read_common:
        pathlen = strlen(handle->he_name);
        if (pathlen > sizeof (pathbuf) - 257) {
            fsprintf("Path too long: %u bytes\n", pathlen);
            goto reply_read_fail;
        }
        strcpy(pathbuf, handle->he_name);
        if ((handle->he_mode & HM_MODE_DIR) &&
            (handle->he_type != HM_TYPE_VOLDIR)) {
            char *sname = pathbuf;
            /* Trim file name from path */
            while (*sname != '\0')
                sname++;
            for (sname--; sname > pathbuf; sname--)
                if (*sname == '/')
                    break;
                else
                    *sname = '\0';
            pathlen = strlen(pathbuf);
        } else if ((pathlen > 1) && (pathbuf[pathlen - 1] != '/')) {
            pathbuf[pathlen++] = '/';  // Append trailing slash
            pathbuf[pathlen] = '\0';
        }
#ifdef DEBUG_READ
        fsprintf("STAT pathbuf=%s\n", pathbuf);
#endif
    }

    /*
     * Allocate buffer larger than requested. This is to accommodate
     * the specific case of short read where a single directory entry
     * is desired.
     */
    hmr = malloc(sizeof (*hmr) + hm_length + 256);
    pos = 0;
    rc = 0;
    while (pos < hm_length) {
        uint8_t *ndata = ((uint8_t *)(hmr + 1)) + pos;
        len = hm_length - pos;
        if (handle->he_mode & HM_MODE_LINK) {
            char  lbuf[PATH_MAX];
            char *path;
            int   llen;

            fsprintf("readlink %s\n", handle->he_path);
            llen = readlink(handle->he_path, lbuf, sizeof (lbuf) - 1);
            if (llen == -1) {
                fsprintf("readlink %s failed\n", handle->he_path);
                llen = 0;
            }
            lbuf[llen] = '\0';
            path = host_to_amiga_path(handle->he_path, handle->he_name, lbuf);

            llen = strlen(path) + 1;
            memcpy(ndata, path, llen);
            free(path);

            pos += llen;
            break;
        } else if ((handle->he_type == HM_TYPE_DIR) ||
                   (handle->he_type == HM_TYPE_VOLDIR) ||
                   (handle->he_mode & HM_MODE_DIR)) {
            struct dirent *dp;
            char *nptr;
            uint nlen;
            uint hmd_type;
            uint he_mode = handle->he_mode;
            uint32_t size_hi = 0;
            uint32_t size_lo = 0;
            uint32_t amiga_perms;
            struct stat st;
            static struct dirent ldp;
            hm_fdirent_t *hm_dirent = (hm_fdirent_t *)ndata;
            uint maxlen = hm_length - pos;
            char *host_path = NULL;
            char d_name[256];
            uint d_type = 0;

            if (pos > hm_length)  // Safeguard
                maxlen = 0;
            if ((sizeof (*hm_dirent) +
                 sizeof (d_name) + 2 > maxlen) && (pos > 0)) {
                /*
                 * Next entry might not fit, so stop here.
                 *
                 * Note this only applies to subsequent directory entries.
                 * At least one dirent will always be retrieved. The caller
                 * to sm_fread() can * acquire a single dirent by
                 * requesting a small buffer, for example the size of
                 * hm_fdirent_t, and will get that including an untruncated
                 * filename. This might seem bad (buffer overrun), but it
                 * isn't because the receiving Amiga code will land data in
                 * an oversized buffer.
                 */
                rc = 0;
#ifdef DEBUG_READ
                fsprintf("No space for next dirent %u %u\n",
                         (uint) sizeof (*dp), maxlen);
#endif
                break;
            }

            memset(hm_dirent, 0, sizeof (*hm_dirent)); // wipe the dirent

            if (handle->he_type == HM_TYPE_VOLDIR) {
                amiga_vol_t *vol;

                vol = volume_get_by_index(handle->he_entnum);
                if (vol == NULL) {
                    dp = NULL;
                } else {
                    dp = &ldp;
                    d_type = DT_DIR;
                    strcpy(d_name, vol->av_volume);
                    if (handle->he_mode & HM_MODE_DIR) {
                        if (handle->he_entnum == 0) {
                            strcpy(d_name, "Volume Directory");
                        } else {
                            dp = NULL;
                        }
                    }
                }
            } else if (handle->he_mode & HM_MODE_DIR) {
                /* This mode is to STAT a single file */
                if (handle->he_entnum != 0) {
                    dp = NULL;
                } else {
                    const char *sname;

                    handle->he_entnum++;
                    if ((handle->he_name[0] == '\0') ||
                        ((handle->he_name[0] == '.') &&
                         (handle->he_name[1] == '\0'))) {
                        /* Volume root */
                        sname = handle->he_avolume->av_volume;
                        handle->he_type = HM_TYPE_VOLUME;
                        hmd_type = HM_TYPE_VOLUME;
                    } else {
                        /* Find name following directory path */
                        sname = handle->he_name;

                        while (*sname != '\0')
                            sname++;
                        if ((sname > handle->he_name) &&
                            (sname[-1] == '/')) {
                            /* Name is before trailing slash */
                            sname--;
                        }
                        for (sname--; sname > handle->he_name; sname--)
                            if (sname[-1] == '/')
                                break;
                    }
                    dp = &ldp;
                    d_type = DT_REG;
                    dp->d_ino = 0;
                    strcpy(d_name, sname);
                }
            } else {
                uint skip = 0;
                do {
                    if (handle->he_dir == NULL) {
                        printf("NULL dir handle for %x\n", handle->he_handle);
                        dp = NULL;
                    } else {
                        dp = readdir(handle->he_dir);
                    }
                    skip = 0;
                    if (dp != NULL) {
                        char *end;
                        strcpy(d_name, dp->d_name);
#ifdef __MINGW32__
                        d_type = DT_UNKNOWN;
#else
                        d_type = dp->d_type;
#endif

                        /* Skip .uaem files */
                        end = d_name + strlen(d_name);
                        if (((end - d_name) >= 6) &&
                            (strcmp(end - 5, ".uaem") == 0)) {
                            skip = 1;
                        }

#define IS_DOT(x)     (((x)[0] == '.') && ((x)[1] == '\0'))
#define IS_DOT_DOT(x) (((x)[0] == '.') && ((x)[1] == '.') && ((x)[2] == '\0'))
                        /* Skip . and .. files */
                        if (IS_DOT(d_name) || IS_DOT_DOT(d_name)) {
                            skip = 1;
                        }
                    }
                } while (skip);
                he_mode |= HM_MODE_NOFOLLOW;
            }
            if (dp == NULL) {
                rc = KM_STATUS_EOF;  // end of directory
                break;
            }
            strcpy(pathbuf + pathlen, d_name);
            switch (d_type) {
                default:
                case DT_UNKNOWN:
                    hmd_type = HM_TYPE_UNKNOWN;
                    break;
                case DT_FIFO:
                    hmd_type = HM_TYPE_FIFO;
                    break;
                case DT_CHR:
                    hmd_type = HM_TYPE_CDEV;
                    break;
                case DT_DIR:
                    hmd_type = HM_TYPE_DIR;
                    break;
                case DT_BLK:
                    hmd_type = HM_TYPE_BDEV;
                    break;
                case DT_REG:
                    hmd_type = HM_TYPE_FILE;
                    break;
                case DT_LNK:
                    hmd_type = HM_TYPE_LINK;
                    break;
                case DT_SOCK:
                    hmd_type = HM_TYPE_SOCKET;
                    break;
                case DT_WHT:
                    hmd_type = HM_TYPE_WHTOUT;
                    break;
            }

            if ((handle->he_type == HM_TYPE_VOLDIR) ||
                (handle->he_type == HM_TYPE_VOLUME)) {
                amiga_vol_t *vol;
                uint64_t    fs_size;
                uint64_t    fs_used;
                uint        fs_blksize;
                const char *path;
                time_t      utctime = time(NULL);
                time_t      time_a  = get_localtime(utctime);
                hmd_type = HM_TYPE_VOLDIR;
                hm_dirent->hmd_atime = SWAP32(time_a);
                hm_dirent->hmd_ctime = SWAP32(time_a);
                hm_dirent->hmd_mtime = SWAP32(time_a);
                hm_dirent->hmd_mode = SWAP32(S_IFDIR|S_IRUSR|S_IWUSR|S_IXUSR);

                if (handle->he_type == HM_TYPE_VOLUME) {
                    hmd_type = HM_TYPE_VOLUME;
                    vol = handle->he_avolume;
                } else {
                    hmd_type = HM_TYPE_VOLDIR;
                    vol = volume_get_by_index(handle->he_entnum);
                    handle->he_entnum++;
                }
                if (vol == NULL)
                    path = ".";
                else
                    path = vol->av_path;

                fs_size = get_fs_size(path, &fs_used, &fs_blksize);
                size_lo = (uint32_t) fs_used;
                hm_dirent->hmd_blksize = SWAP32(fs_blksize);
                hm_dirent->hmd_blks    = SWAP32(size_lo);
                if (vol == NULL) {
                    hm_dirent->hmd_ino   = 0;
                    hm_dirent->hmd_nlink = 1;
                } else {
                    hm_dirent->hmd_ino   = vol->av_flags;
                    hm_dirent->hmd_nlink = vol->av_bootpri;
                }
                size_hi = (uint32_t) (fs_size >> 32);
                size_lo = (uint32_t) fs_size;

                hmd_type = HM_TYPE_VOLUME;
                amiga_perms = amiga_perms_from_host(0444);  // read-only
            } else {
                /* he_avolume is NULL for the Volume Directory */
                amiga_vol_t *avol = handle->he_avolume;
                if (avol == NULL) {
                    fsprintf("BUG: handle=%x he_avolume is NULL\n",
                             handle->he_handle);
                    break;
                }
                host_path = make_host_path(avol, handle->he_name);
                if ((handle->he_mode & HM_MODE_DIR) == 0) {
                    char *temp_path = host_path;
                    host_path = merge_host_paths(temp_path, d_name);
                    free(temp_path);
                }

                if (lstat(host_path, &st) == 0) {
                    uint32_t time_a;
                    uint32_t time_c;
                    uint32_t time_m;
                    char *host_path_uaem;

                    if (((he_mode & HM_MODE_NOFOLLOW) == 0) &&
                        (stat(host_path, &st) != 0)) {
                        /* Just use the result of previous lstat */
                        fsprintf("stat %s failed\n", host_path);
                    }

                    /* UAE support: check for .uaem file */
                    host_path_uaem = malloc(strlen(host_path) + 6);
                    if (host_path_uaem != NULL) {
                        FILE *fp;
                        strcpy(host_path_uaem, host_path);
                        strcat(host_path_uaem, ".uaem");
                        if ((fp = fopen(host_path_uaem, "r")) != NULL) {
                            char f_perms[16];
                            char f_date[12];
                            char f_time[12];
                            if (fscanf(fp, "%s %s %s",
                                       f_perms, f_date, f_time) == 3) {
                                fsprintf("%s UAEM perms=%s\n",
                                         host_path_uaem, f_perms);
                                amiga_perms = amiga_perms_from_str(f_perms);
                                if (amiga_perms != 0xffffffff) {
                                    st.st_mode = (st.st_mode & S_IFMT) |
                                             host_perms_from_amiga(amiga_perms);
                                }
                            }
                            fclose(fp);
                        }
                        free(host_path_uaem);
                    }

                    time_a = get_localtime(st.st_atime);
                    time_c = get_localtime(st.st_ctime);
                    time_m = get_localtime(st.st_mtime);
                    hm_dirent->hmd_atime = SWAP32(time_a);
                    hm_dirent->hmd_ctime = SWAP32(time_c);
                    hm_dirent->hmd_mtime = SWAP32(time_m);
#ifdef __MINGW32__
                    uint blksize = 1 << 20;
                    hm_dirent->hmd_blksize = SWAP32(blksize);
                    hm_dirent->hmd_blks = SWAP32(st.st_size / blksize);
#else
                    hm_dirent->hmd_blksize = SWAP32(st.st_blksize);
                    hm_dirent->hmd_blks = SWAP32(st.st_blocks);
#endif
                    hm_dirent->hmd_ouid = SWAP32(st.st_uid);
                    hm_dirent->hmd_ogid = SWAP32(st.st_gid);
                    hm_dirent->hmd_mode = SWAP32(st.st_mode);

                    size_hi = ((uint64_t) st.st_size) >> 32;
                    size_lo = (uint32_t) st.st_size;
                    hmd_type = st_mode_to_hm_type(st.st_mode);
                    amiga_perms = amiga_perms_from_host(st.st_mode);
                } else {
                    fsprintf("lstat %s failed\n", host_path);
                    size_hi = 0;
                    size_lo = 0;
                    amiga_perms = FIBF_OTR_READ | FIBF_GRP_READ;
                }
            }

            hm_dirent->hmd_aperms  = SWAP32(amiga_perms);
            hm_dirent->hmd_type    = SWAP16(hmd_type);
            hm_dirent->hmd_ino     = SWAP32(dp->d_ino);
            hm_dirent->hmd_size_hi = SWAP32(size_hi);
            hm_dirent->hmd_size_lo = SWAP32(size_lo);
            hm_dirent->hmd_rsvd[0] = 0;
            hm_dirent->hmd_rsvd[1] = 0;

            nptr = (char *) (hm_dirent + 1);
            nlen = strlen(d_name) + 1;  // Include NIL
            memcpy(nptr, d_name, nlen);
            if (hmd_type == HM_TYPE_LINK) {
                char  lbuf[PATH_MAX];
                char *path;
                int   llen;
                llen = readlink(host_path, lbuf, sizeof (lbuf) - 1);
                if (llen == -1) {
                    fsprintf("readlink %s failed\n", host_path);
                    llen = 0;
                }
                lbuf[llen] = '\0';
                path = host_to_amiga_path(host_path, handle->he_name, lbuf);

                /* Fill comment with link information */
                llen = strlen(path) + 1;
                memcpy(nptr + nlen, path, llen);
                free(path);

                nlen += llen;
            } else {
                nptr[nlen++] = '\0';            // Comment NIL
            }
            if (nlen & 1)
                nptr[nlen++] = '\0';            // Round up
#ifdef DEBUG_READ
            fsprintf("dirent %u %s\n", nlen, nptr);
#endif
            hm_dirent->hmd_elen = SWAP16(nlen);
            pos += sizeof (*hm_dirent) + nlen;
            if (host_path != NULL)
                free(host_path);
        } else {
            /* Regular file */
            if (hm_flag & HM_FLAG_SEEK0) {
                hm_flag &= ~HM_FLAG_SEEK0;
                (void) lseek64(handle->he_fd, 0, SEEK_SET);
            }

            readcnt = read(handle->he_fd, ndata, len);
#ifdef DEBUG_READ
            fsprintf("read %d bytes from fd=%d %s\n",
                     rc, handle->he_fd, handle->he_name);
#endif
            if (readcnt <= 0) {
                if (readcnt == 0) {
                    rc = KM_STATUS_EOF;
                } else if (readcnt < 0) {
                    rc = errno_to_km_status();
                }
                break;
            }
            pos += readcnt;
            rc = 0;
        }
    }
    if ((rc != KM_STATUS_OK) && (rc != KM_STATUS_EOF))
        fsprintf("Returning odd rc=%d\n", rc);

    hmr->hm_hdr.km_op = hm->hm_hdr.km_op;
    hmr->hm_hdr.km_status = rc;
    hmr->hm_hdr.km_tag = hm->hm_hdr.km_tag;
    hmr->hm_handle = hm->hm_handle;
    hmr->hm_length = SWAP32(pos);
    hmr->hm_flag = 0;
    hmr->hm_unused = 0;
#ifdef DEBUG_READ_DATA
    dump_memory(hmr, sizeof (*hmr) + pos, VALUE_UNASSIGNED);
#endif
    rc = send_msg(hmr, sizeof (*hmr) + pos, status);
    free(hmr);
    return (rc);
}

uint
sm_fwrite(hm_freadwrite_t *hm, uint rxlen, uint *status)
{
    uint             rc        = 0;
    uint             hm_length = SWAP32(hm->hm_length);
    uint             hm_flag   = SWAP16(hm->hm_flag);
    handle_ent_t    *handle    = handle_get(hm->hm_handle);
    uint8_t         *ndata     = (uint8_t *)(hm + 1);
    ssize_t          writecnt  = 0;
    uint16_t         main_tag  = hm->hm_hdr.km_tag;

#ifdef FILE_DEBUG
    off64_t          fpos = 0;
    if ((hm_flag & HM_FLAG_SEEK0) == 0) {
        if (handle != NULL) {
            fpos = lseek64(handle->he_fd, 0, SEEK_CUR);
            if (fpos < 0)
                fpos = 0;
        }
    }
    fsprintf("fwrite(%x p=%jx l=%x tag=%x)\n",
             hm->hm_handle, (intmax_t)fpos, hm_length, SWAP16(main_tag));
#endif
    hm->hm_hdr.km_op |= KM_OP_REPLY;

    if (handle == NULL) {
        fsprintf("handle get %x failed\n", hm->hm_handle);
reply_write_fail:
        if (hm->hm_hdr.km_status == KM_STATUS_OK)
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
        return (send_msg(hm, sizeof (*hm), status));
    }
    if ((handle->he_mode & HM_MODE_WRITE) == 0) {
        fsprintf("%s not opened for write mode: %x\n",
                 handle->he_name, handle->he_mode);
        hm->hm_hdr.km_status = KM_STATUS_INVALID;
        goto reply_write_fail;
    }
    if ((handle->he_type == HM_TYPE_DIR) ||
        (handle->he_type == HM_TYPE_VOLDIR) ||
        (handle->he_mode & HM_MODE_DIR)) {
        fsprintf("Can't write to directory\n");
        /*
         * XXX: Writing a file's dirent could be used as a method to
         *      change atime, ctime, mtime, aperms, ouid, ogid, and
         *      maybe even allow rename.
         *      Maybe require a field which specifies which other
         *      fields are to be updated.
         */
        hm->hm_hdr.km_status = KM_STATUS_INVALID;
        goto reply_write_fail;
    }

    if (rxlen >= sizeof (hm_freadwrite_t))
        rxlen -= sizeof (hm_freadwrite_t);
    else
        rxlen = 0;

    if (rxlen < hm_length) {
        /* More data pending */
        uint8_t *rdata = malloc(hm_length + sizeof (km_msg_hdr_t));
        uint8_t  rxdata[4096];
        uint     rdatapos = rxlen;
        uint     timeout = 0;

        if (rdata == NULL) {
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
            return (send_msg(hm, sizeof (*hm), status));
        }
        memcpy(rdata, ndata, rxlen);
        ndata = (uint8_t *) ((km_msg_hdr_t *) rxdata + 1);
        while (rdatapos < hm_length) {
            uint rxmax = hm_length - rdatapos + sizeof (km_msg_hdr_t);
            rc = recv_msg(main_tag, rxdata, sizeof (rxdata), status, &rxlen);
            if (rc != RC_SUCCESS)
                break;

            if (rxlen == 0) {
                if (++timeout < 1000) {
                    time_delay_msec(1);
                    continue;
                }
                printf("fwrite(%x) tag=%x data timeout at pos=%x\n",
                       hm->hm_handle, main_tag, rdatapos);
                rc = RC_FAILURE;
                break;
            }
            timeout = 0;
            if (rxlen > rxmax) {
                printf("fwrite(%x) tag=%x receive len %x > max %x\n",
                       hm->hm_handle, main_tag, rxlen, rxmax);
                rc = RC_FAILURE;
                break;
            }

            if (rxlen >= sizeof (km_msg_hdr_t))
                rxlen -= sizeof (km_msg_hdr_t);
            else
                rxlen = 0;
            memcpy(rdata + rdatapos, ndata, rxlen);
            rdatapos += rxlen;
        }
        if (rc == RC_SUCCESS) {
            if (hm_flag & HM_FLAG_SEEK0) {
                hm_flag &= ~HM_FLAG_SEEK0;
                (void) lseek64(handle->he_fd, 0, SEEK_SET);
            }
            writecnt = write(handle->he_fd, rdata, hm_length);
        }
        free(rdata);
    } else {
        writecnt = write(handle->he_fd, ndata, hm_length);
    }
    if (rc == KM_STATUS_OK) {
        if (writecnt < 0) {
            fsprintf("write rc=%d errno=%d\n", rc, errno);
            rc = errno_to_km_status();
        }
    }

    hm->hm_hdr.km_status = rc;
    return (send_msg(hm, sizeof (*hm), status));
}

uint
sm_fseek(hm_fseek_t *hm, uint *status)
{
    handle_ent_t *handle = handle_get(hm->hm_handle);

    fsprintf("fseek(%x, o=%"PRIx64" from=%d)\n", hm->hm_handle,
             ((uint64_t) hm->hm_off_hi << 32) | hm->hm_off_lo, hm->hm_seek);
    hm->hm_hdr.km_status = KM_STATUS_OK;
    if (handle == NULL) {
        fsprintf("handle get %x failed\n", hm->hm_handle);
        if (hm->hm_hdr.km_status == KM_STATUS_OK)
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
        return (send_msg(hm, sizeof (*hm), status));
    }
    if ((handle->he_type == HM_TYPE_VOLDIR) ||
        (handle->he_mode & HM_MODE_DIR)) {
        /* Can rewind volume dir or file stat */
        hm->hm_old_hi = 0;
        hm->hm_old_lo = SWAP32(handle->he_entnum);
        handle->he_entnum = 0;  // Can only rewind volume dir pointer
    } else if (handle->he_type == HM_TYPE_DIR) {
        /* Can only rewind dir */
        if (handle->he_dir != NULL)
            rewinddir(handle->he_dir);
        hm->hm_old_hi = 0;
        hm->hm_old_lo = SWAP32(handle->he_entnum);
        handle->he_entnum = 0;
    } else if (handle->he_type == HM_TYPE_FILE) {
        uint32_t hi = SWAP32(hm->hm_off_hi);
        uint32_t lo = SWAP32(hm->hm_off_lo);
        int seek_mode = hm->hm_seek;
        off64_t oldpos;
        off64_t offset = ((uint64_t) hi << 32) | lo;
        off64_t newpos;
        int whence;

        switch (seek_mode) {
            default:
                fsprintf("Unknown seek mode (%d)\n", seek_mode);
                hm->hm_hdr.km_status = KM_STATUS_INVALID;
                goto reply_seek;
            case SEEK_OFFSET_BEGINNING:
                whence = SEEK_SET;
                break;
            case SEEK_OFFSET_CURRENT:
                whence = SEEK_CUR;
                break;
            case SEEK_OFFSET_END:
                whence = SEEK_END;
                break;
        }

        oldpos = lseek64(handle->he_fd, 0, SEEK_CUR);
        newpos = lseek64(handle->he_fd, offset, whence);
        if (newpos < 0) {
            /* Seek failed */
            fsprintf("Seek %x to %jd (%u) failed\n",
                     hm->hm_handle, (intmax_t)offset, whence);
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
        } else {
            hi = newpos >> 32;
            lo = (uint32_t) newpos;
            hm->hm_off_hi = SWAP32(hi);
            hm->hm_off_lo = SWAP32(lo);
            hi = oldpos >> 32;
            lo = (uint32_t) oldpos;
            hm->hm_old_hi = SWAP32(hi);
            hm->hm_old_lo = SWAP32(lo);
        }
    } else {
        fsprintf("Can't seek in file type %x\n", handle->he_type);
        hm->hm_hdr.km_status = KM_STATUS_INVALID;
    }
reply_seek:
    hm->hm_hdr.km_op |= KM_OP_REPLY;
    return (send_msg(hm, sizeof (*hm), status));
}

uint
sm_fcreate(hm_fopenhandle_t *hm, uint *status)
{
    handle_ent_t *phandle = handle_get(hm->hm_handle);
    char         *hm_name = (char *)(hm + 1);
    uint32_t      aperms  = SWAP32(hm->hm_aperms);
    uint32_t      umode   = host_perms_from_amiga(aperms);
    uint          hm_type = SWAP16(hm->hm_type);
    uint          dev     = SWAP16(hm->hm_mode);
    uint          ftype;
    char         *host_path = NULL;
    char         *name = NULL;

    fsprintf("fcreate(%s) type=%x perms=%x umode=%x in %x\n",
             hm_name, hm_type, hm->hm_mode, umode, hm->hm_handle);

    hm->hm_hdr.km_op |= KM_OP_REPLY;
    hm->hm_hdr.km_status = KM_STATUS_OK;

    if ((name = make_amiga_relpath(&phandle, hm_name)) == NULL) {
        fsprintf("fcreate(%s) relative path failed\n", hm_name);
reply_create_fail:
        if (name != NULL)
            free(name);
        hm->hm_handle = 0;
        if (hm->hm_hdr.km_status == KM_STATUS_OK)
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
        return (send_msg(hm, sizeof (*hm), status));
    }

    if (phandle == NULL) {
        /* Can't create the volume directory */
        fsprintf("Can't create the volume directory\n");
        hm->hm_hdr.km_status = KM_STATUS_INVALID;
        goto reply_create_fail;
    }
    host_path = make_host_path(phandle->he_avolume, name);
    free(name);
    name = NULL;

    fsprintf("host path=%s\n", host_path);
    switch (hm_type) {
        case HM_TYPE_FILE:   // Regular file
            ftype = S_IFREG;
create_node:
#ifdef __MINGW32__
            (void) ftype;
            (void) dev;
            printf("mknod() not supported in Windows\n");
#else
            if (mknod(host_path, ftype | umode, dev)) {
                hm->hm_hdr.km_status = errno_to_km_status();
                goto reply_create_fail;
            }
#endif
            break;
        case HM_TYPE_DIR:    // Directory
            if (mkdir(host_path, umode)) {
                hm->hm_hdr.km_status = errno_to_km_status();
                goto reply_create_fail;
            }
            break;
#ifdef ALLOW_CREATE_LINK
        case HM_TYPE_LINK: {   // Symbolic (soft) link
            uint  hm_name_len = strlen(hm_name) + 1;
            char *lname = hm_name + hm_name_len;
            char *relname;
            relname = amiga_link_to_host_path(phandle, lname, host_path);

            if (symlink(relname, host_path)) {
                fsprintf("symlink: %s -> %s failed: %d\n",
                         host_path, lname, errno);
                hm->hm_hdr.km_status = errno_to_km_status();
                goto reply_create_fail;
            }
            if (relname != NULL)
                free(relname);
            break;
        }
        case HM_TYPE_HLINK: {  // Hard link
            uint  hm_name_len = strlen(hm_name) + 1;
            char *lname = hm_name + hm_name_len;
            char *relname;

            relname = amiga_link_to_host_path(phandle, lname, host_path);
            if (link(lname, host_path)) {
                fsprintf("hard link %s -> %s failed: %d\n",
                         host_path, lname, errno);
                hm->hm_hdr.km_status = errno_to_km_status();
                goto reply_create_fail;
            }
            if (relname != NULL)
                free(relname);
            break;
        }
#endif /* ALLOW_CREATE_LINK */
        case HM_TYPE_BDEV:   // Block device
            ftype = S_IFBLK;
            goto create_node;
        case HM_TYPE_CDEV:   // Block device
            ftype = S_IFCHR;
            goto create_node;
        case HM_TYPE_FIFO:   // FIFO
            ftype = S_IFIFO;
            goto create_node;
#ifndef __MINGW32__
        case HM_TYPE_SOCKET: // Socket
            ftype = S_IFSOCK;
            goto create_node;
#endif
        case HM_TYPE_WHTOUT: // Whiteout entry
        case HM_TYPE_VOLUME: // Disk volume
        case HM_TYPE_VOLDIR: // Volume directory
        default:
            hm->hm_hdr.km_status = KM_STATUS_INVALID;
            break;
    }

    return (send_msg(hm, sizeof (*hm), status));
}

uint
sm_fdelete(hm_fhandle_t *hm, uint *status)
{
    handle_ent_t *phandle = handle_get(hm->hm_handle);
    char         *hm_name = (char *)(hm + 1);
    char         *host_path = NULL;
    char         *name = NULL;
    struct stat   st;

    fsprintf("fdelete(%s) in %x\n", hm_name, hm->hm_handle);

    hm->hm_hdr.km_op |= KM_OP_REPLY;
    hm->hm_hdr.km_status = KM_STATUS_OK;

    if ((name = make_amiga_relpath(&phandle, hm_name)) == NULL) {
        fsprintf("fdelete(%s) relative path failed\n", hm_name);
reply_delete_fail:
        if (host_path != NULL)
            free(host_path);
        if (name != NULL)
            free(name);
        hm->hm_handle = 0;
        if (hm->hm_hdr.km_status == KM_STATUS_OK)
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
        return (send_msg(hm, sizeof (*hm), status));
    }

    if (phandle == NULL) {
        /* Can't delete the volume directory */
        fsprintf("Can't delete the volume directory\n");
        hm->hm_hdr.km_status = KM_STATUS_INVALID;
        goto reply_delete_fail;
    }
    host_path = make_host_path(phandle->he_avolume, name);

    if (lstat(host_path, &st) != 0) {
        fsprintf("fdelete(%s) stat fail errno=%d\n", host_path, errno);
        goto reply_delete_fail;
    }
    switch (st.st_mode & S_IFMT) {
        case S_IFDIR:
            /* Use rmdir() */
            if (volume_get_by_path(host_path, 0) != NULL) {
                fsprintf("fdelete(%s) can't remove a volume\n", host_path);
                hm->hm_hdr.km_status = KM_STATUS_PERM;
                goto reply_delete_fail;
            }
            if (rmdir(host_path)) {
                /* Failed */
                fsprintf("rmdir(%s) failed: %d\n", host_path, errno);
                hm->hm_hdr.km_status = errno_to_km_status();
                goto reply_delete_fail;
            }
            break;
        default:
        case S_IFBLK:   // Block device
        case S_IFCHR:   // Character device
        case S_IFIFO:   // FIFO (Pipe)
        case S_IFREG:   // Regular file
#ifndef __MINGW32__
        case S_IFLNK:   // Symlink
        case S_IFSOCK:  // Socket
#endif
            /* Use unlink() */
            if (unlink(host_path)) {
                fsprintf("unlink(%s) failed: %d\n", host_path, errno);
                hm->hm_hdr.km_status = errno_to_km_status();
                goto reply_delete_fail;
            }
            break;
    }
    free(name);
    free(host_path);
    return (send_msg(hm, sizeof (*hm), status));
}

uint
sm_frename(hm_frename_t *hm, uint *status)
{
    handle_ent_t *phandle_old  = handle_get(hm->hm_shandle);
    handle_ent_t *phandle_new  = handle_get(hm->hm_dhandle);
    char         *name_old     = (char *)(hm + 1);
    uint          len_old_name = strlen(name_old) + 1;
    char         *name_new     = name_old + len_old_name;
    char         *path_old     = NULL;
    char         *path_new     = NULL;
    char         *apath_old    = NULL;
    char         *apath_new    = NULL;

    fsprintf("frename(%s to %s) in %x to %x\n",
             name_old, name_new, hm->hm_shandle, hm->hm_dhandle);
    hm->hm_hdr.km_op |= KM_OP_REPLY;
    hm->hm_hdr.km_status = KM_STATUS_OK;

    if ((apath_old = make_amiga_relpath(&phandle_old, name_old)) == NULL) {
        fsprintf("frename(%s) relative path failed\n", name_old);
reply_rename_fail:
        if (apath_old != NULL)
            free(apath_old);
        if (apath_new != NULL)
            free(apath_new);
        if (path_old != NULL)
            free(path_old);
        if (path_new != NULL)
            free(path_new);
        hm->hm_shandle = 0;
        hm->hm_dhandle = 0;
        if (hm->hm_hdr.km_status == KM_STATUS_OK)
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
        return (send_msg(hm, sizeof (*hm), status));
    }

    if (phandle_old == NULL) {
        /* Can't rename the volume directory */
        fsprintf("frename(%s) Can't rename the volume directory\n", name_old);
        hm->hm_hdr.km_status = KM_STATUS_INVALID;
        goto reply_rename_fail;
    }
    path_old = make_host_path(phandle_old->he_avolume, apath_old);

    if ((apath_new = make_amiga_relpath(&phandle_new, name_new)) == NULL) {
        fsprintf("frename(%s) relative path failed\n", name_new);
        goto reply_rename_fail;
    }

    if (phandle_new == NULL) {
        /* Can't rename the volume directory */
        fsprintf("frename(%s) Can't rename to the volume directory\n",
                 name_new);
        hm->hm_hdr.km_status = KM_STATUS_INVALID;
        goto reply_rename_fail;
    }
    path_new = make_host_path(phandle_new->he_avolume, apath_new);

    if (volume_get_by_path(path_old, 0) != NULL) {
        fsprintf("frename(%s) can't rename a volume\n", path_old);
        hm->hm_hdr.km_status = KM_STATUS_PERM;
        goto reply_rename_fail;
    }
    if (volume_get_by_path(path_new, 0) != NULL) {
        fsprintf("frename(%s) can't rename to a volume\n", path_new);
        hm->hm_hdr.km_status = KM_STATUS_PERM;
        goto reply_rename_fail;
    }
    if (rename(path_old, path_new)) {
        fsprintf("rename %s to %s failed\n", path_old, path_new);
        hm->hm_hdr.km_status = errno_to_km_status();
        goto reply_rename_fail;
    }
    free(path_old);
    free(path_new);
    free(apath_old);
    free(apath_new);

    hm->hm_hdr.km_status = KM_STATUS_OK;
    return (send_msg(hm, sizeof (*hm), status));
}

uint
sm_fpath(hm_fhandle_t *hm, uint *status)
{
    /* Resolve handle to Amiga-specific path */
    hm_fhandle_t *hmr;
    handle_ent_t *handle = handle_get(hm->hm_handle);
    char         *pathbuf;
    char          pathlen;
    uint          rc;

    fsprintf("fpath(%x)\n", hm->hm_handle);
    hm->hm_hdr.km_status = KM_STATUS_OK;
    hm->hm_hdr.km_op |= KM_OP_REPLY;
    if (handle == NULL) {
        /* Volume directory */
        pathbuf = strdup("::");
    } else {
        amiga_vol_t *vol = NULL;
        handle_ent_t *volhandle = handle->he_volume;

        if (volhandle != NULL)
            vol = volume_get_by_handle(volhandle);

        if (vol != NULL) {
            pathbuf = merge_amiga_paths(vol->av_volume, handle->he_name);
        } else {
            pathbuf = strdup(handle->he_name);
        }
    }

    fsprintf("pathbuf=%s\n", pathbuf);
    pathlen = strlen(pathbuf) + 1;
    hmr = malloc(sizeof (*hmr) + pathlen + 1);
    memcpy(hmr, hm, sizeof (*hmr));
    strcpy((char *) (hmr + 1), pathbuf);
    ((char *) hmr)[sizeof (*hmr) + pathlen] = '\0';  // unaligned data end init

    rc = send_msg(hmr, sizeof (*hmr) + pathlen, status);
    free(pathbuf);
    free(hmr);
    return (rc);
}

uint
sm_fsetdate(hm_fsetdate_t *hm, uint *status)
{
    /* Resolve handle to Amiga-specific path */
    handle_ent_t *phandle = handle_get(hm->hm_handle);
    char         *name    = (char *)(hm + 1);
    char         *path    = NULL;
    char         *apath;
    uint8_t       which   = hm->hm_which;
    uint32_t      sec     = SWAP32(hm->hm_time);
    uint32_t      nsec    = SWAP32(hm->hm_time_ns);
    uint32_t      utcsec  = get_utctime(sec);
    struct stat   statbuf;

    fsprintf("fsetdate(%s %u %u.%u)\n", name, which, utcsec, nsec);
    hm->hm_hdr.km_op |= KM_OP_REPLY;

    if ((apath = make_amiga_relpath(&phandle, name)) == NULL) {
        fsprintf("fsetdate(%s) relative path failed\n", name);
reply_setdate_fail:
        if (apath != NULL)
            free(apath);
        if (path != NULL)
            free(path);
        hm->hm_handle = 0;
        if (hm->hm_hdr.km_status == KM_STATUS_OK)
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
        return (send_msg(hm, sizeof (*hm), status));
    }

    if (phandle == NULL) {
        /* Can't set perms on the volume directory */
        fsprintf("fsetdate(%s) can't set dateer of the volume directory\n",
                 name);
        hm->hm_hdr.km_status = KM_STATUS_INVALID;
        goto reply_setdate_fail;
    }
    path = make_host_path(phandle->he_avolume, apath);

    if (volume_get_by_path(path, 0) != NULL) {
        fsprintf("fsetdate(%s) can't set owner of a volume\n", path);
        hm->hm_hdr.km_status = KM_STATUS_PERM;
        goto reply_setdate_fail;
    }

    if (lstat(path, &statbuf)) {
        fsprintf("lstat fail: %d\n", errno);
        hm->hm_hdr.km_status = errno_to_km_status();
        goto reply_setdate_fail;
    }

#ifdef __MINGW32__
    struct _utimbuf times;
    times.actime  = statbuf.st_atime;  // last access time
    times.modtime = statbuf.st_mtime;  // last modification time

    switch (which) {
        case 0: // Set the modify date/time
            times.modtime = (((uint64_t) utcsec) << 32) | nsec;
            break;
        case 1: // Get the modify date/time
            hm->hm_time    = SWAP32((uint32_t) (statbuf.st_ctime >> 32));
            hm->hm_time_ns = SWAP32((uint32_t) statbuf.st_ctime);
            goto reply_setdate_good;
        case 2: // Set the change date/time
            /* Not sure how to set this one */
            hm->hm_hdr.km_status = KM_STATUS_INVALID;
            goto reply_setdate_fail;
        case 3: // Get the change date/time
            hm->hm_time    = SWAP32((uint32_t) (statbuf.st_ctime >> 32));
            hm->hm_time_ns = SWAP32((uint32_t) statbuf.st_ctime);
            goto reply_setdate_good;
        case 4: // Set the access date/time
            times.actime = (((uint64_t) utcsec) << 32) | nsec;
            break;
        case 5: // Get the access date/time
            hm->hm_time    = SWAP32((uint32_t) (statbuf.st_atime >> 32));
            hm->hm_time_ns = SWAP32((uint32_t) statbuf.st_atime);
            goto reply_setdate_good;
        default:
            hm->hm_hdr.km_status = KM_STATUS_INVALID;
            goto reply_setdate_fail;
    }

    if (_utime(path, &times)) {
        fsprintf("utimesat fail: %d\n", errno);
        hm->hm_hdr.km_status = errno_to_km_status();
        goto reply_setdate_fail;
    }
#elif defined(OSX)
    /* MacOS */
    struct timeval times[2];
    timespec_to_timeval(&times[0], &statbuf.st_atimespec);  // last access time
    timespec_to_timeval(&times[1], &statbuf.st_ctimespec);  // last modify time

    switch (which) {
        case 0: // Set the modify date/time
            times[1].tv_sec  = utcsec;
            times[1].tv_usec = nsec / 1000;
            break;
        case 1: // Get the modify date/time
            hm->hm_time    = SWAP32(statbuf.st_ctimespec.tv_sec);
            hm->hm_time_ns = SWAP32(statbuf.st_ctimespec.tv_nsec);
            goto reply_setdate_good;
        case 2: // Set the change date/time
            /* Not sure how to set this one */
            hm->hm_hdr.km_status = KM_STATUS_INVALID;
            goto reply_setdate_fail;
        case 3: // Get the change date/time
            hm->hm_time    = SWAP32(statbuf.st_ctimespec.tv_sec);
            hm->hm_time_ns = SWAP32(statbuf.st_ctimespec.tv_nsec);
            goto reply_setdate_good;
        case 4: // Set the access date/time
            times[0].tv_sec  = utcsec;
            times[0].tv_usec = nsec / 1000;
            break;
        case 5: // Get the access date/time
            hm->hm_time    = SWAP32(statbuf.st_atimespec.tv_sec);
            hm->hm_time_ns = SWAP32(statbuf.st_atimespec.tv_nsec);
            goto reply_setdate_good;
        default:
            hm->hm_hdr.km_status = KM_STATUS_INVALID;
            goto reply_setdate_fail;
    }
    if (utimes(path, times)) {
        fsprintf("utimes fail: %d\n", errno);
        hm->hm_hdr.km_status = errno_to_km_status();
        goto reply_setdate_fail;
    }
#else
    /* Linux */
    struct timespec times[2];
    times[0] = statbuf.st_atim;  // last access time
    times[1] = statbuf.st_ctim;  // last modification time

    switch (which) {
        case 0: // Set the modify date/time
            times[1].tv_sec  = utcsec;
            times[1].tv_nsec = nsec;
            break;
        case 1: // Get the modify date/time
            hm->hm_time    = SWAP32(statbuf.st_ctim.tv_sec);
            hm->hm_time_ns = SWAP32(statbuf.st_ctim.tv_nsec);
            goto reply_setdate_good;
        case 2: // Set the change date/time
            /* Not sure how to set this one */
            hm->hm_hdr.km_status = KM_STATUS_INVALID;
            goto reply_setdate_fail;
        case 3: // Get the change date/time
            hm->hm_time    = SWAP32(statbuf.st_ctim.tv_sec);
            hm->hm_time_ns = SWAP32(statbuf.st_ctim.tv_nsec);
            goto reply_setdate_good;
        case 4: // Set the access date/time
            times[0].tv_sec  = utcsec;
            times[0].tv_nsec = nsec;
            break;
        case 5: // Get the access date/time
            hm->hm_time    = SWAP32(statbuf.st_atim.tv_sec);
            hm->hm_time_ns = SWAP32(statbuf.st_atim.tv_nsec);
            goto reply_setdate_good;
        default:
            hm->hm_hdr.km_status = KM_STATUS_INVALID;
            goto reply_setdate_fail;
    }
    if (utimensat(AT_FDCWD, path, times, AT_SYMLINK_NOFOLLOW)) {
        fsprintf("utimesat fail: %d\n", errno);
        hm->hm_hdr.km_status = errno_to_km_status();
        goto reply_setdate_fail;
    }
#endif

reply_setdate_good:
    free(apath);
    free(path);

    hm->hm_hdr.km_status = KM_STATUS_OK;
    return (send_msg(hm, sizeof (*hm), status));
}

uint
sm_fsetown(hm_fsetown_t *hm, uint *status)
{
    /* Resolve handle to Amiga-specific path */
    handle_ent_t *phandle = handle_get(hm->hm_handle);
    char         *name    = (char *)(hm + 1);
    char         *path    = NULL;
    char         *apath;
    uint32_t      oid = SWAP32(hm->hm_oid);
    uint32_t      gid = SWAP32(hm->hm_gid);

    fsprintf("fsetown(%s %d %d)\n", name, oid, gid);
    hm->hm_hdr.km_op |= KM_OP_REPLY;

    if ((apath = make_amiga_relpath(&phandle, name)) == NULL) {
        fsprintf("fsetown(%s) relative path failed\n", name);
reply_setown_fail:
        if (apath != NULL)
            free(apath);
        if (path != NULL)
            free(path);
        hm->hm_handle = 0;
        if (hm->hm_hdr.km_status == KM_STATUS_OK)
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
        return (send_msg(hm, sizeof (*hm), status));
    }

    if (phandle == NULL) {
        /* Can't set perms on the volume directory */
        fsprintf("fsetown(%s) can't set owner of the volume directory\n",
                 name);
        hm->hm_hdr.km_status = KM_STATUS_INVALID;
        goto reply_setown_fail;
    }
    path = make_host_path(phandle->he_avolume, apath);

    if (volume_get_by_path(path, 0) != NULL) {
        fsprintf("fsetown(%s) can't set owner of a volume\n", path);
        hm->hm_hdr.km_status = KM_STATUS_PERM;
        goto reply_setown_fail;
    }

#ifdef __MINGW32__
    hm->hm_hdr.km_status = KM_STATUS_PERM;
    goto reply_setown_fail;
#else
    if (chown(path, oid, gid)) {
        fsprintf("chown fail: %d\n", errno);
        hm->hm_hdr.km_status = errno_to_km_status();
        goto reply_setown_fail;
    }
#endif

    free(apath);
    free(path);

    hm->hm_hdr.km_status = KM_STATUS_OK;
    return (send_msg(hm, sizeof (*hm), status));
}

uint
sm_fsetprotect(hm_fopenhandle_t *hm, uint *status)
{
    /* Resolve handle to Amiga-specific path */
    handle_ent_t *phandle = handle_get(hm->hm_handle);
    char         *name    = (char *)(hm + 1);
    char         *path    = NULL;
    char         *apath;
    uint32_t      aperms = SWAP32(hm->hm_aperms);
    uint          uperms;

    fsprintf("fsetprotect(%s %x)\n", name, aperms);
    hm->hm_hdr.km_op |= KM_OP_REPLY;

    if ((apath = make_amiga_relpath(&phandle, name)) == NULL) {
        fsprintf("fsetprotect(%s) relative path failed\n", name);
reply_setprotect_fail:
        if (path != NULL)
            free(path);
        if (apath != NULL)
            free(apath);
        hm->hm_handle = 0;
        if (hm->hm_hdr.km_status == KM_STATUS_OK)
            hm->hm_hdr.km_status = KM_STATUS_FAIL;
        return (send_msg(hm, sizeof (*hm), status));
    }

    if (phandle == NULL) {
        /* Can't set perms on the volume directory */
        fsprintf("fsetprotect(%s) can't set perms on the volume directory\n",
                 name);
        hm->hm_hdr.km_status = KM_STATUS_INVALID;
        goto reply_setprotect_fail;
    }
    path = make_host_path(phandle->he_avolume, apath);

    if (volume_get_by_path(path, 0) != NULL) {
        fsprintf("fsetprotect(%s) can't set perms on a volume\n", path);
        hm->hm_hdr.km_status = KM_STATUS_PERM;
        goto reply_setprotect_fail;
    }
    uperms = host_perms_from_amiga(aperms);
    fsprintf("uperms=%x %o\n", uperms, uperms);

    if (chmod(path, uperms)) {
        fsprintf("chmod fail\n");
        hm->hm_hdr.km_status = errno_to_km_status();
        goto reply_setprotect_fail;
    }

    free(apath);
    free(path);

    hm->hm_hdr.km_status = KM_STATUS_OK;
    return (send_msg(hm, sizeof (*hm), status));
}
