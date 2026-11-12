// Crash-safe logger, startup phase tracker, CPUID/system inspector, watchdog, and minidump crash handler.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DIAG_TRACE = 0,
    DIAG_DEBUG = 1,
    DIAG_INFO  = 2,
    DIAG_WARN  = 3,
    DIAG_ERROR = 4,
    DIAG_FATAL = 5,
} DiagLevel;

extern volatile int g_diag_level;
extern volatile int g_diag_console;
extern volatile int g_diag_keep_files;
extern volatile int g_diag_watchdog_sec;
extern volatile int g_diag_vk_validation;
extern volatile int g_diag_minidump;

// Initialize logger as the very first step at startup (idempotent; also runs via constructor(101)).
void diag_init_early(void);

// Re-apply configuration after mhgurecomp.ini and environment variables are parsed.
void diag_configure(int level, int console_mode, int keep_files, int watchdog_sec, int vk_validation, int minidump);

// Parse a log level string ("TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL") -> DiagLevel (or -1 if invalid).
int diag_parse_level(const char *str);
const char *diag_level_name(int level);

// Core thread-safe, write-through logger.
void diag_log(DiagLevel level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void diag_vlog(DiagLevel level, const char *tag, const char *fmt, va_list ap);

// Legacy compatibility wrapper for existing LOG(...) macro calls (extracts leading "[tag]" if present).
void diag_log_legacy(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Record a message directly into the in-memory TRACE ring buffer (dumped on crash/fatal).
void diag_trace_ring_push(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void diag_dump_trace_ring(void);

// System, CPUID, XGETBV, SHA-256, paths, and config dump at startup.
void diag_log_system_info(void);
void diag_log_paths_and_files(const char *game_dir, const char *save_dir);
void diag_log_effective_ini(const char *ini_path);
void diag_log_binary_hashes(void);

// Startup phase tracking (records start timestamp, duration in ms, or failure with GetLastError).
void diag_phase_begin(const char *phase_name);
void diag_phase_end(const char *phase_name);
void diag_phase_fail(const char *phase_name, int err_code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
const char *diag_current_phase(void);

// Thread naming & lifecycle tracking + Watchdog.
void diag_thread_register(const char *name, uint32_t guest_handle, uint32_t guest_entry);
void diag_thread_unregister(void);
const char *diag_thread_name(void);
void diag_watchdog_start(void);
void diag_watchdog_heartbeat(void);

// Exception, VEH, and fatal crash reporting.
void diag_install_crash_handlers(void);
void diag_report_unhandled_exception(void *exception_pointers, const char *source_tag);
void diag_fatal_abort(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

// Vulkan diagnostic helpers.
const char *diag_vk_result_str(int vk_result);
void diag_log_vk_create(const char *api_name, int vk_result, const char *detail);
void diag_log_vk_device_lost(const char *api_name, int vk_result, uint64_t frame_no, uint64_t submit_seq, uint64_t completed_seq);

// Built-in verification self-test runner (triggered via MHGU_SELFTEST=null_crash|illegal_insn|vk_fail).
void diag_check_selftest_early(void);

#define LOG_TRACE(tag, ...) do { \
    if (__builtin_expect(g_diag_level <= DIAG_TRACE, 0)) diag_log(DIAG_TRACE, tag, __VA_ARGS__); \
    else diag_trace_ring_push(tag, __VA_ARGS__); \
} while (0)

#define LOG_DEBUG(tag, ...) do { \
    if (__builtin_expect(g_diag_level <= DIAG_DEBUG, 0)) diag_log(DIAG_DEBUG, tag, __VA_ARGS__); \
    else diag_trace_ring_push(tag, __VA_ARGS__); \
} while (0)

#define LOG_INFO(tag, ...)  diag_log(DIAG_INFO,  tag, __VA_ARGS__)
#define LOG_WARN(tag, ...)  diag_log(DIAG_WARN,  tag, __VA_ARGS__)
#define LOG_ERROR(tag, ...) diag_log(DIAG_ERROR, tag, __VA_ARGS__)
#define LOG_FATAL(tag, ...) diag_fatal_abort("[" tag "] " __VA_ARGS__)

#ifdef __cplusplus
}
#endif
