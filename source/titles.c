// SPDX-License-Identifier: MIT
// Lists the installed system programs and system data on BuiltInSystem.
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KEYS 1024

Title *g_titles;
int g_title_count;

static int by_id(const void *a, const void *b) {
    u64 x = ((const Title *)a)->id, y = ((const Title *)b)->id;
    return x < y ? -1 : x > y;
}

Title *titles_find(u64 id) {
    for (int i = 0; i < g_title_count; i++)
        if (g_titles[i].id == id) return &g_titles[i];
    return NULL;
}

Result titles_load(void) {
    NcmContentMetaDatabase db;
    NcmContentStorage cs;
    Result rc = ncmOpenContentMetaDatabase(&db, NcmStorageId_BuiltInSystem);
    if (R_FAILED(rc)) { log_msg("ncm: open meta database failed " RC_FMT "\n", RC_ARGS(rc)); return rc; }
    rc = ncmOpenContentStorage(&cs, NcmStorageId_BuiltInSystem);
    if (R_FAILED(rc)) {
        log_msg("ncm: open content storage failed " RC_FMT "\n", RC_ARGS(rc));
        ncmContentMetaDatabaseClose(&db);
        return rc;
    }

    NcmContentMetaKey *keys = calloc(MAX_KEYS, sizeof *keys);
    g_titles = calloc(MAX_KEYS, sizeof *g_titles);
    s32 total = 0, written = 0;
    if (!keys || !g_titles) { rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory); goto done; }

    rc = ncmContentMetaDatabaseList(&db, &total, &written, keys, MAX_KEYS, NcmContentMetaType_Unknown,
                                    0, 0, UINT64_MAX, NcmContentInstallType_Full);
    if (R_FAILED(rc)) { log_msg("ncm: list failed " RC_FMT "\n", RC_ARGS(rc)); goto done; }
    if (total > written) log_msg("ncm: %d entries, only the first %d are served\n", total, written);

    for (s32 i = 0; i < written; i++) {
        NcmContentMetaKey *k = &keys[i];
        NcmContentType type;
        if (k->type == NcmContentMetaType_SystemProgram) type = NcmContentType_Program;
        else if (k->type == NcmContentMetaType_SystemData) type = NcmContentType_Data;
        else continue;

        Title *t = &g_titles[g_title_count];
        memset(t, 0, sizeof *t);
        t->id = k->id;
        t->version = k->version;
        t->meta_type = k->type;
        Result r = ncmContentMetaDatabaseGetContentIdByType(&db, &t->content_id, k, type);
        if (R_FAILED(r)) { log_msg("%016lx: no content (" RC_FMT ")\n", k->id, RC_ARGS(r)); continue; }
        r = ncmContentStorageGetPath(&cs, t->path, sizeof t->path, &t->content_id);
        if (R_FAILED(r)) { log_msg("%016lx: no content path (" RC_FMT ")\n", k->id, RC_ARGS(r)); continue; }
        ncmContentStorageGetSizeFromContentId(&cs, &t->size, &t->content_id);
        for (int j = 0; j < 16; j++) sprintf(&t->cid_hex[j * 2], "%02x", t->content_id.c[j]);
        g_title_count++;
    }
    qsort(g_titles, g_title_count, sizeof *g_titles, by_id);

done:
    free(keys);
    ncmContentStorageClose(&cs);
    ncmContentMetaDatabaseClose(&db);
    return rc;
}

void titles_exit(void) {
    free(g_titles);
    g_titles = NULL;
    g_title_count = 0;
}
