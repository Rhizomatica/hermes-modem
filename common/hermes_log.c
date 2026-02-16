#include "hermes_log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HERMES_LOG_DEFAULT_CAPACITY 1024
#define HERMES_LOG_COMPONENT_MAX 24
#define HERMES_LOG_MESSAGE_MAX 240

typedef struct
{
    struct timespec ts;
    hermes_log_level_t level;
    char component[HERMES_LOG_COMPONENT_MAX];
    char message[HERMES_LOG_MESSAGE_MAX];
} hermes_log_entry_t;

typedef struct
{
    pthread_t worker;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    hermes_log_entry_t *entries;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    bool running;
    bool initialized;
    atomic_ulong dropped;
    atomic_int min_level;
} hermes_log_state_t;

static hermes_log_state_t g_log = {
    .worker = 0,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .entries = NULL,
    .capacity = 0,
    .head = 0,
    .tail = 0,
    .count = 0,
    .running = false,
    .initialized = false,
    .dropped = 0,
    .min_level = HERMES_LOG_LEVEL_INFO,
};

static const char *level_name(hermes_log_level_t level)
{
    switch (level)
    {
    case HERMES_LOG_LEVEL_DEBUG:
        return "DBG";
    case HERMES_LOG_LEVEL_INFO:
        return "INF";
    case HERMES_LOG_LEVEL_WARN:
        return "WRN";
    case HERMES_LOG_LEVEL_ERROR:
    default:
        return "ERR";
    }
}

static void print_entry(const hermes_log_entry_t *entry)
{
    struct tm tmv;
    char tsbuf[40];
    time_t sec;
    long ms;
    unsigned long dropped;

    if (!entry)
        return;

    sec = entry->ts.tv_sec;
    ms = entry->ts.tv_nsec / 1000000L;
    localtime_r(&sec, &tmv);
    snprintf(tsbuf,
             sizeof(tsbuf),
             "%02d:%02d:%02d.%03ld",
             tmv.tm_hour,
             tmv.tm_min,
             tmv.tm_sec,
             ms);

    fprintf(stderr, "%s [%s] [%s] %s\n", tsbuf, level_name(entry->level), entry->component, entry->message);

    dropped = atomic_exchange_explicit(&g_log.dropped, 0, memory_order_relaxed);
    if (dropped > 0)
        fprintf(stderr, "%s [WRN] [log] dropped %lu messages\n", tsbuf, dropped);
}

static void *log_worker(void *arg)
{
    (void)arg;

    for (;;)
    {
        hermes_log_entry_t entry;
        bool have_entry = false;

        pthread_mutex_lock(&g_log.lock);
        while (g_log.running && g_log.count == 0)
            pthread_cond_wait(&g_log.cond, &g_log.lock);

        if (g_log.count > 0)
        {
            entry = g_log.entries[g_log.head];
            g_log.head = (g_log.head + 1) % g_log.capacity;
            g_log.count--;
            have_entry = true;
        }
        else if (!g_log.running)
        {
            pthread_mutex_unlock(&g_log.lock);
            break;
        }
        pthread_mutex_unlock(&g_log.lock);

        if (have_entry)
            print_entry(&entry);
    }

    return NULL;
}

int hermes_log_init(size_t capacity)
{
    int rc;

    pthread_mutex_lock(&g_log.lock);
    if (g_log.initialized)
    {
        pthread_mutex_unlock(&g_log.lock);
        return 0;
    }

    if (capacity == 0)
        capacity = HERMES_LOG_DEFAULT_CAPACITY;

    g_log.entries = (hermes_log_entry_t *)calloc(capacity, sizeof(*g_log.entries));
    if (!g_log.entries)
    {
        pthread_mutex_unlock(&g_log.lock);
        return -1;
    }

    g_log.capacity = capacity;
    g_log.head = 0;
    g_log.tail = 0;
    g_log.count = 0;
    g_log.running = true;
    g_log.initialized = true;
    atomic_store_explicit(&g_log.dropped, 0, memory_order_relaxed);
    atomic_store_explicit(&g_log.min_level, HERMES_LOG_LEVEL_INFO, memory_order_relaxed);
    pthread_mutex_unlock(&g_log.lock);

    rc = pthread_create(&g_log.worker, NULL, log_worker, NULL);
    if (rc != 0)
    {
        pthread_mutex_lock(&g_log.lock);
        g_log.running = false;
        g_log.initialized = false;
        free(g_log.entries);
        g_log.entries = NULL;
        g_log.capacity = 0;
        pthread_mutex_unlock(&g_log.lock);
        return -1;
    }

    return 0;
}

void hermes_log_shutdown(void)
{
    pthread_t worker;
    bool should_join = false;

    pthread_mutex_lock(&g_log.lock);
    if (!g_log.initialized)
    {
        pthread_mutex_unlock(&g_log.lock);
        return;
    }

    g_log.running = false;
    worker = g_log.worker;
    should_join = true;
    pthread_cond_broadcast(&g_log.cond);
    pthread_mutex_unlock(&g_log.lock);

    if (should_join)
        pthread_join(worker, NULL);

    pthread_mutex_lock(&g_log.lock);
    free(g_log.entries);
    g_log.entries = NULL;
    g_log.capacity = 0;
    g_log.count = 0;
    g_log.head = 0;
    g_log.tail = 0;
    g_log.initialized = false;
    pthread_mutex_unlock(&g_log.lock);
}

void hermes_log_set_level(hermes_log_level_t level)
{
    atomic_store_explicit(&g_log.min_level, level, memory_order_relaxed);
}

unsigned long hermes_log_dropped_count(void)
{
    return atomic_load_explicit(&g_log.dropped, memory_order_relaxed);
}

void hermes_logf(hermes_log_level_t level, const char *component, const char *fmt, ...)
{
    hermes_log_entry_t entry;
    va_list ap;

    if (level < atomic_load_explicit(&g_log.min_level, memory_order_relaxed))
        return;

    if (!g_log.initialized || !g_log.running || !fmt)
        return;

    clock_gettime(CLOCK_REALTIME, &entry.ts);
    entry.level = level;
    snprintf(entry.component, sizeof(entry.component), "%s", component ? component : "core");

    va_start(ap, fmt);
    vsnprintf(entry.message, sizeof(entry.message), fmt, ap);
    va_end(ap);

    if (pthread_mutex_trylock(&g_log.lock) != 0)
    {
        atomic_fetch_add_explicit(&g_log.dropped, 1, memory_order_relaxed);
        return;
    }

    if (!g_log.running || g_log.count >= g_log.capacity)
    {
        atomic_fetch_add_explicit(&g_log.dropped, 1, memory_order_relaxed);
        pthread_mutex_unlock(&g_log.lock);
        return;
    }

    g_log.entries[g_log.tail] = entry;
    g_log.tail = (g_log.tail + 1) % g_log.capacity;
    g_log.count++;
    pthread_cond_signal(&g_log.cond);
    pthread_mutex_unlock(&g_log.lock);
}
