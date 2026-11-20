// SPDX-License-Identifier: MIT
// The read-only tree served over FTP. Title filesystems are opened the first
// time they're used, and a few stay open (LRU).
//
// Each title filesystem is opened by trying a list of methods until one works:
// fsp-ldr OpenCodeFileSystem on kaiHEN's loader root (ExeFS), fsOpenFileSystemWithId
// with several type/attribute values, and RomFS storages (by data ID, by program
// ID, or the 23.0.0+ by-path BatchRead storage) mounted through romfsdev. The
// fsp-srv methods are also tried on HEN's own FS session, which has HEN's
// permissions instead of ours. Every attempt goes to /probe.txt, and the method that
// worked is tried first for the next title of the same kind.
#include "common.h"
#include <kaihen.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_MOUNTS 6
#define RAW_CHUNK  0x80000

enum { SUB_EXEFS, SUB_ROMFS, SUB_COUNT };
static const char *const g_sub_names[SUB_COUNT] = {"exefs", "romfs"};

// ---------------------------------------------------------------- open methods

typedef enum {
    M_WITH_ID,       // fsOpenFileSystemWithId(id, type, path, attr)
    M_WITH_PATCH,    // fsOpenFileSystemWithPatch(id, type)
    M_DATA_ID,       // fsOpenDataStorageByDataId(id, BuiltInSystem) -> romfsdev
    M_PROGRAM_ID,    // fsOpenDataStorageByProgramId(id) -> romfsdev
    M_PATH_BATCH,    // fsOpenDataStorageByPathForBatchRead(path, attr, type) -> romfsdev
    M_LDR_CODE,      // fsp-ldr OpenCodeFileSystem(attr, BuiltInSystem, id) on HEN's loader root
    M_HEN_WITH_ID,   // M_WITH_ID on HEN's FS session
    M_HEN_PROGRAM_ID,// M_PROGRAM_ID on HEN's FS session
    M_HEN_PATH_BATCH,// M_PATH_BATCH on HEN's FS session
} MethodKind;

typedef struct {
    MethodKind kind;
    u32 type;
    u8 attr;
} Method;

static const Method g_exefs_methods[] = {
    {M_LDR_CODE, 0, 1},   {M_LDR_CODE, 0, 0xF},   {M_LDR_CODE, 0, 0},
    {M_WITH_ID, 0, 1},    {M_WITH_ID, 0, 0xF},    {M_WITH_ID, 0, 0},  {M_WITH_PATCH, 0, 0},
    {M_HEN_WITH_ID, 0, 1}, {M_HEN_WITH_ID, 0, 0xF},
};
static const Method g_program_romfs_methods[] = {
    {M_PATH_BATCH, 1, 1}, {M_PATH_BATCH, 1, 0xF}, {M_PATH_BATCH, 1, 0},
    {M_PATH_BATCH, 6, 1}, {M_PATH_BATCH, 6, 0xF}, {M_PATH_BATCH, 6, 0},
    {M_HEN_PATH_BATCH, 1, 1}, {M_HEN_PATH_BATCH, 1, 0xF}, {M_HEN_PATH_BATCH, 1, 0},
    {M_HEN_PATH_BATCH, 6, 1}, {M_HEN_PATH_BATCH, 6, 0xF},
    {M_HEN_WITH_ID, 1, 1}, {M_HEN_WITH_ID, 1, 0xF}, {M_HEN_WITH_ID, 6, 1}, {M_HEN_WITH_ID, 6, 0xF},
    {M_HEN_PROGRAM_ID, 0, 0},
    {M_WITH_ID, 1, 1},    {M_WITH_ID, 1, 0xF},    {M_WITH_ID, 1, 0},
    {M_WITH_ID, 6, 1},    {M_WITH_ID, 6, 0xF},    {M_WITH_ID, 6, 0},
    {M_WITH_PATCH, 1, 0}, {M_PROGRAM_ID, 0, 0},
};
static const Method g_data_romfs_methods[] = {
    {M_DATA_ID, 0, 0},
    {M_WITH_ID, 6, 1},    {M_WITH_ID, 6, 0xF},    {M_WITH_ID, 6, 0},
    {M_WITH_ID, 1, 1},    {M_WITH_ID, 1, 0xF},    {M_WITH_ID, 1, 0},
    {M_PATH_BATCH, 6, 1}, {M_PATH_BATCH, 6, 0xF}, {M_PATH_BATCH, 6, 0},
    {M_PATH_BATCH, 1, 1}, {M_PATH_BATCH, 1, 0xF}, {M_PATH_BATCH, 1, 0},
};

