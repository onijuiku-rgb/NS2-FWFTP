// SPDX-License-Identifier: MIT
#pragma once
#include <switch.h>
#include <stdbool.h>
#include <stddef.h>

#define FTP_PORT 1337

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

// Installed system title from the BuiltInSystem content meta database.
typedef struct {
    u64 id;
    u32 version;
    u8 meta_type;               // NcmContentMetaType_SystemProgram or _SystemData
    NcmContentId content_id;    // its Program or Data content
    s64 size;                   // NCA size in bytes
    char path[FS_MAX_PATH];     // content path from NCM, for fsOpenFileSystemWithId
    char cid_hex[33];
} Title;

extern Title *g_titles;
extern int g_title_count;
extern char g_firmware[0x20];

// Result as the error code the system shows, e.g. 2002-1001. Switch 2 sets
// varying bits above the module and description, so print only those.
#define RC_FMT "%04u-%04u"
#define RC_ARGS(rc) (unsigned)(2000 + R_MODULE(rc)), (unsigned)R_DESCRIPTION(rc)

void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Copy of everything logged so far (malloc'd, caller frees), served as /log.txt.
char *log_snapshot(size_t *len);

// titles.c
Result titles_load(void);
void titles_exit(void);
Title *titles_find(u64 id);

// vfs.c: the tree the FTP server serves, all read-only.
//   /index.txt                      title inventory
//   /log.txt                        this session's log
//   /probe.txt                      every filesystem open attempt and its result
//   /raw/<id>_<content id>.nca      the encrypted NCA as stored
//   /<id>/exefs/...                 a system program's code filesystem
//   /<id>/romfs/...                 a program's or system data's RomFS
typedef void (*VfsListCb)(void *ctx, const char *name, bool is_dir, s64 size);
typedef struct VFile VFile;

void vfs_init(void);
void vfs_exit(void);
// Opens the home menu's filesystems and one system data title, so /probe.txt
// shows which open methods this firmware accepts.
void vfs_probe_defaults(void);
// path is absolute and normalized ("/", "/raw", "/0100000000001000/romfs/lyt").
// Each returns 0 on success, or a negative value with a short reason in err.
int vfs_stat(const char *path, bool *is_dir, s64 *size, char *err, size_t err_len);
int vfs_list(const char *path, VfsListCb cb, void *ctx, char *err, size_t err_len);
VFile *vfs_open(const char *path, s64 *size, char *err, size_t err_len);
s64 vfs_read(VFile *f, s64 offset, void *buf, size_t len);
void vfs_close(VFile *f);

// ftp.c
int ftp_start(u16 port);
void ftp_poll(void);
void ftp_stop(void);
