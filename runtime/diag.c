// Crash-safe logger, startup phase tracker, CPUID/system inspector, watchdog, and minidump crash handler.
#define _GNU_SOURCE
#include "diag.h"
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <shlobj.h>
#include <intrin.h>
#include <io.h>
#include <fcntl.h>
#else
#include <cpuid.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <unistd.h>
#include <sys/utsname.h>
#endif

#include "rt.h"
#include "kernel.h"

#ifndef MHGU_GIT_COMMIT
#define MHGU_GIT_COMMIT "v3.2-diag"
#endif

#ifndef MHGU_BUILD_FLAGS
#define MHGU_BUILD_FLAGS "-march=bdver3 -mtune=bdver3 -mno-avx2 -mno-bmi2 -mno-movbe"
#endif

volatile int g_diag_level = DIAG_INFO;
volatile int g_diag_console = -1;     // -1: auto, 0: off, 1: on
volatile int g_diag_keep_files = 5;   // default: keep last 5 logs (log.txt, log.1.txt .. log.4.txt)
volatile int g_diag_watchdog_sec = 15;
volatile int g_diag_vk_validation = 0;
volatile int g_diag_minidump = 1;

static _Atomic bool g_diag_initialized = false;
static uint64_t g_start_ns = 0;
static char g_log_dir[1024] = ".";
static char g_log_file_path[1200] = "log.txt";
static char g_crash_dir[1200] = "crash";
static bool g_used_fallback_dir = false;
static char g_fallback_reason[256] = "";

#ifdef _WIN32
static HANDLE g_log_handle = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_log_cs;
static bool g_log_cs_init = false;
#else
static int g_log_fd = -1;
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;
#endif

// ============================================================================
// 1. In-memory Lock-Free TRACE Ring Buffer (Zero Heap Allocation)
// ============================================================================
#define TRACE_RING_SLOTS 256
#define TRACE_RING_LINE  256

static char g_trace_ring[TRACE_RING_SLOTS][TRACE_RING_LINE];
static _Atomic uint64_t g_trace_ring_seq = 0;

// ============================================================================
// 2. Thread Registry & Naming
// ============================================================================
#define MAX_DIAG_THREADS 128
typedef struct {
    _Atomic bool active;
    uint32_t os_tid;
    uint32_t guest_handle;
    uint32_t guest_entry;
    uint64_t start_ns;
    char name[48];
#ifdef _WIN32
    HANDLE win_handle;
#endif
} DiagThreadInfo;

static DiagThreadInfo g_threads_reg[MAX_DIAG_THREADS];
static __thread char t_thread_name[48] = "";
static __thread uint32_t t_os_tid = 0;
static __thread int t_reg_slot = -1;

static uint32_t get_os_tid(void) {
    if (t_os_tid) return t_os_tid;
#ifdef _WIN32
    t_os_tid = (uint32_t)GetCurrentThreadId();
#else
    t_os_tid = (uint32_t)pthread_self();
#endif
    return t_os_tid;
}

const char *diag_thread_name(void) {
    if (t_thread_name[0]) return t_thread_name;
    uint32_t gh = kernel_current_thread_handle();
    if (gh) {
        snprintf(t_thread_name, sizeof t_thread_name, "guest:%x", gh);
        return t_thread_name;
    }
    return "thread";
}

#ifdef _WIN32
typedef HRESULT(WINAPI *PFN_SetThreadDescription)(HANDLE, PCWSTR);
static void set_os_thread_name(const char *name) {
    static PFN_SetThreadDescription pSetThreadDesc = NULL;
    static bool checked = false;
    if (!checked) {
        checked = true;
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        if (k32) pSetThreadDesc = (PFN_SetThreadDescription)(void *)GetProcAddress(k32, "SetThreadDescription");
    }
    if (pSetThreadDesc) {
        wchar_t wname[64];
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 64);
        pSetThreadDesc(GetCurrentThread(), wname);
    }
}
#else
static void set_os_thread_name(const char *name) {
    pthread_setname_np(pthread_self(), name);
}
#endif

void diag_thread_register(const char *name, uint32_t guest_handle, uint32_t guest_entry) {
    snprintf(t_thread_name, sizeof t_thread_name, "%s", name ? name : "worker");
    set_os_thread_name(t_thread_name);
    uint32_t tid = get_os_tid();
    uint64_t now = plat_now_ns();
    for (int i = 0; i < MAX_DIAG_THREADS; i++) {
        bool expected = false;
        if (atomic_compare_exchange_strong(&g_threads_reg[i].active, &expected, true)) {
            g_threads_reg[i].os_tid = tid;
            g_threads_reg[i].guest_handle = guest_handle;
            g_threads_reg[i].guest_entry = guest_entry;
            g_threads_reg[i].start_ns = now;
            snprintf(g_threads_reg[i].name, sizeof g_threads_reg[i].name, "%s", t_thread_name);
#ifdef _WIN32
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                            &g_threads_reg[i].win_handle, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                            FALSE, 0);
#endif
            t_reg_slot = i;
            break;
        }
    }
    if (guest_entry)
        diag_log(DIAG_INFO, "thread", "Thread started: name='%s' os_tid=%u (0x%x) guest_handle=0x%x guest_entry=0x%08x\n",
                 t_thread_name, tid, tid, guest_handle, guest_entry);
    else
        diag_log(DIAG_INFO, "thread", "Thread started: name='%s' os_tid=%u (0x%x)\n", t_thread_name, tid, tid);
}

void diag_thread_unregister(void) {
    uint32_t tid = get_os_tid();
    int slot = t_reg_slot;
    if (slot >= 0 && slot < MAX_DIAG_THREADS && atomic_load(&g_threads_reg[slot].active)) {
        double dur_ms = (double)(plat_now_ns() - g_threads_reg[slot].start_ns) / 1e6;
        diag_log(DIAG_INFO, "thread", "Thread exited: name='%s' os_tid=%u (0x%x) duration=%.2f ms\n",
                 g_threads_reg[slot].name, tid, tid, dur_ms);
#ifdef _WIN32
        if (g_threads_reg[slot].win_handle) {
            CloseHandle(g_threads_reg[slot].win_handle);
            g_threads_reg[slot].win_handle = NULL;
        }
#endif
        atomic_store(&g_threads_reg[slot].active, false);
        t_reg_slot = -1;
    } else {
        diag_log(DIAG_INFO, "thread", "Thread exited: name='%s' os_tid=%u (0x%x)\n", diag_thread_name(), tid, tid);
    }
}

// ============================================================================
// 3. Built-in Dependency-Free SHA-256 Implementation
// ============================================================================
typedef struct {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t buffer[64];
} Sha256Ctx;