enum { LIST_EXEFS, LIST_PROGRAM_ROMFS, LIST_DATA_ROMFS, LIST_COUNT };
static const struct {
    const Method *m;
    int count;
} g_lists[LIST_COUNT] = {
    {g_exefs_methods, sizeof g_exefs_methods / sizeof *g_exefs_methods},
    {g_program_romfs_methods, sizeof g_program_romfs_methods / sizeof *g_program_romfs_methods},
    {g_data_romfs_methods, sizeof g_data_romfs_methods / sizeof *g_data_romfs_methods},
};
static int g_best[LIST_COUNT]; // index of the method that last worked, -1 if none yet

static void describe(const Method *m, char *out, size_t len) {
    switch (m->kind) {
    case M_WITH_ID:    snprintf(out, len, "OpenFileSystemWithId type=%u attr=0x%x", m->type, m->attr); break;
    case M_WITH_PATCH: snprintf(out, len, "OpenFileSystemWithPatch type=%u", m->type); break;
    case M_DATA_ID:    snprintf(out, len, "OpenDataStorageByDataId (romfs)"); break;
    case M_PROGRAM_ID: snprintf(out, len, "OpenDataStorageByProgramId (romfs)"); break;
    case M_PATH_BATCH: snprintf(out, len, "OpenDataStorageByPathForBatchRead type=%u attr=0x%x (romfs)", m->type, m->attr); break;
    case M_LDR_CODE:   snprintf(out, len, "fsp-ldr OpenCodeFileSystem attr=0x%x", m->attr); break;
    case M_HEN_WITH_ID: snprintf(out, len, "HEN fs: OpenFileSystemWithId type=%u attr=0x%x", m->type, m->attr); break;
    case M_HEN_PROGRAM_ID: snprintf(out, len, "HEN fs: OpenDataStorageByProgramId (romfs)"); break;
    case M_HEN_PATH_BATCH: snprintf(out, len, "HEN fs: OpenDataStorageByPathForBatchRead type=%u attr=0x%x (romfs)", m->type, m->attr); break;
    }
}

// ---------------------------------------------------------------- state

typedef enum { BK_FS, BK_ROMFS } Backend;

typedef struct {
    bool used;
    Title *t;
    int sub;
    Backend backend;
    FsFileSystem fs; // BK_FS
    char dev[8];     // BK_ROMFS: romfsdev device name, "m<slot>"
    int refs;
    u64 tick;
} Mount;

enum { VF_MEM, VF_RAW, VF_FS, VF_STDIO };
struct VFile {
    int kind;
    s64 size;
    char *mem; // VF_MEM, owned copy
    Title *t;
    Mount *m;
    FsFile file; // VF_FS
    FILE *fp;    // VF_STDIO
};

typedef enum { P_BAD, P_ROOT, P_INDEX, P_LOG, P_PROBE, P_RAWDIR, P_RAW, P_TITLE, P_SUB } PKind;
typedef struct {
    PKind kind;
    Title *t;
    int sub;
    const char *inner; // path inside the title filesystem, starts with '/'
} VPath;

static Mount g_mounts[MAX_MOUNTS];
static Mutex g_lock;
static u64 g_tick;
static Result (*g_fail)[SUB_COUNT]; // per title: mount failure (last attempt's result), 0 if none
static bool (*g_probed)[SUB_COUNT]; // per title: already in /probe.txt
static NcmContentStorage g_cs;
static bool g_cs_open;
static char *g_index;
static size_t g_index_len;
static char *g_probe;
static size_t g_probe_len, g_probe_cap;
static Service g_hen_fs, g_ldr; // HEN's fsp-srv and fsp-ldr root clones (non-domain)
static bool g_have_hen_fs, g_have_ldr;

static void probe_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void probe_add(const char *fmt, ...) {
    char line[FS_MAX_PATH + 160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof line) n = sizeof line - 1;
    if (g_probe_len + n + 1 > g_probe_cap) {
        size_t cap = g_probe_cap ? g_probe_cap * 2 : 16384;
        while (cap < g_probe_len + n + 1) cap *= 2;
        char *p = realloc(g_probe, cap);
        if (!p) return;
        g_probe = p;
        g_probe_cap = cap;
    }
    memcpy(g_probe + g_probe_len, line, n + 1);
    g_probe_len += n;
}

