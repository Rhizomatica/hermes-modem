#ifndef HERMES_LOG_H_
#define HERMES_LOG_H_

#include <stddef.h>

typedef enum
{
    HERMES_LOG_LEVEL_DEBUG = 0,
    HERMES_LOG_LEVEL_INFO = 1,
    HERMES_LOG_LEVEL_WARN = 2,
    HERMES_LOG_LEVEL_ERROR = 3
} hermes_log_level_t;

int hermes_log_init(size_t capacity);
void hermes_log_shutdown(void);
void hermes_log_set_level(hermes_log_level_t level);
unsigned long hermes_log_dropped_count(void);
void hermes_logf(hermes_log_level_t level, const char *component, const char *fmt, ...);

#define HLOGD(component, fmt, ...) hermes_logf(HERMES_LOG_LEVEL_DEBUG, component, fmt, ##__VA_ARGS__)
#define HLOGI(component, fmt, ...) hermes_logf(HERMES_LOG_LEVEL_INFO, component, fmt, ##__VA_ARGS__)
#define HLOGW(component, fmt, ...) hermes_logf(HERMES_LOG_LEVEL_WARN, component, fmt, ##__VA_ARGS__)
#define HLOGE(component, fmt, ...) hermes_logf(HERMES_LOG_LEVEL_ERROR, component, fmt, ##__VA_ARGS__)

#endif /* HERMES_LOG_H_ */