static const uint32_t k_sha256[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfc,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_transform(Sha256Ctx *ctx, const uint8_t data[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) | (uint32_t)data[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + k_sha256[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static bool sha256_file(const char *path, char out_hex[65], uint64_t *out_size, bool *has_tramp) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    Sha256Ctx ctx;
    ctx.state[0] = 0x6a09e667u; ctx.state[1] = 0xbb67ae85u;
    ctx.state[2] = 0x3c6ef372u; ctx.state[3] = 0xa54ff53au;
    ctx.state[4] = 0x510e527fu; ctx.state[5] = 0x9b05688cu;
    ctx.state[6] = 0x1f83d9abu; ctx.state[7] = 0x5be0cd19u;
    ctx.bitcount = 0;

    uint8_t buf[65536];
    uint64_t total = 0;
    size_t n;
    bool first_chunk = true;
    if (has_tramp) *has_tramp = false;

    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) {
        if (first_chunk && has_tramp && n >= 0x400 && buf[0] == 'M' && buf[1] == 'Z') {
            uint32_t pe_off = *(uint32_t *)(buf + 0x3C);
            if (pe_off + 24 < n && memcmp(buf + pe_off, "PE\0\0", 4) == 0) {
                uint16_t nsec = *(uint16_t *)(buf + pe_off + 6);
                uint16_t opt_sz = *(uint16_t *)(buf + pe_off + 20);
                uint32_t sec_off = pe_off + 24 + opt_sz;
                for (uint16_t s = 0; s < nsec && sec_off + 40 <= n; s++, sec_off += 40) {
                    if (memcmp(buf + sec_off, ".tramp", 6) == 0) *has_tramp = true;
                }
            }
        }
        first_chunk = false;
        size_t off = 0;
        uint32_t buf_idx = (uint32_t)((total) & 63u);
        total += n;
        if (buf_idx) {
            size_t take = 64 - buf_idx;
            if (take > n) take = n;
            memcpy(ctx.buffer + buf_idx, buf, take);
            off += take;
            if (buf_idx + take == 64) sha256_transform(&ctx, ctx.buffer);
        }
        while (off + 64 <= n) {
            sha256_transform(&ctx, buf + off);
            off += 64;
        }
        if (off < n) memcpy(ctx.buffer, buf + off, n - off);
    }
    fclose(fp);
    if (out_size) *out_size = total;

    ctx.bitcount = total * 8;
    uint32_t idx = (uint32_t)(total & 63u);
    ctx.buffer[idx++] = 0x80;
    if (idx > 56) {
        memset(ctx.buffer + idx, 0, 64 - idx);
        sha256_transform(&ctx, ctx.buffer);
        idx = 0;
    }
    memset(ctx.buffer + idx, 0, 56 - idx);
    for (int i = 0; i < 8; i++) ctx.buffer[56 + i] = (uint8_t)(ctx.bitcount >> (56 - i * 8));
    sha256_transform(&ctx, ctx.buffer);

    for (int i = 0; i < 8; i++) sprintf(out_hex + i * 8, "%08x", ctx.state[i]);
    out_hex[64] = 0;
    return true;
}

// ============================================================================
// 4. Core Write-Through File Output & Log Rotation
// ============================================================================
static void raw_write_log(const char *buf, size_t len, bool is_urgent) {
    if (!len) return;
#ifdef _WIN32
    if (g_log_handle != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_log_handle, buf, (DWORD)len, &written, NULL);
        if (is_urgent) FlushFileBuffers(g_log_handle);
    }
    if (g_diag_console == 1) {
        HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
        if (hErr && hErr != INVALID_HANDLE_VALUE && hErr != g_log_handle) {
            DWORD written = 0;
            WriteFile(hErr, buf, (DWORD)len, &written, NULL);
        }
    }
#else
    if (g_log_fd >= 0) {
        ssize_t r = write(g_log_fd, buf, len);
        (void)r;
        if (is_urgent) fsync(g_log_fd);
    }
    if (g_diag_console == 1) {
        ssize_t r = write(STDERR_FILENO, buf, len);
        (void)r;
    }
#endif
}

int diag_parse_level(const char *str) {
    if (!str || !*str) return -1;
    char s[32];
    size_t i = 0;
    while (str[i] && i < sizeof(s) - 1) { s[i] = (char)toupper((unsigned char)str[i]); i++; }
    s[i] = 0;
    if (!strcmp(s, "TRACE") || !strcmp(s, "0")) return DIAG_TRACE;
    if (!strcmp(s, "DEBUG") || !strcmp(s, "1")) return DIAG_DEBUG;
    if (!strcmp(s, "INFO")  || !strcmp(s, "2")) return DIAG_INFO;
    if (!strcmp(s, "WARN")  || !strcmp(s, "WARNING") || !strcmp(s, "3")) return DIAG_WARN;
    if (!strcmp(s, "ERROR") || !strcmp(s, "ERR") || !strcmp(s, "4")) return DIAG_ERROR;
    if (!strcmp(s, "FATAL") || !strcmp(s, "5")) return DIAG_FATAL;
    return -1;
}

const char *diag_level_name(int level) {
    switch (level) {
    case DIAG_TRACE: return "TRACE";
    case DIAG_DEBUG: return "DEBUG";
    case DIAG_INFO:  return "INFO ";
    case DIAG_WARN:  return "WARN ";
    case DIAG_ERROR: return "ERROR";
    case DIAG_FATAL: return "FATAL";
    default:         return "INFO ";
    }
}

static void rotate_logs_in_dir(const char *dir, int keep) {
    if (keep < 1) keep = 1;
    if (keep > 50) keep = 50;
    char old_p[1300], new_p[1300];
    // Remove oldest log.(keep-1).txt
    if (keep > 1) {
        snprintf(old_p, sizeof old_p, "%s/log.%d.txt", dir, keep - 1);
        remove(old_p);
        for (int i = keep - 2; i >= 1; i--) {
            snprintf(old_p, sizeof old_p, "%s/log.%d.txt", dir, i);
            snprintf(new_p, sizeof new_p, "%s/log.%d.txt", dir, i + 1);
            rename(old_p, new_p);
        }
        snprintf(old_p, sizeof old_p, "%s/log.txt", dir);
        snprintf(new_p, sizeof new_p, "%s/log.1.txt", dir);
        rename(old_p, new_p);
    }
}

static bool try_open_log_in_dir(const char *dir, int keep, char *err_reason, size_t err_sz) {
    if (getenv("MHGU_TEST_READONLY_LOG") && atoi(getenv("MHGU_TEST_READONLY_LOG")) == 1 && !g_used_fallback_dir) {
        snprintf(err_reason, err_sz, "simulated read-only directory via MHGU_TEST_READONLY_LOG=1");
        return false;
    }
    // Probe write capability before rotating so we don't fail mid-rotation
    char probe_path[1300];
    snprintf(probe_path, sizeof probe_path, "%s/.mhgu_write_probe.tmp", dir);
#ifdef _WIN32
    HANDLE hp = CreateFileA(probe_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (hp == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        snprintf(err_reason, err_sz, "directory '%s' is not writable (Win32 error %lu)", dir, (unsigned long)err);
        return false;
    }
    CloseHandle(hp);
#else
    FILE *fp = fopen(probe_path, "wb");
    if (!fp) {
        snprintf(err_reason, err_sz, "directory '%s' is not writable (errno %d: %s)", dir, errno, strerror(errno));
        return false;
    }
    fclose(fp);
    unlink(probe_path);
#endif

    rotate_logs_in_dir(dir, keep);
    snprintf(g_log_dir, sizeof g_log_dir, "%s", dir);
    snprintf(g_log_file_path, sizeof g_log_file_path, "%s/log.txt", dir);
    snprintf(g_crash_dir, sizeof g_crash_dir, "%s/crash", dir);

#ifdef _WIN32
    g_log_handle = CreateFileA(g_log_file_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (g_log_handle == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        snprintf(err_reason, err_sz, "CreateFileA('%s') failed (Win32 error %lu)", g_log_file_path, (unsigned long)err);
        return false;
    }
    return true;
#else
    g_log_fd = open(g_log_file_path, O_CREAT | O_TRUNC | O_WRONLY | O_SYNC, 0644);
    if (g_log_fd < 0) {
        snprintf(err_reason, err_sz, "open('%s') failed (errno %d: %s)", g_log_file_path, errno, strerror(errno));
        return false;
    }
    return true;
#endif
}

static bool get_localappdata_fallback_dir(char *buf, size_t size) {
#ifdef _WIN32
    const char *env_lad = getenv("LOCALAPPDATA");
    if (env_lad && *env_lad) {
        snprintf(buf, size, "%s\\MHGURecomp", env_lad);
        CreateDirectoryA(buf, NULL);
        return true;
    }
    PWSTR base = NULL;
    if (SHGetKnownFolderPath(&FOLDERID_LocalAppData, KF_FLAG_CREATE, NULL, &base) == S_OK && base) {
        wchar_t wdir[MAX_PATH];
        _snwprintf(wdir, MAX_PATH, L"%ls\\MHGURecomp", base);
        CoTaskMemFree(base);
        CreateDirectoryW(wdir, NULL);
        WideCharToMultiByte(CP_ACP, 0, wdir, -1, buf, (int)size, NULL, NULL);
        return true;
    }
    if (base) CoTaskMemFree(base);
#else
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(buf, size, "%s/.local/share/MHGURecomp", home);
        mkdir(buf, 0755);
        return true;
    }
#endif
    return false;
}

__attribute__((constructor(101))) void diag_init_early(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&g_diag_initialized, &expected, true)) return;

#ifdef _WIN32
    if (!g_log_cs_init) {
        InitializeCriticalSection(&g_log_cs);
        g_log_cs_init = true;
    }
#endif
    g_start_ns = plat_now_ns();

    // Check env overrides before opening log
    const char *env_lvl = getenv("MHGU_LOG_LEVEL");
    if (env_lvl) {
        int l = diag_parse_level(env_lvl);
        if (l >= 0) g_diag_level = l;
    }
    const char *env_keep = getenv("MHGU_LOG_KEEP");
    if (env_keep && atoi(env_keep) > 0) g_diag_keep_files = atoi(env_keep);

    const char *env_con = getenv("MHGU_LOG_CONSOLE");
    if (env_con) g_diag_console = atoi(env_con) != 0 ? 1 : 0;
    else {
#ifdef _WIN32
        DWORD ids[2];
        g_diag_console = (GetConsoleProcessList(ids, 2) > 1) ? 1 : 0;
#else
        g_diag_console = isatty(STDERR_FILENO) ? 1 : 0;
#endif
    }

    const char *env_wd = getenv("MHGU_WATCHDOG_SEC");
    if (env_wd) g_diag_watchdog_sec = atoi(env_wd);

    const char *env_val = getenv("MHGU_VK_VALIDATION");
    if (env_val) g_diag_vk_validation = atoi(env_val) != 0 ? 1 : 0;

    // Determine primary log directory
    char primary_dir[1024] = ".";
    const char *env_log_dir = getenv("MHGU_LOG_DIR");
    const char *env_home = getenv("MHGU_HOME");
    if (env_log_dir && *env_log_dir) {
        snprintf(primary_dir, sizeof primary_dir, "%s", env_log_dir);
#ifdef _WIN32
        CreateDirectoryA(primary_dir, NULL);
#else
        mkdir(primary_dir, 0755);
#endif
    } else if (env_home && *env_home) {
        snprintf(primary_dir, sizeof primary_dir, "%s", env_home);
#ifdef _WIN32
        CreateDirectoryA(primary_dir, NULL);
#endif
    } else {
        char exe[1024];
        if (plat_exe_path(exe, sizeof exe)) {
            char *s = strrchr(exe, '\\'), *s2 = strrchr(exe, '/');
            if (s2 > s) s = s2;
            if (s) { *s = 0; snprintf(primary_dir, sizeof primary_dir, "%s", exe); }
        }
    }

    if (!try_open_log_in_dir(primary_dir, g_diag_keep_files, g_fallback_reason, sizeof g_fallback_reason)) {
        char fallback_dir[1024];
        g_used_fallback_dir = true;
        if (get_localappdata_fallback_dir(fallback_dir, sizeof fallback_dir)) {
            char dummy[256];
            try_open_log_in_dir(fallback_dir, g_diag_keep_files, dummy, sizeof dummy);
        }
    }

    time_t now_t = time(NULL);
    struct tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &now_t);