// ---------------------------------------------------------------- mounts

// ---------------------------------------------------------------- HEN sessions

static Result unavailable(void) { return MAKERESULT(Module_Libnx, LibnxError_NotInitialized); }

static Result ldr_open_code(Title *t, u8 attr, FsFileSystem *out) {
    if (!g_have_ldr) return unavailable();
    // Switch 2 layout, as kaiHEN sends it (common/policy_package.cpp).
    const struct {
        u8 attr;
        u8 storage_id;
        u8 pad[6];
        u64 program_id;
    } in = {attr, NcmStorageId_BuiltInSystem, {0}, t->id};
    static u8 code_info[0x1a4];
    return serviceDispatchIn(&g_ldr, 0, in,
        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
        .buffers = {{code_info, sizeof code_info}},
        .out_num_objects = 1,
        .out_objects = &out->s);
}

static Result hen_with_id(Title *t, u32 type, u8 attr, FsFileSystem *out) {
    if (!g_have_hen_fs) return unavailable();
    const struct {
        u8 attr;
        u32 type;
        u64 id;
    } in = {attr, type, t->id};
    return serviceDispatchIn(&g_hen_fs, 10, in,
        .buffer_attrs = {SfBufferAttr_HipcPointer | SfBufferAttr_In},
        .buffers = {{t->path, FS_MAX_PATH}},
        .out_num_objects = 1,
        .out_objects = &out->s);
}

static Result hen_program_id(Title *t, FsStorage *out) {
    if (!g_have_hen_fs) return unavailable();
    return serviceDispatchIn(&g_hen_fs, 201, t->id, .out_num_objects = 1, .out_objects = &out->s);
}

static Result hen_path_batch(Title *t, u32 type, u8 attr, FsStorage *out) {
    if (!g_have_hen_fs) return unavailable();
    const struct {
        u8 attr;
        u8 pad[3];
        u32 type;
    } in = {attr, {0}, type};
    return serviceDispatchIn(&g_hen_fs, 213, in,
        .buffer_attrs = {SfBufferAttr_HipcPointer | SfBufferAttr_In},
        .buffers = {{t->path, FS_MAX_PATH}},
        .out_num_objects = 1,
        .out_objects = &out->s);
}

static void hen_open(void) {
    Service ro, hen;
    Result rc = kaihenGetRoSession(&ro);
    if (R_SUCCEEDED(rc)) rc = kaihenOpenHomebrewSession(&ro, &hen);
    if (R_FAILED(rc)) {
        log_msg("HEN session: " RC_FMT " (HEN methods skipped)\n", RC_ARGS(rc));
        probe_add("HEN homebrew session: " RC_FMT "\n", RC_ARGS(rc));
        return;
    }
    Handle h = INVALID_HANDLE;
    Result fs_rc = henGetFilesystemRoot(&hen, &h);
    if (R_SUCCEEDED(fs_rc)) {
        serviceCreate(&g_hen_fs, h);
        g_have_hen_fs = true;
    }
    h = INVALID_HANDLE;
    Result ldr_rc = henGetLoaderRoot(&hen, &h);
    if (R_SUCCEEDED(ldr_rc)) {
        serviceCreate(&g_ldr, h);
        g_have_ldr = true;
    }
    serviceClose(&hen);
    probe_add("HEN fs root: %s, loader root: %s\n\n", g_have_hen_fs ? "OK" : "failed",
              g_have_ldr ? "OK" : "failed");
    if (R_FAILED(fs_rc)) log_msg("HEN fs root failed: " RC_FMT "\n", RC_ARGS(fs_rc));
    if (R_FAILED(ldr_rc)) log_msg("HEN loader root failed: " RC_FMT " (no ExeFS)\n", RC_ARGS(ldr_rc));
}

