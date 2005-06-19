/*
 * jigdoofus.c
 *
 * jigdo file system interface using FUSE
 * 
 * jigdoofus is a simple, read-only filesystem. It will transform any
 * jigdo template files into ISO images, but will pass anything else
 * through raw, similar to the fuse example filesystem fusexmp.
 *
 * Copyright (c) 2005 Steve McIntyre <steve@einval.com>
 *
 * GPL v2 - see COPYING
 */

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/statfs.h>
#include <stdarg.h>
#include "jigdb.h"
#include "jte.h"

/* Some definitions needed */
int G_verbose = 0;
char *G_base_dir = NULL;
struct fuse *M_fuse_state = NULL;

static int jdfs_is_template(const char *path)
{
    int length = strlen(path);
    if (!strcmp(&path[length - 9], ".template"))
        return 1;
    /* else */
    return 0;
}

static int jdfs_getattr(const char *path, struct stat *stbuf)
{
    int res;

    res = lstat(path, stbuf);
    if(res == -1)
        return -errno;

    return 0;
}

static int jdfs_readlink(const char *path, char *buf, size_t size)
{
    int res;

    res = readlink(path, buf, size - 1);
    if(res == -1)
        return -errno;

    buf[res] = '\0';
    return 0;
}

static int jdfs_getdir(const char *path, fuse_dirh_t h, fuse_dirfil_t filler)
{
    DIR *dp;
    struct dirent *de;
    int res = 0;

    dp = opendir(path);
    if(dp == NULL)
        return -errno;

    while((de = readdir(dp)) != NULL)
    {
        if (jdfs_is_template(de->d_name))
        {
            char buf[PATH_MAX];
            int length = strlen(de->d_name);
            
            strcpy(buf, de->d_name);
            strcpy(&buf[length - 9], ".iso");
            res = filler(h, buf, de->d_type);
        }
        else
            res = filler(h, de->d_name, de->d_type);
        if(res != 0)
            break;
    }

    closedir(dp);
    return res;
}

static int jdfs_mknod(const char *path, mode_t mode, dev_t rdev)
{
    return -EROFS;
}

static int jdfs_mkdir(const char *path, mode_t mode)
{
    return -EROFS;
}

static int jdfs_unlink(const char *path)
{
    return -EROFS;
}

static int jdfs_rmdir(const char *path)
{
    return -EROFS;
}

static int jdfs_symlink(const char *from, const char *to)
{
    return -EROFS;
}

static int jdfs_rename(const char *from, const char *to)
{
    return -EROFS;
}

static int jdfs_link(const char *from, const char *to)
{
    return -EROFS;
}

static int jdfs_chmod(const char *path, mode_t mode)
{
    return -EROFS;
}

static int jdfs_chown(const char *path, uid_t uid, gid_t gid)
{
    return -EROFS;
}

static int jdfs_truncate(const char *path, off_t size)
{
    return -EROFS;
}

static int jdfs_utime(const char *path, struct utimbuf *buf)
{
    return -EROFS;
}

static int jdfs_open(const char *path, int flags)
{
    int res;

    res = open(path, flags);
    if(res == -1)
        return -errno;

    close(res);
    return 0;
}

static int jdfs_read(const char *path, char *buf, size_t size, off_t offset)
{
    int fd;
    int res;
    off_t seek_offset = 0;

    fd = open(path, O_RDONLY);
    if(fd == -1)
        return -errno;

    seek_offset = lseek(fd, offset, SEEK_SET);
    if (seek_offset != offset)
        return EIO;

    res = read(fd, buf, size);
    if(res == -1)
        res = -errno;

    close(fd);
    return res;
}

static int jdfs_write(const char *path, const char *buf, size_t size, off_t offset)
{
    return -EROFS;
}

static int jdfs_statfs(const char *path, struct statfs *stbuf)
{
    int res;

    res = statfs(path, stbuf);
    if(res == -1)
        return -errno;

    return 0;
}

static int jdfs_release(const char *path, int flags)
{
    /* Just a stub.  This method is optional and can safely be left
       unimplemented */

    (void) path;
    (void) flags;
    return 0;
}

static int jdfs_fsync(const char *path, int isdatasync)
{
    /* Just a stub.  This method is optional and can safely be left
       unimplemented */

    (void) path;
    (void) isdatasync;
    return 0;
}

static struct fuse_operations jdfs_oper = {
    .getattr    = jdfs_getattr,
    .readlink   = jdfs_readlink,
    .getdir     = jdfs_getdir,
    .mknod      = jdfs_mknod,
    .mkdir      = jdfs_mkdir,
    .symlink    = jdfs_symlink,
    .unlink     = jdfs_unlink,
    .rmdir      = jdfs_rmdir,
    .rename     = jdfs_rename,
    .link       = jdfs_link,
    .chmod      = jdfs_chmod,
    .chown      = jdfs_chown,
    .truncate   = jdfs_truncate,
    .utime      = jdfs_utime,
    .open       = jdfs_open,
    .read       = jdfs_read,
    .write      = jdfs_write,
    .statfs     = jdfs_statfs,
    .release    = jdfs_release,
    .fsync      = jdfs_fsync,
};

int jd_log(int level, char *fmt, ...)
{
    int error = 0;    
    va_list ap;

    if (level <= G_verbose)
    {
        va_start(ap, fmt);
        error = vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    return error;
}

void display_progress(int verbose_level, INT64 image_size, INT64 current_offset, char *text)
{
    if ((verbose_level <= G_verbose) && (image_size > 0))
        jd_log(1, "\r %5.2f%%  %-60.60s",
               100.0 * current_offset / image_size, text);
}

extern void file_missing(char *missing, char *filename)
{
    jd_log(1, "Missing file %s\n", filename);
}

int main(int argc, char *argv[])
{
    const char *opts = "default_permissions,allow_other,allow_root,fsname=jigdoofus";
    int fuse_fd = -1;
    struct stat sb;
    int error = 0;
    char *mountpoint;
    
    if (argc != 3)
    {
        fprintf(stderr, "jigdoofus: not enough args!\n");
        fprintf(stderr, "Needs base directory and mount point\n");
        return 1;
    }
    
    G_base_dir = argv[1];
    mountpoint = argv[2];
    
    error = lstat(G_base_dir, &sb);
    if (error)
    {
        fprintf(stderr, "jigdoofus: can't open base dir %s (error %d)! Abort.\n", G_base_dir, error);
        return errno;
    }
    
    fuse_fd = fuse_mount(mountpoint, opts);
    if (-1 == fuse_fd)
    {
        fprintf(stderr, "jigdoofus: Unable to open FUSE control connection (error %d)! Abort.\n", errno);
        return errno;
    }        

    M_fuse_state = fuse_new(fuse_fd, NULL, &jdfs_oper);
    if (!M_fuse_state)
    {
        fprintf(stderr, "jigdoofus: Unable to mount filesystem (error %d)! Abort.\n", errno);
        return errno;
    }        

    fuse_loop_mt(M_fuse_state);
    fuse_unmount(mountpoint);
    
    return error;
}