#else
    localtime_r(&now_t, &tm_buf);
#endif
    char ts_str[64];
    strftime(ts_str, sizeof ts_str, "%Y-%m-%d %H:%M:%S %z", &tm_buf);

    diag_thread_register("main", 0, 0);

    // Line 1: Final resolved log path, rotation state, and fallback status
    diag_log(DIAG_INFO, "logger", "================================================================================\n");
    diag_log(DIAG_INFO, "logger", "MHGURecomp Diagnostic Logger initialized at %s | Final log path: %s (keep=%d, write-through=ON)\n",
             ts_str, g_log_file_path, g_diag_keep_files);
    if (g_used_fallback_dir) {
        diag_log(DIAG_WARN, "logger", "Primary folder not writable (%s) -> fell back to %%LOCALAPPDATA%%\\MHGURecomp: %s\n",
                 g_fallback_reason, g_log_file_path);
    }
}

void diag_configure(int level, int console_mode, int keep_files, int watchdog_sec, int vk_validation, int minidump) {
    // Environment variables take precedence over mhgurecomp.ini
    if (!getenv("MHGU_LOG_LEVEL") && level >= DIAG_TRACE && level <= DIAG_FATAL) g_diag_level = level;
    if (!getenv("MHGU_LOG_CONSOLE") && console_mode >= 0) g_diag_console = console_mode;
    if (!getenv("MHGU_LOG_KEEP") && keep_files >= 1 && keep_files <= 50) g_diag_keep_files = keep_files;
    if (!getenv("MHGU_WATCHDOG_SEC") && watchdog_sec >= 0) g_diag_watchdog_sec = watchdog_sec;
    if (!getenv("MHGU_VK_VALIDATION") && vk_validation >= 0) g_diag_vk_validation = vk_validation;
    if (minidump >= 0) g_diag_minidump = minidump;

    diag_log(DIAG_INFO, "logger", "Logger config active: level=%s console=%s keep=%d watchdog=%ds vk_validation=%s minidump=%s\n",
             diag_level_name(g_diag_level), g_diag_console ? "ON" : "OFF", g_diag_keep_files,
             g_diag_watchdog_sec, g_diag_vk_validation ? "ON" : "OFF", g_diag_minidump ? "ON" : "OFF");
}

void diag_trace_ring_push(const char *tag, const char *fmt, ...) {
    uint64_t slot = atomic_fetch_add(&g_trace_ring_seq, 1) % TRACE_RING_SLOTS;
    double ms = (double)(plat_now_ns() - g_start_ns) / 1e6;
    uint32_t tid = get_os_tid();
    const char *tname = diag_thread_name();
    char msg[180];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    size_t ml = strlen(msg);
    while (ml > 0 && (msg[ml - 1] == '\n' || msg[ml - 1] == '\r')) msg[--ml] = 0;
    snprintf(g_trace_ring[slot], TRACE_RING_LINE, "[+%010.3f ms] [T:%04x/%-12.12s] [TRACE] [%-8.8s] %s\n",
             ms, tid, tname, tag ? tag : "trace", msg);
}

void diag_dump_trace_ring(void) {
    uint64_t total = atomic_load(&g_trace_ring_seq);
    char hdr[160];
    int hn = snprintf(hdr, sizeof hdr,
                      "\n--- BEGIN IN-MEMORY TRACE RING BUFFER DUMP (%llu total trace events, last %d slots) ---\n",
                      (unsigned long long)total, TRACE_RING_SLOTS);
    if (hn > 0) raw_write_log(hdr, (size_t)hn, true);

    uint64_t count = total < TRACE_RING_SLOTS ? total : TRACE_RING_SLOTS;
    uint64_t start = total - count;
    for (uint64_t i = 0; i < count; i++) {
        const char *line = g_trace_ring[(start + i) % TRACE_RING_SLOTS];
        if (line[0]) raw_write_log(line, strlen(line), false);
    }
    const char *ftr = "--- END IN-MEMORY TRACE RING BUFFER DUMP ---\n\n";
    raw_write_log(ftr, strlen(ftr), true);
}

void diag_vlog(DiagLevel level, const char *tag, const char *fmt, va_list ap) {
    if (!atomic_load(&g_diag_initialized)) diag_init_early();

    char body[1536];
    vsnprintf(body, sizeof body, fmt, ap);
    size_t blen = strlen(body);
    while (blen > 0 && (body[blen - 1] == '\n' || body[blen - 1] == '\r')) body[--blen] = 0;

    double ms = (double)(plat_now_ns() - g_start_ns) / 1e6;
    uint32_t tid = get_os_tid();
    const char *tname = diag_thread_name();
    const char *lname = diag_level_name(level);
    const char *mtag = (tag && *tag) ? tag : "core";

    // Always push into the in-memory ring buffer so crash dumps have complete context
    uint64_t slot = atomic_fetch_add(&g_trace_ring_seq, 1) % TRACE_RING_SLOTS;
    snprintf(g_trace_ring[slot], TRACE_RING_LINE, "[+%010.3f ms] [T:%04x/%-12.12s] [%s] [%-8.8s] %.160s\n",
             ms, tid, tname, lname, mtag, body);

    if ((int)level < g_diag_level) return;

    char line[1792];
    int n = snprintf(line, sizeof line, "[+%010.3f ms] [T:%04x/%-12.12s] [%s] [%-8.8s] %s\n",
                     ms, tid, tname, lname, mtag, body);
    if (n <= 0) return;
    if ((size_t)n >= sizeof line) n = (int)(sizeof line - 1);

#ifdef _WIN32
    if (g_log_cs_init) EnterCriticalSection(&g_log_cs);
    raw_write_log(line, (size_t)n, level >= DIAG_WARN);
    if (g_log_cs_init) LeaveCriticalSection(&g_log_cs);
#else
    pthread_mutex_lock(&g_log_mu);
    raw_write_log(line, (size_t)n, level >= DIAG_WARN);
    pthread_mutex_unlock(&g_log_mu);
#endif
}

void diag_log(DiagLevel level, const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    diag_vlog(level, tag, fmt, ap);
    va_end(ap);
}

void diag_log_legacy(const char *fmt, ...) {
    char buf[1536];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

    char tag[32] = "runtime";
    const char *msg = buf;
    if (buf[0] == '[') {
        const char *close = strchr(buf + 1, ']');
        if (close && (close - (buf + 1)) < (ptrdiff_t)(sizeof tag - 1)) {
            size_t tlen = (size_t)(close - (buf + 1));
            memcpy(tag, buf + 1, tlen);
            tag[tlen] = 0;
            msg = close + 1;
            while (*msg == ' ') msg++;
        }
    }
    DiagLevel lvl = DIAG_INFO;
    if (strstr(msg, "warning:") || strstr(msg, "unavailable") || strstr(msg, "did not finish")) lvl = DIAG_WARN;
    else if (strstr(msg, "failed") || strstr(msg, "error") || strstr(msg, "FATAL")) lvl = DIAG_ERROR;
    diag_log(lvl, tag, "%s", msg);
}

// ============================================================================
// 5. System Info, CPUID, XGETBV, Binary SHA-256, and INI Dump
// ============================================================================
static void query_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t regs[4]) {
#ifdef _WIN32
    int info[4] = {0};
    __cpuidex(info, (int)leaf, (int)subleaf);
    for (int i = 0; i < 4; i++) regs[i] = (uint32_t)info[i];
#elif defined(__x86_64__) || defined(__i386__)
    __cpuid_count(leaf, subleaf, regs[0], regs[1], regs[2], regs[3]);
#else
    memset(regs, 0, 16);
#endif
}

static uint64_t query_xgetbv0(void) {
#if defined(_WIN32) || defined(__x86_64__)
    uint32_t eax = 0, edx = 0;
    __asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(eax), "=d"(edx) : "c"(0));
    return ((uint64_t)edx << 32) | eax;