static Result try_method(const Method *m, Title *t, Mount *slot) {
    if (m->kind == M_LDR_CODE)
        return ldr_open_code(t, m->attr, &slot->fs);
    if (m->kind == M_HEN_WITH_ID)
        return hen_with_id(t, m->type, m->attr, &slot->fs);
    if (m->kind == M_WITH_ID)
        return fsOpenFileSystemWithId(&slot->fs, t->id, (FsFileSystemType)m->type, t->path, (FsContentAttributes)m->attr);
    if (m->kind == M_WITH_PATCH)
        return fsOpenFileSystemWithPatch(&slot->fs, t->id, (FsFileSystemType)m->type);

    FsStorage storage;
    Result rc;
    if (m->kind == M_DATA_ID) {
        rc = fsOpenDataStorageByDataId(&storage, t->id, NcmStorageId_BuiltInSystem);
    } else if (m->kind == M_PROGRAM_ID) {
        rc = fsOpenDataStorageByProgramId(&storage, t->id);
    } else if (m->kind == M_HEN_PROGRAM_ID) {
        rc = hen_program_id(t, &storage);
    } else if (m->kind == M_HEN_PATH_BATCH) {
        rc = hen_path_batch(t, m->type, m->attr, &storage);
    } else {
        // IStorageForBatchRead keeps IStorage's Read (0) and GetSize (4), which is all romfsdev uses.
        FsStorageForBatchRead batch;
        rc = fsOpenDataStorageByPathForBatchRead(&batch, t->path, (FsContentAttributes)m->attr, (FsFileSystemType)m->type);
        if (R_SUCCEEDED(rc)) storage.s = batch.s;
    }
    if (R_FAILED(rc)) return rc;
    return romfsMountFromStorage(storage, 0, slot->dev); // closes the storage itself on failure
}

// FS 2002-1001 (PartitionNotFound): the NCA has no such section, e.g. a system
// module without a RomFS. The upper result bits vary on Switch 2, so compare
// module and description only.
static bool no_partition(Result rc) {
    return R_FAILED(rc) && R_MODULE(rc) == 2 /* fs */ && R_DESCRIPTION(rc) == 1001;
}

// Opens t's sub filesystem into slot, trying each method. Caller holds g_lock.
static Result open_title(Title *t, int sub, Mount *slot) {
    int list = sub == SUB_EXEFS ? LIST_EXEFS
             : t->meta_type == NcmContentMetaType_SystemData ? LIST_DATA_ROMFS : LIST_PROGRAM_ROMFS;
    const Method *methods = g_lists[list].m;
    int count = g_lists[list].count, best = g_best[list];
    char what[96];

    // Titles are reopened after LRU eviction: record only the first open of each.
    bool *probed = &g_probed[t - g_titles][sub];
    bool quiet = *probed;
    *probed = true;
    if (!quiet)
        probe_add("%016lx %s (%s, path %s)\n", t->id, g_sub_names[sub],
                  t->meta_type == NcmContentMetaType_SystemData ? "data" : "program", t->path);
    Result rc = 0;
    for (int k = -1; k < count; k++) {
        int i = k < 0 ? best : k;
        if (i < 0 || (k >= 0 && i == best)) continue;
        const Method *m = &methods[i];
        rc = try_method(m, t, slot);
        describe(m, what, sizeof what);
        if (quiet && R_FAILED(rc) && k >= 0) {
            // A reopen that the first open's method no longer serves: log it after all.
            quiet = false;
            probe_add("%016lx %s (reopen)\n", t->id, g_sub_names[sub]);
        }
        if (!quiet) {
            if (R_SUCCEEDED(rc)) probe_add("  %-66s -> OK\n", what);
            else probe_add("  %-66s -> " RC_FMT "\n", what, RC_ARGS(rc));
        }
        if (R_SUCCEEDED(rc)) {
            slot->backend = m->kind == M_WITH_ID || m->kind == M_WITH_PATCH || m->kind == M_LDR_CODE ||
                                    m->kind == M_HEN_WITH_ID ? BK_FS : BK_ROMFS;
            if (best != i) log_msg("%s: using %s\n", g_sub_names[sub], what);
            g_best[list] = i;
            return 0;
        }
        if (k < 0 && no_partition(rc)) {
            // The method known to work says the section doesn't exist: done.
            if (!quiet) probe_add("  (no %s section in this title)\n", g_sub_names[sub]);
            return rc;
        }
    }
    return rc;
}

static void mount_close(Mount *m) {
    if (!m->used) return;
    if (m->backend == BK_FS) fsFsClose(&m->fs);
    else romfsUnmount(m->dev);
    m->used = false;
}

