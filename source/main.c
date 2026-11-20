// SPDX-License-Identifier: MIT
// ns2-fwftp: serves this console's installed system titles (BuiltInSystem)
// over read-only FTP, so firmware can be pulled to a PC without the SD card.
#include "common.h"
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_DIR  "sdmc:/switch/ns2-fwftp"
#define LOG_FILE LOG_DIR "/log.txt"

char g_firmware[0x20] = "unknown";
static Mutex g_log_lock;
static FILE *g_log;
static char *g_logbuf;
static size_t g_loglen, g_logcap;

void log_msg(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof line) n = sizeof line - 1;

    mutexLock(&g_log_lock);
    fputs(line, stdout);
    if (g_log) {
        fputs(line, g_log);
        fflush(g_log);
    }
    if (g_loglen + n + 1 > g_logcap) {
        size_t cap = g_logcap ? g_logcap * 2 : 16384;
        while (cap < g_loglen + n + 1) cap *= 2;
        char *p = realloc(g_logbuf, cap);
        if (p) {
            g_logbuf = p;
            g_logcap = cap;
        }
    }
    if (g_loglen + n + 1 <= g_logcap) {
        memcpy(g_logbuf + g_loglen, line, n + 1);
        g_loglen += n;
    }
    mutexUnlock(&g_log_lock);
}

char *log_snapshot(size_t *len) {
    mutexLock(&g_log_lock);
    char *out = malloc(g_loglen + 1);
    if (out) {
        if (g_loglen) memcpy(out, g_logbuf, g_loglen);
        out[g_loglen] = 0;
        *len = g_loglen;
    }
    mutexUnlock(&g_log_lock);
    return out;
}

static void present(void) {
    mutexLock(&g_log_lock);
    consoleUpdate(NULL);
    mutexUnlock(&g_log_lock);
}

int main(void) {
    mutexInit(&g_log_lock);
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    mkdir(LOG_DIR, 0777);
    g_log = fopen(LOG_FILE, "w");

    log_msg("ns2-fwftp " APP_VERSION " by Onijuiku_: read-only FTP for this console's system titles\n\n");
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
            snprintf(g_firmware, sizeof g_firmware, "%s", fw.display_version);
            // kaiHEN's hbloader reports 20.0.0 to every NRO, so libounce refuses
            // 23.0.0+ commands (OpenDataStorageByPathForBatchRead) with 0x4b59.
            // Use the real version, keeping the loader's marker bits.
            u32 old = hosversionGet();
            u32 real = MAKEHOSVERSION(fw.major, fw.minor, fw.micro);
            if (real > old) {
                hosversionSet(real | (hosversionIsKaiHEN() ? BIT(30) : 0) | (hosversionIsAtmosphere() ? BIT(31) : 0));
                log_msg("hosversion: loader said 0x%x, using 0x%x\n", old, real);
            }
        }
        setsysExit();
    }
    log_msg("Firmware: %s\n", g_firmware);

    bool ncm = false, sockets = false, serving = false;
    Result rc = ncmInitialize();
    if (R_FAILED(rc)) {
        log_msg("ncmInitialize failed: " RC_FMT "\n", RC_ARGS(rc));
    } else {
        ncm = true;
        rc = titles_load();
        int programs = 0;
        for (int i = 0; i < g_title_count; i++)
            programs += g_titles[i].meta_type == NcmContentMetaType_SystemProgram;
        log_msg("Titles: %d system programs, %d system data\n", programs, g_title_count - programs);
    }

    if (ncm && g_title_count) {
        vfs_init();
        log_msg("\nProbing filesystem open methods:\n");
        present();
        vfs_probe_defaults();
        rc = socketInitializeDefault();
        if (R_FAILED(rc)) {
            log_msg("socketInitialize failed: " RC_FMT "\n", RC_ARGS(rc));
        } else {
            sockets = true;
            serving = ftp_start(FTP_PORT) == 0;
        }
    }

    if (serving) {
        struct in_addr ip = {.s_addr = (in_addr_t)gethostid()};
        log_msg("\nServing on ftp://%s:%d/ (any login, read-only)\n", inet_ntoa(ip), FTP_PORT);
        log_msg("  /index.txt     title list\n"
                "  /probe.txt     which open methods worked\n"
                "  /log.txt       this log\n"
                "  /<id>/exefs    system program code\n"
                "  /<id>/romfs    RomFS (home menu: 0400000000001000)\n"
                "  /raw           encrypted NCAs as stored\n\n");
    } else {
        log_msg("\nNot serving. Details are in " LOG_FILE "\n");
    }
    log_msg("Press + to exit.\n\n");

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        if (serving) ftp_poll();
        present();
        svcSleepThread(16000000ULL);
    }

    if (serving) ftp_stop();
    if (sockets) socketExit();
    if (ncm && g_title_count) vfs_exit();
    titles_exit();
    if (ncm) ncmExit();
    if (g_log) fclose(g_log);
    consoleExit(NULL);
    return 0;
}