#else
    return 0;
#endif
}

void diag_log_system_info(void) {
    diag_log(DIAG_INFO, "sysinfo", "--- SYSTEM & HARDWARE DIAGNOSTICS ---\n");
    diag_log(DIAG_INFO, "sysinfo", "Build Version : MHGURecomp 0.10.1 Non-AVX2 (%s)\n", MHGU_GIT_COMMIT);
    diag_log(DIAG_INFO, "sysinfo", "Build Date    : %s %s | Compiler: %s\n", __DATE__, __TIME__, __VERSION__);
    diag_log(DIAG_INFO, "sysinfo", "Compile Flags : %s\n", MHGU_BUILD_FLAGS);

#ifdef _WIN32
    typedef LONG(WINAPI *PFN_RtlGetVersion)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW osvi = {0};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll) {
        PFN_RtlGetVersion pRtlGetVersion = (PFN_RtlGetVersion)(void *)GetProcAddress(ntdll, "RtlGetVersion");
        if (pRtlGetVersion) pRtlGetVersion(&osvi);
    }
    SYSTEM_INFO si = {0};
    GetNativeSystemInfo(&si);
    MEMORYSTATUSEX ms = {0};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);

    diag_log(DIAG_INFO, "sysinfo", "OS Version    : Windows %lu.%lu (Build %lu) 64-bit | Logical CPUs: %lu | PageSize: %lu\n",
             (unsigned long)osvi.dwMajorVersion, (unsigned long)osvi.dwMinorVersion, (unsigned long)osvi.dwBuildNumber,
             (unsigned long)si.dwNumberOfProcessors, (unsigned long)si.dwPageSize);
    diag_log(DIAG_INFO, "sysinfo", "System Memory : Total RAM %llu MB | Available RAM %llu MB (%lu%% load) | Virtual Free %llu MB\n",
             (unsigned long long)(ms.ullTotalPhys >> 20), (unsigned long long)(ms.ullAvailPhys >> 20),
             (unsigned long)ms.dwMemoryLoad, (unsigned long long)(ms.ullAvailVirtual >> 20));
#else
    struct utsname un;
    if (uname(&un) == 0) {
        diag_log(DIAG_INFO, "sysinfo", "OS Version    : %s %s (%s) | Cores: %ld\n",
                 un.sysname, un.release, un.machine, sysconf(_SC_NPROCESSORS_ONLN));
    }
#endif

    // CPUID Vendor, Brand, and Feature Flags
    uint32_t r[4] = {0};
    query_cpuid(0, 0, r);
    uint32_t max_leaf = r[0];
    char vendor[13] = {0};
    memcpy(vendor + 0, &r[1], 4);
    memcpy(vendor + 4, &r[3], 4);
    memcpy(vendor + 8, &r[2], 4);

    char brand[49] = {0};
    query_cpuid(0x80000000u, 0, r);
    uint32_t max_ext = r[0];
    if (max_ext >= 0x80000004u) {
        for (int i = 0; i < 3; i++) {
            query_cpuid(0x80000002u + (uint32_t)i, 0, r);
            memcpy(brand + i * 16 + 0,  &r[0], 4);
            memcpy(brand + i * 16 + 4,  &r[1], 4);
            memcpy(brand + i * 16 + 8,  &r[2], 4);
            memcpy(brand + i * 16 + 12, &r[3], 4);
        }
    }
    char *bp = brand;
    while (*bp == ' ') bp++;

    uint32_t f1_ecx = 0, f1_edx = 0, f1_eax = 0;
    if (max_leaf >= 1) {
        query_cpuid(1, 0, r);
        f1_eax = r[0]; f1_ecx = r[2]; f1_edx = r[3];
    }
    uint32_t f7_ebx = 0;
    if (max_leaf >= 7) {
        query_cpuid(7, 0, r);
        f7_ebx = r[1];
    }
    uint32_t ext1_ecx = 0;
    if (max_ext >= 0x80000001u) {
        query_cpuid(0x80000001u, 0, r);
        ext1_ecx = r[2];
    }

    bool has_sse42   = (f1_ecx & (1u << 20)) != 0;
    bool has_popcnt  = (f1_ecx & (1u << 23)) != 0;
    bool has_movbe   = (f1_ecx & (1u << 22)) != 0;
    bool has_aes     = (f1_ecx & (1u << 25)) != 0;
    bool has_xsave   = (f1_ecx & (1u << 26)) != 0;
    bool has_osxsave = (f1_ecx & (1u << 27)) != 0;
    bool has_avx     = (f1_ecx & (1u << 28)) != 0;
    bool has_f16c    = (f1_ecx & (1u << 29)) != 0;
    bool has_fma3    = (f1_ecx & (1u << 12)) != 0;
    bool has_bmi1    = (f7_ebx & (1u << 3))  != 0;
    bool has_avx2    = (f7_ebx & (1u << 5))  != 0;
    bool has_bmi2    = (f7_ebx & (1u << 8))  != 0;
    bool has_fma4    = (ext1_ecx & (1u << 16)) != 0;
    bool has_tbm     = (ext1_ecx & (1u << 21)) != 0;

    uint64_t xcr0 = has_osxsave ? query_xgetbv0() : 0;
    bool os_xmm = (xcr0 & 0x2) != 0;
    bool os_ymm = (xcr0 & 0x6) == 0x6;

    diag_log(DIAG_INFO, "cpuid", "CPU Vendor    : %s | Brand: %s (sig=0x%08x)\n", vendor, *bp ? bp : "Unknown", f1_eax);
    diag_log(DIAG_INFO, "cpuid", "CPU Features  : SSE4.2=%d POPCNT=%d AVX=%d AVX2=%d FMA3=%d FMA4=%d BMI1=%d BMI2=%d F16C=%d MOVBE=%d TBM=%d\n",
             has_sse42, has_popcnt, has_avx, has_avx2, has_fma3, has_fma4, has_bmi1, has_bmi2, has_f16c, has_movbe, has_tbm);
    diag_log(DIAG_INFO, "cpuid", "OS XSAVE/YMM  : XSAVE=%d OSXSAVE=%d XCR0=0x%llx (XMM_STATE=%s, YMM_STATE=%s)\n",
             has_xsave, has_osxsave, (unsigned long long)xcr0, os_xmm ? "YES" : "NO", os_ymm ? "YES" : "NO");
    if (!has_avx2 || !has_bmi2 || !has_movbe) {
        diag_log(DIAG_INFO, "cpuid", "Non-AVX2 CPU detected (AVX2=%d BMI2=%d MOVBE=%d): AOT .tramp + VEH fallback active\n",
                 has_avx2, has_bmi2, has_movbe);
    }
}

void diag_log_binary_hashes(void) {
    char exe_path[1024] = "mhgurecomp.exe";
    if (!plat_exe_path(exe_path, sizeof exe_path)) snprintf(exe_path, sizeof exe_path, "mhgurecomp.exe");

    char hex[65];
    uint64_t fsize = 0;
    if (sha256_file(exe_path, hex, &fsize, NULL)) {
        diag_log(DIAG_INFO, "sha256", "Executable    : %s (%llu bytes) SHA-256=%s\n",
                 exe_path, (unsigned long long)fsize, hex);
    } else {
        diag_log(DIAG_WARN, "sha256", "Executable    : %s (could not read for SHA-256)\n", exe_path);
    }

    char dll_path[1100];
    const char *env_dll = getenv("MHGU_GAME_LIB");
    if (env_dll && *env_dll) {
        snprintf(dll_path, sizeof dll_path, "%s", env_dll);
    } else {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s", exe_path);
        char *s = strrchr(dir, '\\'), *s2 = strrchr(dir, '/');
        if (s2 > s) s = s2;
        if (s) { s[1] = 0; snprintf(dll_path, sizeof dll_path, "%smhgugame.dll", dir); }
        else snprintf(dll_path, sizeof dll_path, "mhgugame.dll");
    }

    bool has_tramp = false;
    if (sha256_file(dll_path, hex, &fsize, &has_tramp)) {
        diag_log(DIAG_INFO, "sha256", "Game Library  : %s (%llu bytes) SHA-256=%s | AOT .tramp section=%s\n",
                 dll_path, (unsigned long long)fsize, hex, has_tramp ? "PRESENT" : "NONE (unpatched DLL)");
    } else {
        diag_log(DIAG_WARN, "sha256", "Game Library  : %s (not found or unreadable at startup)\n", dll_path);
    }
}

