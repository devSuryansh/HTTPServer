#ifndef KEEL_LOG_H
#define KEEL_LOG_H

#define KEEL_LOG_ERROR 0
#define KEEL_LOG_WARN 1
#define KEEL_LOG_INFO 2
#define KEEL_LOG_DEBUG 3

/* Empty paths log to stdout (access) and stderr (errors). */
int keel_log_open(const char *access_path, const char *error_path, int level);
void keel_log_reopen(void);
void keel_log_close(void);

void keel_log_error(const char *fmt, ...);
void keel_log_warn(const char *fmt, ...);
void keel_log_info(const char *fmt, ...);
void keel_log_debug(const char *fmt, ...);

void keel_log_access(const char *peer, const char *request_line, int status,
                     unsigned long long body_bytes, const char *referer, const char *user_agent);

#endif