static Mount *mount_get(Title *t, int sub, char *err, size_t err_len) {
    int ti = t - g_titles;
    mutexLock(&g_lock);
    if (g_fail[ti][sub]) {
        if (no_partition(g_fail[ti][sub])) snprintf(err, err_len, "this title has no %s", g_sub_names[sub]);
        else snprintf(err, err_len, "%s not available: " RC_FMT " (see /probe.txt)", g_sub_names[sub], RC_ARGS(g_fail[ti][sub]));
        mutexUnlock(&g_lock);
        return NULL;
    }
    Mount *m = NULL;
    for (int i = 0; i < MAX_MOUNTS && !m; i++)
        if (g_mounts[i].used && g_mounts[i].t == t && g_mounts[i].sub == sub) m = &g_mounts[i];
    if (!m) {
        for (int i = 0; i < MAX_MOUNTS && !m; i++)
            if (!g_mounts[i].used) m = &g_mounts[i];
        for (int i = 0; i < MAX_MOUNTS && !m; i++)
            if (g_mounts[i].refs == 0 && (!m || g_mounts[i].tick < m->tick)) m = &g_mounts[i];
        if (!m) {
            snprintf(err, err_len, "too many filesystems in use");
            mutexUnlock(&g_lock);
            return NULL;
        }
        mount_close(m);
        Result rc = open_title(t, sub, m);
        if (R_FAILED(rc)) {
            g_fail[ti][sub] = rc;
            if (no_partition(rc)) {
                snprintf(err, err_len, "this title has no %s", g_sub_names[sub]);
            } else {
                log_msg("%016lx %s: every open method failed (last " RC_FMT ")\n", t->id, g_sub_names[sub], RC_ARGS(rc));
                snprintf(err, err_len, "%s not available: " RC_FMT " (see /probe.txt)", g_sub_names[sub], RC_ARGS(rc));
            }
            mutexUnlock(&g_lock);
            return NULL;
        }
        m->used = true;
        m->t = t;
        m->sub = sub;
        m->refs = 0;
    }
    m->refs++;
    m->tick = ++g_tick;
    mutexUnlock(&g_lock);
    return m;
}

static void mount_put(Mount *m) {
    mutexLock(&g_lock);
    m->refs--;
    mutexUnlock(&g_lock);
}

static void dev_path(const Mount *m, const char *inner, char *out, size_t len) {
    snprintf(out, len, "%s:%s", m->dev, inner);
}

// ---------------------------------------------------------------- paths

static bool parse_id(const char *s, size_t n, u64 *out) {
    if (n != 16) return false;
    u64 v = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = v << 4 | d;
    }
    *out = v;
    return true;
}

static bool is(const char *s, size_t n, const char *word) {
    return strlen(word) == n && !strncmp(s, word, n);
}

static VPath parse(const char *path) {
    VPath p = {P_BAD, NULL, 0, NULL};
    if (path[0] != '/') return p;
    if (!path[1]) { p.kind = P_ROOT; return p; }

    const char *s = path + 1, *slash = strchr(s, '/');
    size_t n = slash ? (size_t)(slash - s) : strlen(s);
    u64 id;
    if (!slash && is(s, n, "index.txt")) { p.kind = P_INDEX; return p; }
    if (!slash && is(s, n, "log.txt")) { p.kind = P_LOG; return p; }
    if (!slash && is(s, n, "probe.txt")) { p.kind = P_PROBE; return p; }
    if (is(s, n, "raw")) {
        if (!slash) { p.kind = P_RAWDIR; return p; }
        const char *name = slash + 1; // <id>_<content id>.nca
        if (strlen(name) != 16 + 1 + 32 + 4 || !parse_id(name, 16, &id) || name[16] != '_') return p;
        Title *t = titles_find(id);
        if (t && !strncmp(name + 17, t->cid_hex, 32) && !strcmp(name + 49, ".nca")) {
            p.kind = P_RAW;
            p.t = t;
        }
        return p;
    }
    if (!parse_id(s, n, &id) || !(p.t = titles_find(id))) return p;
    if (!slash) { p.kind = P_TITLE; return p; }

    s = slash + 1;
    slash = strchr(s, '/');
    n = slash ? (size_t)(slash - s) : strlen(s);
    if (is(s, n, "exefs") && p.t->meta_type == NcmContentMetaType_SystemProgram) p.sub = SUB_EXEFS;
    else if (is(s, n, "romfs")) p.sub = SUB_ROMFS;
    else return p;
    p.kind = P_SUB;
    p.inner = slash ? slash : "/";
    return p;
}

// ---------------------------------------------------------------- setup