void diag_log_paths_and_files(const char *game_dir, const char *save_dir) {
    char exe[1024] = "", cwd[1024] = "";
    plat_exe_path(exe, sizeof exe);
#ifdef _WIN32
    GetCurrentDirectoryA(sizeof cwd, cwd);
#else
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
#endif
    const char *ini = getenv("MHGU_CONFIG") ? getenv("MHGU_CONFIG") : "mhgurecomp.ini";
    diag_log(DIAG_INFO, "paths", "Path [exe]    : %s\n", exe);
    diag_log(DIAG_INFO, "paths", "Path [cwd]    : %s\n", cwd);
    diag_log(DIAG_INFO, "paths", "Path [log]    : %s\n", g_log_file_path);
    diag_log(DIAG_INFO, "paths", "Path [crash]  : %s\n", g_crash_dir);
    diag_log(DIAG_INFO, "paths", "Path [ini]    : %s\n", ini);
    diag_log(DIAG_INFO, "paths", "Path [data]   : %s\n", game_dir ? game_dir : "(null)");
    diag_log(DIAG_INFO, "paths", "Path [save]   : %s\n", save_dir ? save_dir : "(null)");
    diag_log(DIAG_INFO, "paths", "Path [mods]   : %s/mods\n", cwd);

    if (game_dir && *game_dir) {
        char p_main[1200], p_sdk[1200], p_npdm[1200], p_romfs[1200];
        snprintf(p_main,  sizeof p_main,  "%s/exefs/main", game_dir);
        snprintf(p_sdk,   sizeof p_sdk,   "%s/exefs/sdk", game_dir);
        snprintf(p_npdm,  sizeof p_npdm,  "%s/exefs/main.npdm", game_dir);
        snprintf(p_romfs, sizeof p_romfs, "%s/romfs", game_dir);
        struct stat st_main = {0}, st_sdk = {0}, st_npdm = {0}, st_romfs = {0};
        bool ok_main  = (stat(p_main, &st_main) == 0);
        bool ok_sdk   = (stat(p_sdk, &st_sdk) == 0);
        bool ok_npdm  = (stat(p_npdm, &st_npdm) == 0);
        bool ok_romfs = (stat(p_romfs, &st_romfs) == 0 && S_ISDIR(st_romfs.st_mode));
        diag_log(ok_main && ok_sdk && ok_romfs ? DIAG_INFO : DIAG_WARN, "files",
                 "Game Files    : exefs/main=%s (%lld B) | exefs/sdk=%s (%lld B) | main.npdm=%s | romfs/=%s\n",
                 ok_main ? "FOUND" : "MISSING", (long long)(ok_main ? st_main.st_size : 0),
                 ok_sdk ? "FOUND" : "MISSING", (long long)(ok_sdk ? st_sdk.st_size : 0),
                 ok_npdm ? "FOUND" : "MISSING", ok_romfs ? "FOUND" : "MISSING");
    }
}

void diag_log_effective_ini(const char *ini_path) {
    const char *p = (ini_path && *ini_path) ? ini_path : "mhgurecomp.ini";
    FILE *f = fopen(p, "r");
    if (!f) {
        diag_log(DIAG_INFO, "ini", "Config file '%s' not on disk (using built-in defaults)\n", p);
        return;
    }
    diag_log(DIAG_INFO, "ini", "--- BEGIN EFFECTIVE %s ---\n", p);
    char line[512];
    int lno = 0;
    while (fgets(line, sizeof line, f)) {
        lno++;
        char *s = line;
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s || *s == ';' || *s == '#') continue;
        size_t len = strlen(s);
        while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) s[--len] = 0;
        diag_log(DIAG_INFO, "ini", "  L%03d: %s\n", lno, s);
    }
    fclose(f);
    diag_log(DIAG_INFO, "ini", "--- END EFFECTIVE %s ---\n", p);
}

// ============================================================================
// 6. Startup Phase Tracking
// ============================================================================
#define MAX_PHASE_DEPTH 8
typedef struct {
    char name[64];
    uint64_t start_ns;
} PhaseEntry;

static PhaseEntry g_phase_stack[MAX_PHASE_DEPTH];
static int g_phase_depth = 0;
static char g_last_phase[64] = "early_boot";

const char *diag_current_phase(void) {
    if (g_phase_depth > 0) return g_phase_stack[g_phase_depth - 1].name;
    return g_last_phase;
}

void diag_phase_begin(const char *phase_name) {
    diag_watchdog_heartbeat();
    uint64_t now = plat_now_ns();
    snprintf(g_last_phase, sizeof g_last_phase, "%s", phase_name ? phase_name : "unknown");
    if (g_phase_depth < MAX_PHASE_DEPTH) {
        snprintf(g_phase_stack[g_phase_depth].name, sizeof g_phase_stack[0].name, "%s", g_last_phase);
        g_phase_stack[g_phase_depth].start_ns = now;
        g_phase_depth++;
    }
    diag_log(DIAG_INFO, "phase", ">>> PHASE BEGIN: %s\n", g_last_phase);
}

void diag_phase_end(const char *phase_name) {
    diag_watchdog_heartbeat();
    uint64_t now = plat_now_ns();
    double dur_ms = 0.0;
    if (g_phase_depth > 0) {
        g_phase_depth--;
        dur_ms = (double)(now - g_phase_stack[g_phase_depth].start_ns) / 1e6;
    }
    diag_log(DIAG_INFO, "phase", "<<< PHASE END  : %s (completed in %.2f ms)\n",
             phase_name ? phase_name : g_last_phase, dur_ms);
}

void diag_phase_fail(const char *phase_name, int err_code, const char *fmt, ...) {
    uint64_t now = plat_now_ns();
    double dur_ms = 0.0;
    if (g_phase_depth > 0) {
        dur_ms = (double)(now - g_phase_stack[g_phase_depth - 1].start_ns) / 1e6;
    }
    char detail[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);

#ifdef _WIN32
    DWORD last_err = GetLastError();
    char win_msg[256] = "none";
    if (last_err) {
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, last_err,
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), win_msg, sizeof win_msg, NULL);
        size_t l = strlen(win_msg);
        while (l > 0 && (win_msg[l - 1] == '\n' || win_msg[l - 1] == '\r')) win_msg[--l] = 0;
    }
    diag_log(DIAG_FATAL, "phase", "!!! PHASE FAILED: %s (after %.2f ms) | code=%d (0x%x) | GetLastError=%lu (%s) | %s\n",
             phase_name ? phase_name : diag_current_phase(), dur_ms, err_code, (unsigned)err_code,
             (unsigned long)last_err, win_msg, detail);
#else
    diag_log(DIAG_FATAL, "phase", "!!! PHASE FAILED: %s (after %.2f ms) | code=%d (0x%x) | errno=%d (%s) | %s\n",
             phase_name ? phase_name : diag_current_phase(), dur_ms, err_code, (unsigned)err_code,
             errno, strerror(errno), detail);
#endif
}

// ============================================================================
// 7. Vulkan Result Strings & Device Lost Diagnostics
// ============================================================================
const char *diag_vk_result_str(int r) {
    switch (r) {
    case 0:           return "VK_SUCCESS";
    case 1:           return "VK_NOT_READY";
    case 2:           return "VK_TIMEOUT";
    case 3:           return "VK_EVENT_SET";
    case 4:           return "VK_EVENT_RESET";
    case 5:           return "VK_INCOMPLETE";
    case -1:          return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case -2:          return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case -3:          return "VK_ERROR_INITIALIZATION_FAILED";
    case -4:          return "VK_ERROR_DEVICE_LOST";
    case -5:          return "VK_ERROR_MEMORY_MAP_FAILED";
    case -6:          return "VK_ERROR_LAYER_NOT_PRESENT";
    case -7:          return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case -8:          return "VK_ERROR_FEATURE_NOT_PRESENT";
    case -9:          return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case -10:         return "VK_ERROR_TOO_MANY_OBJECTS";
    case -11:         return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case -12:         return "VK_ERROR_FRAGMENTED_POOL";
    case -13:         return "VK_ERROR_UNKNOWN";
    case -1000069000: return "VK_ERROR_OUT_OF_POOL_MEMORY";
    case -1000072003: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
    case -1000161000: return "VK_ERROR_FRAGMENTATION";
    case -1000257000: return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
    case -1000000000: return "VK_ERROR_SURFACE_LOST_KHR";
    case -1000000001: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
    case 1000001003:  return "VK_SUBOPTIMAL_KHR";
    case -1000001004: return "VK_ERROR_OUT_OF_DATE_KHR";
    default:          return "VK_RESULT_UNKNOWN";
    }
}

void diag_log_vk_create(const char *api_name, int vk_result, const char *detail) {
    DiagLevel lvl = (vk_result == 0) ? DIAG_INFO : DIAG_ERROR;
    diag_log(lvl, "vk", "%s -> %s (%d)%s%s\n",
             api_name, diag_vk_result_str(vk_result), vk_result,
             (detail && *detail) ? " | " : "", (detail && *detail) ? detail : "");
}

