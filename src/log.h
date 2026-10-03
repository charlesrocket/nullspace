#ifndef LOG_H
#define LOG_H

enum LogLevel {
    LOG_TRACE,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
};

enum LogLevel log_get_level(void);

void log_init(void);
void log_set_level(enum LogLevel level);

void log_msg( // internal
    enum LogLevel level, const char *topic, const char *file, int line,
    const char *fmt, ...
) __attribute__((format(printf, 5, 6)));

#define log_trace(...)                                                         \
    log_msg(LOG_TRACE, LOG_TOPIC, __FILE__, __LINE__, __VA_ARGS__)
#define log_debug(...) log_msg(LOG_DEBUG, LOG_TOPIC, NULL, 0, __VA_ARGS__)
#define log_info(...)  log_msg(LOG_INFO, LOG_TOPIC, NULL, 0, __VA_ARGS__)
#define log_warn(...)  log_msg(LOG_WARN, LOG_TOPIC, NULL, 0, __VA_ARGS__)
#define log_err(...)   log_msg(LOG_ERROR, LOG_TOPIC, NULL, 0, __VA_ARGS__)

#endif // LOG_H