void vfs_init(void) {
    mutexInit(&g_lock);
    for (int i = 0; i < LIST_COUNT; i++) g_best[i] = -1;
    for (int i = 0; i < MAX_MOUNTS; i++) snprintf(g_mounts[i].dev, sizeof g_mounts[i].dev, "m%d", i);
    g_fail = calloc(g_title_count ? g_title_count : 1, sizeof *g_fail);
    g_probed = calloc(g_title_count ? g_title_count : 1, sizeof *g_probed);
    g_cs_open = R_SUCCEEDED(ncmOpenContentStorage(&g_cs, NcmStorageId_BuiltInSystem));
    probe_add("ns2-fwftp filesystem open attempts, firmware %s, hosversion 0x%x\n", g_firmware, hosversionGet());
    hen_open();

    size_t cap = 512 + (size_t)g_title_count * 128;
    g_index = malloc(cap);
    if (!g_index) return;
    size_t n = snprintf(g_index, cap,
                        "ns2-fwftp system title inventory\nfirmware: %s\ntitles: %d\n\n"
                        "%-16s  %-7s  %-10s  %-32s  %s\n",
                        g_firmware, g_title_count, "id", "type", "version", "content id", "nca size");
    for (int i = 0; i < g_title_count && n < cap; i++) {
        Title *t = &g_titles[i];
        n += snprintf(g_index + n, cap - n, "%016lx  %-7s  %-10u  %s  %ld\n", t->id,
                      t->meta_type == NcmContentMetaType_SystemProgram ? "program" : "data", t->version,
                      t->cid_hex, t->size);
    }
    g_index_len = n < cap ? n : cap - 1;
}

void vfs_probe_defaults(void) {
    char err[128];
    bool data_done = false;
    for (int i = 0; i < g_title_count; i++) {
        Title *t = &g_titles[i];
        bool home = (t->id & 0x00FFFFFFFFFFFFFFULL) == 0x1000 && t->meta_type == NcmContentMetaType_SystemProgram;
        bool data = !data_done && t->meta_type == NcmContentMetaType_SystemData;
        if (!home && !data) continue;
        data_done |= data;
        for (int sub = home ? SUB_EXEFS : SUB_ROMFS; sub < SUB_COUNT; sub++) {
            Mount *m = mount_get(t, sub, err, sizeof err);
            log_msg("%016lx %s: %s\n", t->id, g_sub_names[sub], m ? "OK" : err);
            if (m) mount_put(m);
        }
    }
}

void vfs_exit(void) {
    for (int i = 0; i < MAX_MOUNTS; i++) mount_close(&g_mounts[i]);
    if (g_cs_open) ncmContentStorageClose(&g_cs);
    g_cs_open = false;
    if (g_have_hen_fs) serviceClose(&g_hen_fs);
    if (g_have_ldr) serviceClose(&g_ldr);
    g_have_hen_fs = g_have_ldr = false;
    free(g_fail);
    free(g_probed);
    g_probed = NULL;
    free(g_index);
    free(g_probe);
    g_fail = NULL;
    g_index = g_probe = NULL;
    g_probe_len = g_probe_cap = 0;
}

// ---------------------------------------------------------------- operations

static void raw_name(const Title *t, char *out, size_t len) {
    snprintf(out, len, "%016lx_%s.nca", t->id, t->cid_hex);
}

// Snapshot of a generated text file. Caller frees.
static char *text_copy(PKind kind, size_t *len) {
    if (kind == P_LOG) return log_snapshot(len);
    mutexLock(&g_lock);
    const char *src = kind == P_INDEX ? g_index : g_probe;
    size_t n = kind == P_INDEX ? g_index_len : g_probe_len;
    char *out = malloc(n + 1);
    if (out) {
        if (n) memcpy(out, src, n);
        out[n] = 0;
        *len = n;
    }
    mutexUnlock(&g_lock);
    return out;
}

static s64 text_size(PKind kind) {
    size_t len = 0;
    char *s = text_copy(kind, &len);
    free(s);
    return len;
}