void diag_log_vk_device_lost(const char *api_name, int vk_result, uint64_t frame_no, uint64_t submit_seq, uint64_t completed_seq) {
    diag_log(DIAG_ERROR, "vk", "================================================================================\n");
    diag_log(DIAG_ERROR, "vk", "VULKAN GPU FAULT / DEVICE LOST DETECTED in %s!\n", api_name);
    diag_log(DIAG_ERROR, "vk", "  VkResult       : %s (%d)\n", diag_vk_result_str(vk_result), vk_result);
    diag_log(DIAG_ERROR, "vk", "  Frame Number   : %llu\n", (unsigned long long)frame_no);
    diag_log(DIAG_ERROR, "vk", "  Timeline Seq   : submitted=%llu, last_completed=%llu (in_flight=%lld)\n",
             (unsigned long long)submit_seq, (unsigned long long)completed_seq,
             (long long)(submit_seq - completed_seq));
    diag_log(DIAG_ERROR, "vk", "  Probable Causes: (1) Windows GPU TDR timeout (>2s shader/draw stall),\n");
    diag_log(DIAG_ERROR, "vk", "                   (2) Out-of-bounds buffer/descriptor read in guest shader,\n");
    diag_log(DIAG_ERROR, "vk", "                   (3) GPU driver reset or VRAM exhaustion.\n");
    diag_log(DIAG_ERROR, "vk", "================================================================================\n");
}

// ============================================================================
// 8. Watchdog Thread (Detects Main/Game Loop Freezes & Dumps All Thread Stacks)
// ============================================================================
static _Atomic uint64_t g_watchdog_last_ns = 0;
static _Atomic bool g_watchdog_running = false;

void diag_watchdog_heartbeat(void) {
    atomic_store(&g_watchdog_last_ns, plat_now_ns());
}

#ifdef _WIN32
static void dump_thread_stack_win32(HANDLE hThread, uint32_t os_tid, const char *tname, uint32_t g_handle, uint32_t g_entry) {
    if (!hThread) return;
    DWORD self_tid = GetCurrentThreadId();
    if (os_tid == self_tid) return;

    if (SuspendThread(hThread) == (DWORD)-1) return;
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(hThread, &ctx)) {
        uint64_t off = 0;
        const char *fn = rt_host_fn_name(ctx.Rip, &off);
        if (!fn) fn = plat_symbol(ctx.Rip, &off);
        diag_log(DIAG_WARN, "watchdog", "  Thread [T:%04x/%-14s] guest_h=0x%x entry=0x%08x | RIP=0x%016llx (%s+0x%llx) RSP=0x%016llx\n",
                 os_tid, tname, g_handle, g_entry, (unsigned long long)ctx.Rip, fn ? fn : "exe",
                 (unsigned long long)off, (unsigned long long)ctx.Rsp);

        STACKFRAME64 sf;
        memset(&sf, 0, sizeof sf);
        sf.AddrPC.Offset    = ctx.Rip;
        sf.AddrPC.Mode      = AddrModeFlat;
        sf.AddrFrame.Offset = ctx.Rbp;
        sf.AddrFrame.Mode   = AddrModeFlat;
        sf.AddrStack.Offset = ctx.Rsp;
        sf.AddrStack.Mode   = AddrModeFlat;

        HANDLE hProc = GetCurrentProcess();
        for (int frame = 0; frame < 12; frame++) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hProc, hThread, &sf, &ctx, NULL,
                             SymFunctionTableAccess64, SymGetModuleBase64, NULL))
                break;
            if (!sf.AddrPC.Offset) break;
            uint64_t f_off = 0;
            const char *f_sym = rt_host_fn_name(sf.AddrPC.Offset, &f_off);
            if (!f_sym) f_sym = plat_symbol(sf.AddrPC.Offset, &f_off);
            diag_log(DIAG_WARN, "watchdog", "    #%-2d 0x%016llx %s+0x%llx\n",
                     frame, (unsigned long long)sf.AddrPC.Offset, f_sym ? f_sym : "mhgurecomp.exe",
                     (unsigned long long)f_off);
        }
    }
    ResumeThread(hThread);
}
#endif

static void *watchdog_thread_fn(void *arg) {
    (void)arg;
    diag_thread_register("watchdog", 0, 0);
    uint64_t last_frames = g_present_count;
    uint64_t last_progress_ns = plat_now_ns();
    bool alerted_current_stall = false;

    while (atomic_load(&g_watchdog_running)) {
#ifdef _WIN32
        Sleep(1000);
#else
        sleep(1);
#endif
        int timeout_s = g_diag_watchdog_sec;
        if (timeout_s <= 0) continue;

        uint64_t now = plat_now_ns();
        uint64_t cur_frames = g_present_count;
        uint64_t hb = atomic_load(&g_watchdog_last_ns);
        if (cur_frames != last_frames || hb > last_progress_ns) {
            last_frames = cur_frames;
            last_progress_ns = (hb > now) ? now : (cur_frames != last_frames ? now : hb);
            if (cur_frames != last_frames) last_progress_ns = now;
            alerted_current_stall = false;
            continue;
        }

        double stalled_s = (double)(now - last_progress_ns) / 1e9;
        if (stalled_s >= (double)timeout_s && !alerted_current_stall) {
            alerted_current_stall = true;
            diag_log(DIAG_WARN, "watchdog", "================================================================================\n");
            diag_log(DIAG_WARN, "watchdog", "WATCHDOG HANG WARNING: No frame/loop progress for %.1f seconds (threshold=%ds)!\n",
                     stalled_s, timeout_s);
            diag_log(DIAG_WARN, "watchdog", "  Current Phase : %s | Presented Frames: %llu\n",
                     diag_current_phase(), (unsigned long long)cur_frames);
#ifdef _WIN32
            SymInitialize(GetCurrentProcess(), NULL, TRUE);
            for (int i = 0; i < MAX_DIAG_THREADS; i++) {
                if (atomic_load(&g_threads_reg[i].active)) {
                    dump_thread_stack_win32(g_threads_reg[i].win_handle, g_threads_reg[i].os_tid,
                                            g_threads_reg[i].name, g_threads_reg[i].guest_handle,
                                            g_threads_reg[i].guest_entry);
                }
            }
#endif
            diag_log(DIAG_WARN, "watchdog", "================================================================================\n");
        }
    }
    diag_thread_unregister();
    return NULL;
}

void diag_watchdog_start(void) {
    if (g_diag_watchdog_sec <= 0) {
        diag_log(DIAG_INFO, "watchdog", "Watchdog disabled via config (WatchdogTimeout=0)\n");
        return;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&g_watchdog_running, &expected, true)) return;
    diag_watchdog_heartbeat();
    pthread_t th;
    if (pthread_create(&th, NULL, watchdog_thread_fn, NULL) == 0) {
        pthread_detach(th);
    }
}

// ============================================================================
// 9. Crash Handler, DbgHelp StackWalk64, and MiniDumpWriteDump
// ============================================================================
static _Atomic int g_in_crash_handler = 0;

#ifdef _WIN32
static const char *win32_exc_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:             return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION:    return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT:    return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_BREAKPOINT:               return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_SINGLE_STEP:              return "EXCEPTION_SINGLE_STEP";
    case 0x40000015u:                        return "STATUS_FATAL_APP_EXIT";
    case 0xC0000409u:                        return "STATUS_STACK_BUFFER_OVERRUN";
    case 0xC0000374u:                        return "STATUS_HEAP_CORRUPTION";
    case 0xE06D7363u:                        return "CPP_EH_EXCEPTION";
    default:                                 return "UNKNOWN_EXCEPTION";
    }
}

static void resolve_module_and_rva(uint64_t addr, char *mod_name, size_t mod_sz, uint64_t *rva) {
    snprintf(mod_name, mod_sz, "unknown");
    *rva = addr;
    HMODULE hMod = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &hMod) && hMod) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(hMod, path, sizeof path)) {
            const char *base = strrchr(path, '\\');
            snprintf(mod_name, mod_sz, "%s", base ? base + 1 : path);
        }
        *rva = addr - (uint64_t)(uintptr_t)hMod;
    }
}

static void write_minidump_win32(EXCEPTION_POINTERS *ep) {
    if (!g_diag_minidump) return;
    CreateDirectoryA(g_crash_dir, NULL);

    SYSTEMTIME st;
    GetLocalTime(&st);
    DWORD pid = GetCurrentProcessId();
    DWORD tid = GetCurrentThreadId();

    char dmp_path[1300];
    snprintf(dmp_path, sizeof dmp_path, "%s/crash_%04u%02u%02u_%02u%02u%02u_pid%lu_tid%lu.dmp",
             g_crash_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             (unsigned long)pid, (unsigned long)tid);

    HANDLE hFile = CreateFileA(dmp_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        diag_log(DIAG_ERROR, "crash", "Failed to create minidump file '%s' (GetLastError=%lu)\n",
                 dmp_path, (unsigned long)GetLastError());
        return;
    }

    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = tid;
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;

    MINIDUMP_TYPE mtype = (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo |
                                          MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs);
    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), pid, hFile, mtype, ep ? &mei : NULL, NULL, NULL);
    if (!ok) {
        // Fallback to basic MiniDumpNormal if extended flags fail
        ok = MiniDumpWriteDump(GetCurrentProcess(), pid, hFile, MiniDumpNormal, ep ? &mei : NULL, NULL, NULL);
    }
    LARGE_INTEGER dmp_sz = {0};
    GetFileSizeEx(hFile, &dmp_sz);
    FlushFileBuffers(hFile);
    CloseHandle(hFile);

    if (ok) {
        diag_log(DIAG_FATAL, "crash", "Minidump written successfully: %s (%lld bytes)\n",
                 dmp_path, (long long)dmp_sz.QuadPart);
    } else {
        diag_log(DIAG_ERROR, "crash", "MiniDumpWriteDump failed for '%s' (GetLastError=%lu)\n",
                 dmp_path, (unsigned long)GetLastError());
    }
}

static void walk_stack_dbghelp(CONTEXT *ctx_in) {
    HANDLE hProc = GetCurrentProcess();
    HANDLE hThread = GetCurrentThread();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(hProc, NULL, TRUE);

    CONTEXT ctx_local;
    if (ctx_in) {
        ctx_local = *ctx_in;
    } else {
        RtlCaptureContext(&ctx_local);
    }

    STACKFRAME64 sf;
    memset(&sf, 0, sizeof sf);
    sf.AddrPC.Offset    = ctx_local.Rip;
    sf.AddrPC.Mode      = AddrModeFlat;
    sf.AddrFrame.Offset = ctx_local.Rbp;
    sf.AddrFrame.Mode   = AddrModeFlat;
    sf.AddrStack.Offset = ctx_local.Rsp;
    sf.AddrStack.Mode   = AddrModeFlat;

    diag_log(DIAG_FATAL, "crash", "Host Stack Trace (DbgHelp StackWalk64 + Guest Function Table):\n");
    for (int frame = 0; frame < 48; frame++) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hProc, hThread, &sf, &ctx_local, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL))
            break;
        uint64_t pc = sf.AddrPC.Offset;
        if (!pc) break;

        char mod_name[128];
        uint64_t rva = 0;
        resolve_module_and_rva(pc, mod_name, sizeof mod_name, &rva);

        uint64_t g_off = 0;
        const char *g_fn = rt_host_fn_name(pc, &g_off);

        uint8_t sym_buf[sizeof(SYMBOL_INFO) + 256];
        memset(sym_buf, 0, sizeof sym_buf);
        SYMBOL_INFO *sym = (SYMBOL_INFO *)sym_buf;
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 sym_disp = 0;
        bool has_sym = SymFromAddr(hProc, pc, &sym_disp, sym) != FALSE;

        IMAGEHLP_LINE64 line_info;
        memset(&line_info, 0, sizeof line_info);
        line_info.SizeOfStruct = sizeof(line_info);
        DWORD line_disp = 0;
        bool has_line = SymGetLineFromAddr64(hProc, pc, &line_disp, &line_info) != FALSE;

        if (g_fn) {
            diag_log(DIAG_FATAL, "crash", "  #%-2d 0x%016llx %s+0x%llx [%s+0x%llx]\n",
                     frame, (unsigned long long)pc, mod_name, (unsigned long long)rva,
                     g_fn, (unsigned long long)g_off);
        } else if (has_sym && has_line) {
            diag_log(DIAG_FATAL, "crash", "  #%-2d 0x%016llx %s+0x%llx (%s+0x%llx at %s:%lu)\n",
                     frame, (unsigned long long)pc, mod_name, (unsigned long long)rva,
                     sym->Name, (unsigned long long)sym_disp, line_info.FileName, (unsigned long)line_info.LineNumber);
        } else if (has_sym) {
            diag_log(DIAG_FATAL, "crash", "  #%-2d 0x%016llx %s+0x%llx (%s+0x%llx)\n",
                     frame, (unsigned long long)pc, mod_name, (unsigned long long)rva,
                     sym->Name, (unsigned long long)sym_disp);
        } else {
            diag_log(DIAG_FATAL, "crash", "  #%-2d 0x%016llx %s+0x%llx\n",
                     frame, (unsigned long long)pc, mod_name, (unsigned long long)rva);
        }
    }
}
#endif

static void dump_guest_cpu_state(void) {
    Cpu *c = kernel_current_cpu();
    if (!c) {
        diag_log(DIAG_FATAL, "crash", "Guest ARM32 CPU State: None (host-only thread)\n");
        return;
    }
    uint32_t pc_off = 0, lr_off = 0;
    const char *pc_sym = loader_symbol_at(c->pc, &pc_off);
    const char *lr_sym = loader_symbol_at(c->r[14], &lr_off);
    diag_log(DIAG_FATAL, "crash", "Guest ARM32 CPU State (handle=0x%x):\n", kernel_current_thread_handle());
    diag_log(DIAG_FATAL, "crash", "  PC=0x%08x (%s+0x%x) | LR=0x%08x (%s+0x%x) | SP=0x%08x\n",
             c->pc, pc_sym ? pc_sym : "?", pc_off, c->r[14], lr_sym ? lr_sym : "?", lr_off, c->r[13]);
    for (int i = 0; i < 16; i += 4) {
        diag_log(DIAG_FATAL, "crash", "  r%-2d=0x%08x  r%-2d=0x%08x  r%-2d=0x%08x  r%-2d=0x%08x\n",
                 i, c->r[i], i + 1, c->r[i + 1], i + 2, c->r[i + 2], i + 3, c->r[i + 3]);
    }
}