// stat inside a title filesystem
static int sub_stat(Mount *m, const char *inner, bool *is_dir, s64 *size, char *err, size_t err_len) {
    if (!strcmp(inner, "/")) {
        *is_dir = true;
        return 0;
    }
    if (m->backend == BK_ROMFS) {
        char full[FS_MAX_PATH + 16];
        struct stat st;
        dev_path(m, inner, full, sizeof full);
        if (stat(full, &st) < 0) {
            snprintf(err, err_len, "no such file (errno %d)", errno);
            return -1;
        }
        *is_dir = S_ISDIR(st.st_mode);
        *size = *is_dir ? 0 : st.st_size;
        return 0;
    }
    FsDirEntryType type;
    Result rc = fsFsGetEntryType(&m->fs, inner, &type);
    if (R_FAILED(rc)) {
        snprintf(err, err_len, "no such file (" RC_FMT ")", RC_ARGS(rc));
        return -1;
    }
    *is_dir = type == FsDirEntryType_Dir;
    FsFile f;
    if (!*is_dir && R_SUCCEEDED(fsFsOpenFile(&m->fs, inner, FsOpenMode_Read, &f))) {
        fsFileGetSize(&f, size);
        fsFileClose(&f);
    }
    return 0;
}

int vfs_stat(const char *path, bool *is_dir, s64 *size, char *err, size_t err_len) {
    VPath p = parse(path);
    *size = 0;
    *is_dir = false;
    switch (p.kind) {
    case P_ROOT: case P_RAWDIR: case P_TITLE:
        *is_dir = true;
        return 0;
    case P_INDEX: case P_LOG: case P_PROBE:
        *size = text_size(p.kind);
        return 0;
    case P_RAW:
        *size = p.t->size;
        return 0;
    case P_SUB: {
        Mount *m = mount_get(p.t, p.sub, err, err_len);
        if (!m) return -1;
        int ret = sub_stat(m, p.inner, is_dir, size, err, err_len);
        mount_put(m);
        return ret;
    }
    default:
        snprintf(err, err_len, "no such file or directory");
        return -1;
    }
}

static int list_sub(Mount *m, const char *inner, VfsListCb cb, void *ctx, char *err, size_t err_len) {
    if (m->backend == BK_ROMFS) {
        char full[FS_MAX_PATH + 16], child[FS_MAX_PATH + 16 + 256];
        dev_path(m, inner, full, sizeof full);
        DIR *d = opendir(full);
        if (!d) {
            snprintf(err, err_len, "cannot open directory (errno %d)", errno);
            return -1;
        }
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            struct stat st;
            snprintf(child, sizeof child, "%s%s%s", full, full[strlen(full) - 1] == '/' ? "" : "/", e->d_name);
            bool dir = stat(child, &st) == 0 && S_ISDIR(st.st_mode);
            cb(ctx, e->d_name, dir, dir ? 0 : st.st_size);
        }
        closedir(d);
        return 0;
    }
    FsDir dir;
    Result rc = fsFsOpenDirectory(&m->fs, inner, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &dir);
    if (R_FAILED(rc)) {
        snprintf(err, err_len, "cannot open directory (" RC_FMT ")", RC_ARGS(rc));
        return -1;
    }
    enum { BATCH = 32 };
    FsDirectoryEntry *entries = malloc(BATCH * sizeof *entries);
    s64 count = 0;
    while (entries && R_SUCCEEDED(fsDirRead(&dir, &count, BATCH, entries)) && count > 0)
        for (s64 i = 0; i < count; i++)
            cb(ctx, entries[i].name, entries[i].type == FsDirEntryType_Dir, entries[i].file_size);
    free(entries);
    fsDirClose(&dir);
    return 0;
}

int vfs_list(const char *path, VfsListCb cb, void *ctx, char *err, size_t err_len) {
    VPath p = parse(path);
    char name[64];
    switch (p.kind) {
    case P_ROOT:
        cb(ctx, "index.txt", false, text_size(P_INDEX));
        cb(ctx, "log.txt", false, text_size(P_LOG));
        cb(ctx, "probe.txt", false, text_size(P_PROBE));
        cb(ctx, "raw", true, 0);
        for (int i = 0; i < g_title_count; i++) {
            snprintf(name, sizeof name, "%016lx", g_titles[i].id);
            cb(ctx, name, true, 0);
        }
        return 0;
    case P_RAWDIR:
        for (int i = 0; i < g_title_count; i++) {
            raw_name(&g_titles[i], name, sizeof name);
            cb(ctx, name, false, g_titles[i].size);
        }
        return 0;
    case P_TITLE: {
        bool romfs = true;
        if (p.t->meta_type == NcmContentMetaType_SystemProgram) {
            cb(ctx, "exefs", true, 0);
            // Most system modules have no RomFS: leave the folder out for those.
            char e[128];
            Mount *m = mount_get(p.t, SUB_ROMFS, e, sizeof e);
            if (m) mount_put(m);
            else romfs = !no_partition(g_fail[p.t - g_titles][SUB_ROMFS]);
        }
        if (romfs) cb(ctx, "romfs", true, 0);
        return 0;
    }
    case P_SUB: {
        Mount *m = mount_get(p.t, p.sub, err, err_len);
        if (!m) return -1;
        int ret = list_sub(m, p.inner, cb, ctx, err, err_len);
        mount_put(m);
        return ret;
    }
    case P_INDEX: case P_LOG: case P_PROBE: case P_RAW:
        snprintf(err, err_len, "not a directory");
        return -1;
    default:
        snprintf(err, err_len, "no such file or directory");
        return -1;
    }
}