void diag_report_unhandled_exception(void *exception_pointers, const char *source_tag) {
    int prev = atomic_fetch_add(&g_in_crash_handler, 1);
    if (prev > 0) {
        // Re-entrancy guard: already handling a crash
        return;
    }

    const char *src = (source_tag && *source_tag) ? source_tag : "crash";
    diag_log(DIAG_FATAL, src, "================================================================================\n");
    diag_log(DIAG_FATAL, src, "UNHANDLED EXCEPTION / CRASH CAPTURED (%s)\n", src);
    diag_log(DIAG_FATAL, src, "  Startup Phase  : %s\n", diag_current_phase());
    diag_log(DIAG_FATAL, src, "  Presented Frame: %llu\n", (unsigned long long)g_present_count);
    diag_log(DIAG_FATAL, src, "  Thread         : os_tid=%u (0x%x) name='%s' guest_handle=0x%x\n",
             get_os_tid(), get_os_tid(), diag_thread_name(), kernel_current_thread_handle());

#ifdef _WIN32
    EXCEPTION_POINTERS *ep = (EXCEPTION_POINTERS *)exception_pointers;
    if (ep && ep->ExceptionRecord && ep->ContextRecord) {
        EXCEPTION_RECORD *er = ep->ExceptionRecord;
        CONTEXT *ctx = ep->ContextRecord;
        uint64_t rip = ctx->Rip;

        char mod_name[128];
        uint64_t rva = 0;
        resolve_module_and_rva(rip, mod_name, sizeof mod_name, &rva);

        uint64_t g_off = 0;
        const char *g_fn = rt_host_fn_name(rip, &g_off);

        diag_log(DIAG_FATAL, src, "  Exception Code : 0x%08lx (%s)\n",
                 (unsigned long)er->ExceptionCode, win32_exc_name(er->ExceptionCode));
        diag_log(DIAG_FATAL, src, "  Fault Address  : RIP=0x%016llx (%s+0x%llx%s%s)\n",
                 (unsigned long long)rip, mod_name, (unsigned long long)rva,
                 g_fn ? " / " : "", g_fn ? g_fn : "");

        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
            uint64_t acc_type = er->ExceptionInformation[0];
            uint64_t target_va = er->ExceptionInformation[1];
            const char *op_str = (acc_type == 0) ? "READ" : (acc_type == 1) ? "WRITE" : (acc_type == 8) ? "EXEC (DEP)" : "ACCESS";
            if (target_va >= GUEST_BASE && target_va < GUEST_BASE + GUEST_SPACE) {
                diag_log(DIAG_FATAL, src, "  Access Target  : %s at host 0x%016llx (guest 0x%08llx)\n",
                         op_str, (unsigned long long)target_va, (unsigned long long)(target_va - GUEST_BASE));
            } else {
                diag_log(DIAG_FATAL, src, "  Access Target  : %s at host 0x%016llx\n",
                         op_str, (unsigned long long)target_va);
            }
        }

        // Dump 16 instruction bytes at RIP
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((LPCVOID)(uintptr_t)rip, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
            !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            const uint8_t *b = (const uint8_t *)(uintptr_t)rip;
            diag_log(DIAG_FATAL, src,
                     "  Insn Bytes [16]: %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
                     b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                     b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        } else {
            diag_log(DIAG_FATAL, src, "  Insn Bytes [16]: <unmapped memory at RIP 0x%016llx>\n", (unsigned long long)rip);
        }

        // Dump full x64 GPRs, RIP, EFLAGS
        diag_log(DIAG_FATAL, src, "Host x64 General Purpose Registers:\n");
        diag_log(DIAG_FATAL, src, "  RAX=%016llx  RBX=%016llx  RCX=%016llx  RDX=%016llx\n",
                 (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rbx,
                 (unsigned long long)ctx->Rcx, (unsigned long long)ctx->Rdx);
        diag_log(DIAG_FATAL, src, "  RSI=%016llx  RDI=%016llx  RBP=%016llx  RSP=%016llx\n",
                 (unsigned long long)ctx->Rsi, (unsigned long long)ctx->Rdi,
                 (unsigned long long)ctx->Rbp, (unsigned long long)ctx->Rsp);
        diag_log(DIAG_FATAL, src, "  R8 =%016llx  R9 =%016llx  R10=%016llx  R11=%016llx\n",
                 (unsigned long long)ctx->R8,  (unsigned long long)ctx->R9,
                 (unsigned long long)ctx->R10, (unsigned long long)ctx->R11);
        diag_log(DIAG_FATAL, src, "  R12=%016llx  R13=%016llx  R14=%016llx  R15=%016llx\n",
                 (unsigned long long)ctx->R12, (unsigned long long)ctx->R13,
                 (unsigned long long)ctx->R14, (unsigned long long)ctx->R15);
        diag_log(DIAG_FATAL, src, "  RIP=%016llx  EFLAGS=%08lx  MXCSR=%08lx\n",
                 (unsigned long long)ctx->Rip, (unsigned long)ctx->EFlags, (unsigned long)ctx->MxCsr);

        // Dump XMM0..XMM7 (and upper YMM if present)
        const M128A *xmm = &ctx->FltSave.XmmRegisters[0];
        DWORD ymm_len = 0;
        const uint8_t *ymm_hi = NULL;
        if ((ctx->ContextFlags & CONTEXT_AVX) == CONTEXT_AVX) {
            ymm_hi = (const uint8_t *)LocateXStateFeature(ctx, XSTATE_AVX, &ymm_len);
        }
        diag_log(DIAG_FATAL, src, "Host XMM/YMM Vector Registers (0..7):\n");
        for (int i = 0; i < 8; i++) {
            if (ymm_hi && ymm_len >= 256) {
                uint64_t hi_lo = 0, hi_hi = 0;
                memcpy(&hi_lo, ymm_hi + i * 16, 8);
                memcpy(&hi_hi, ymm_hi + i * 16 + 8, 8);
                diag_log(DIAG_FATAL, src, "  YMM%-2d = %016llx_%016llx_%016llx_%016llx\n",
                         i, (unsigned long long)hi_hi, (unsigned long long)hi_lo,
                         (unsigned long long)xmm[i].High, (unsigned long long)xmm[i].Low);
            } else {
                diag_log(DIAG_FATAL, src, "  XMM%-2d = %016llx_%016llx\n",
                         i, (unsigned long long)xmm[i].High, (unsigned long long)xmm[i].Low);
            }
        }

        dump_guest_cpu_state();
        walk_stack_dbghelp(ctx);
        diag_dump_trace_ring();
        write_minidump_win32(ep);
    } else {
        dump_guest_cpu_state();
        walk_stack_dbghelp(NULL);
        diag_dump_trace_ring();
        write_minidump_win32(NULL);
    }
#else
    dump_guest_cpu_state();
    diag_dump_trace_ring();
#endif
    diag_log(DIAG_FATAL, src, "================================================================================\n");
}

void diag_fatal_abort(const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    diag_log(DIAG_FATAL, "fatal", "%s\n", msg);
    diag_report_unhandled_exception(NULL, "fatal_abort");
    plat_fatal_notice();
    _exit(1);
}

#ifdef _WIN32
static LONG WINAPI diag_unhandled_exception_filter(EXCEPTION_POINTERS *ep) {
    diag_report_unhandled_exception(ep, "unhandled_seh");
    return EXCEPTION_EXECUTE_HANDLER;
}

// Tail Vectored Exception Handler (installed with First=0 so it runs AFTER lazy_commit and avx2_bmi2_veh)
static LONG CALLBACK diag_tail_veh(EXCEPTION_POINTERS *ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Ignore benign informational exceptions (OutputDebugString, thread naming, C++ caught exceptions, breakpoint)
    if (code == 0x40010006u || code == 0x4001000Au || code == 0x406D1388u ||
        code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;

    // Only intercept fatal hardware exceptions that were NOT handled by earlier VEH handlers
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_IN_PAGE_ERROR ||
        code == 0xC0000409u || code == 0xC0000374u) {
        // Check if it's a guest lazy-commit page fault that hasn't run yet (should already be handled by first=1 VEH,
        // but double check guest address range just in case)
        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
            uint64_t fa = ep->ExceptionRecord->ExceptionInformation[1];
            if (fa >= GUEST_BASE && fa < GUEST_BASE + GUEST_SPACE) {
                MEMORY_BASIC_INFORMATION mi;
                if (VirtualQuery((void *)(uintptr_t)fa, &mi, sizeof mi) && mi.State == MEM_RESERVE)
                    return EXCEPTION_CONTINUE_SEARCH;
            }
        }
        diag_report_unhandled_exception(ep, "veh_fatal");
        _exit(1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void crt_purecall_handler(void) {
    diag_log(DIAG_FATAL, "crt", "CRT Pure virtual function call (_purecall) invoked!\n");
    diag_report_unhandled_exception(NULL, "crt_purecall");
    _exit(1);
}

static void crt_invalid_param_handler(const wchar_t *expr, const wchar_t *func, const wchar_t *file,
                                      unsigned int line, uintptr_t reserved) {
    (void)reserved;
    diag_log(DIAG_FATAL, "crt", "CRT Invalid parameter detected: expr='%ls' func='%ls' file='%ls' line=%u\n",
             expr ? expr : L"?", func ? func : L"?", file ? file : L"?", line);
    diag_report_unhandled_exception(NULL, "crt_invalid_param");
    _exit(1);
}
#endif

static void crt_signal_handler(int sig) {
    const char *sname = (sig == SIGABRT) ? "SIGABRT" : (sig == SIGSEGV) ? "SIGSEGV" :
                        (sig == SIGILL)  ? "SIGILL"  : (sig == SIGFPE)  ? "SIGFPE"  : "SIGNAL";
    diag_log(DIAG_FATAL, "signal", "Caught fatal CRT signal %d (%s)\n", sig, sname);
    diag_report_unhandled_exception(NULL, sname);
    _exit(1);
}

void diag_install_crash_handlers(void) {
    static _Atomic bool installed = false;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&installed, &expected, true)) return;

#ifdef _WIN32
    ULONG guarantee = 64 << 10;
    SetThreadStackGuarantee(&guarantee);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(diag_unhandled_exception_filter);
    AddVectoredExceptionHandler(0, diag_tail_veh);
    _set_purecall_handler(crt_purecall_handler);
    _set_invalid_parameter_handler(crt_invalid_param_handler);
#endif
    signal(SIGABRT, crt_signal_handler);
    signal(SIGSEGV, crt_signal_handler);
    signal(SIGILL,  crt_signal_handler);
    signal(SIGFPE,  crt_signal_handler);
    diag_log(DIAG_DEBUG, "crash", "Installed SEH, tail-VEH, CRT signal/purecall/invalid_parameter, and minidump handlers\n");
}

// ============================================================================
// 10. Built-in Self-Test Runner (for Automated Verification of Crash Scenarios)
// ============================================================================
void diag_check_selftest_early(void) {
    const char *mode = getenv("MHGU_SELFTEST");
    if (!mode || !*mode) return;

    diag_log(DIAG_INFO, "selftest", "Running diagnostic verification self-test mode: '%s'\n", mode);
    diag_trace_ring_push("selftest", "Breadcrumb 1: entered diag_check_selftest_early(mode=%s)", mode);
    diag_trace_ring_push("selftest", "Breadcrumb 2: preparing synthetic test state before fault");

    if (!strcmp(mode, "null_crash")) {
        diag_phase_begin("selftest_null_deref");
        diag_trace_ring_push("selftest", "Breadcrumb 3: about to dereference NULL pointer at 0x0");
        volatile uint64_t *null_ptr = (volatile uint64_t *)(uintptr_t)0x0;
        *null_ptr = 0xDEADBEEFCAFEBABEull;
        diag_phase_end("selftest_null_deref");
    } else if (!strcmp(mode, "illegal_insn")) {
        diag_phase_begin("selftest_illegal_insn");
        diag_trace_ring_push("selftest", "Breadcrumb 3: about to execute unemulated illegal opcode (UD2 / 0F 0B)");
#if defined(_WIN32) || defined(__x86_64__)
        // Execute an invalid VEX / UD2 sequence that is not an emulated AVX2/BMI2 opcode
        __asm__ volatile(".byte 0x0f, 0x0b");
#endif
        diag_phase_end("selftest_illegal_insn");
    } else if (!strcmp(mode, "vk_fail")) {
        diag_trace_ring_push("selftest", "Breadcrumb 3: invoking backend_init() with forced vkCreateDevice failure");
        extern void backend_init(void);
        backend_init();
    }
}