VFile *vfs_open(const char *path, s64 *size, char *err, size_t err_len) {
    VPath p = parse(path);
    VFile *f = calloc(1, sizeof *f);
    if (!f) { snprintf(err, err_len, "out of memory"); return NULL; }
    switch (p.kind) {
    case P_INDEX: case P_LOG: case P_PROBE: {
        size_t len = 0;
        f->kind = VF_MEM;
        f->mem = text_copy(p.kind, &len);
        if (!f->mem) { snprintf(err, err_len, "out of memory"); goto fail; }
        f->size = len;
        break;
    }
    case P_RAW:
        if (!g_cs_open) { snprintf(err, err_len, "content storage unavailable"); goto fail; }
        f->kind = VF_RAW;
        f->t = p.t;
        f->size = p.t->size;
        break;
    case P_SUB: {
        if (!strcmp(p.inner, "/")) { snprintf(err, err_len, "is a directory"); goto fail; }
        f->m = mount_get(p.t, p.sub, err, err_len);
        if (!f->m) goto fail;
        if (f->m->backend == BK_ROMFS) {
            char full[FS_MAX_PATH + 16];
            struct stat st;
            dev_path(f->m, p.inner, full, sizeof full);
            f->fp = stat(full, &st) == 0 && !S_ISDIR(st.st_mode) ? fopen(full, "rb") : NULL;
            if (!f->fp) {
                mount_put(f->m);
                snprintf(err, err_len, "cannot open file (errno %d)", errno);
                goto fail;
            }
            setvbuf(f->fp, NULL, _IONBF, 0);
            f->kind = VF_STDIO;
            f->size = st.st_size;
        } else {
            Result rc = fsFsOpenFile(&f->m->fs, p.inner, FsOpenMode_Read, &f->file);
            if (R_FAILED(rc)) {
                mount_put(f->m);
                snprintf(err, err_len, "cannot open file (" RC_FMT ")", RC_ARGS(rc));
                goto fail;
            }
            f->kind = VF_FS;
            fsFileGetSize(&f->file, &f->size);
        }
        break;
    }
    case P_ROOT: case P_RAWDIR: case P_TITLE:
        snprintf(err, err_len, "is a directory");
        goto fail;
    default:
        snprintf(err, err_len, "no such file");
        goto fail;
    }
    *size = f->size;
    return f;
fail:
    free(f);
    return NULL;
}

s64 vfs_read(VFile *f, s64 offset, void *buf, size_t len) {
    if (offset >= f->size) return 0;
    if ((s64)len > f->size - offset) len = f->size - offset;
    switch (f->kind) {
    case VF_MEM:
        memcpy(buf, f->mem + offset, len);
        return len;
    case VF_RAW:
        if (len > RAW_CHUNK) len = RAW_CHUNK;
        return R_SUCCEEDED(ncmContentStorageReadContentIdFile(&g_cs, buf, len, &f->t->content_id, offset)) ? (s64)len : -1;
    case VF_FS: {
        u64 got = 0;
        return R_SUCCEEDED(fsFileRead(&f->file, offset, buf, len, FsReadOption_None, &got)) ? (s64)got : -1;
    }
    case VF_STDIO: {
        if (fseeko(f->fp, offset, SEEK_SET) != 0) return -1;
        size_t got = fread(buf, 1, len, f->fp);
        return got ? (s64)got : -1;
    }
    }
    return -1;
}

void vfs_close(VFile *f) {
    if (!f) return;
    if (f->kind == VF_FS) {
        fsFileClose(&f->file);
        mount_put(f->m);
    } else if (f->kind == VF_STDIO) {
        fclose(f->fp);
        mount_put(f->m);
    }
    free(f->mem);
    free(f);
}
