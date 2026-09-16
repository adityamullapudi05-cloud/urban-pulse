/**
 * ============================================================================
 * smart_city_common.h
 * Smart City RTOS Fault & Performance Monitoring Platform
 *
 * Shared definitions between:
 *   node1_workload.c  - Raspberry Pi 1 (Monitored Workload Node)
 *   node2_supervisor.c - Raspberry Pi 2 (Supervisor / Analytics Node)
 *
 * Target: QNX Neutrino RTOS on Raspberry Pi 4/5
 * ============================================================================
 */

#ifndef SMART_CITY_COMMON_H
#define SMART_CITY_COMMON_H

#define _GNU_SOURCE
#define __USE_MINGW_ANSI_STDIO 1

#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(__QNX__)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <inttypes.h>
#include <errno.h>

#if defined(__QNX__) || defined(__QNXNTO__)
#include <sys/neutrino.h>
#include <sys/syspage.h>
#endif

#if !defined(_WIN32) || defined(__CYGWIN__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#endif

/* 64-bit printf format — use PRIu64 from inttypes.h for portability
 * On QNX aarch64: uint64_t = unsigned long  → PRIu64 = "lu"
 * On Windows:     uint64_t = unsigned long long → PRIu64 = "I64u" (MSVC)
 *                                                         "llu"  (MinGW)
 */
#if defined(_WIN32) && !defined(__CYGWIN__)
#define FMT_U64 "%I64u"
#else
#define FMT_U64 "%" PRIu64
#endif

/* ============================================================================
 * CONFIGURATION & NETWORK
 * ============================================================================ */
#define PLATFORM_NAME          "UrbanPulse-QNX-v2.0"
#define TELEMETRY_PORT         5555        /* TCP port Pi1 -> Pi2 */
#define TELEMETRY_INTERVAL_MS  500         /* Send every 500 ms */
#define MONITOR_INTERVAL_MS    100         /* Monitoring task period */
#define MAX_TASKS              3
#define TRACE_BUFFER_SIZE      256
#define IPC_HISTORY_SIZE       100
#define MAX_ACTIVE_FAULTS      16

/* ============================================================================
 * REAL-TIME PRIORITY HIERARCHY  (conforming to design.md §4)
 * Higher number = Higher priority in QNX SCHED_FIFO
 * ============================================================================ */
#define PRIORITY_FAULT_DETECTOR  25   /* Highest: Safety-critical fault engine */
#define PRIORITY_MONITOR         20   /* High: Heartbeat & deadline sampling   */
#define PRIORITY_SERVICE_A       15   /* Medium: Smart Traffic (100 ms)        */
#define PRIORITY_SERVICE_B       14   /* Medium: Smart Grid   (200 ms)         */
#define PRIORITY_SERVICE_C       13   /* Medium: Environment  (500 ms)         */
#define PRIORITY_TELEMETRY       10   /* Normal: TCP telemetry pipeline        */
#define PRIORITY_CLI              5   /* Low:    UART / Diagnostic CLI         */

/* ============================================================================
 * FAULT THRESHOLDS  (conforming to design.md §5, §7, §12)
 * ============================================================================ */
#define CPU_WARN_THRESHOLD       70.0f    /* % */
#define CPU_CRIT_THRESHOLD       85.0f    /* % */
#define IPC_WARN_LATENCY_US      1000U    /* 1 ms  */
#define IPC_CRIT_LATENCY_US      50000U   /* 50 ms */

/* ============================================================================
 * ENUMERATIONS
 * ============================================================================ */
typedef enum {
    TASK_STATE_STOPPED = 0,
    TASK_STATE_RUNNING,
    TASK_STATE_WARNING,
    TASK_STATE_FAULT,
    TASK_STATE_STARVED
} TaskState;

typedef enum {
    FAULT_NONE = 0,
    FAULT_TASK_STARVATION,
    FAULT_DEADLINE_MISS,
    FAULT_CPU_OVERLOAD,
    FAULT_IPC_TIMEOUT,
    FAULT_SERVICE_FAILURE,
    FAULT_NODE_DISCONNECTED
} FaultType;

typedef enum {
    SEV_INFO = 0,
    SEV_WARNING,
    SEV_CRITICAL
} FaultSeverity;

typedef enum {
    LED_GREEN = 0,   /* System Healthy  - GPIO pin 17 */
    LED_YELLOW,      /* Warning present - GPIO pin 27 */
    LED_RED          /* Critical Fault  - GPIO pin 22 */
} HealthLedState;

typedef enum {
    EVENT_TASK_START = 1,
    EVENT_TASK_END,
    EVENT_HEARTBEAT,
    EVENT_FAULT,
    EVENT_IPC_XFER
} TraceEventType;

/* ============================================================================
 * CORE DATA STRUCTURES
 * ============================================================================ */

/** Task Control Block - runtime state of each periodic service */
typedef struct {
    uint32_t  task_id;
    char      name[32];
    uint32_t  period_ms;           /* Periodic execution interval  */
    uint32_t  deadline_ms;         /* Maximum allowed exec time    */
    uint32_t  priority;

    /* Live metrics */
    uint32_t  heartbeat;           /* Incremented every period     */
    uint64_t  last_heartbeat_time_us;
    uint32_t  missed_heartbeats;
    uint64_t  last_start_us;
    uint64_t  last_end_us;
    uint32_t  last_exec_us;
    uint32_t  max_exec_us;
    uint64_t  total_exec_us;
    uint32_t  total_cycles;
    uint32_t  deadline_misses;
    float     cpu_usage_pct;
    TaskState state;

    /* Fault Injection */
    bool      inject_starvation;   /* Freeze heartbeat             */
    uint32_t  inject_exec_delay_ms;/* Inflate execution duration   */
} TaskControlBlock;

/** Active Fault Record in the Fault Map */
typedef struct {
    uint32_t      fault_id;
    uint32_t      node_id;
    uint32_t      task_id;
    FaultType     type;
    FaultSeverity severity;
    uint64_t      timestamp_us;
    char          description[64];
    bool          active;
} FaultRecord;

/** IPC Shared Memory Channel (Service A -> Service B) */
typedef struct {
    uint32_t sequence;
    uint64_t send_time_us;
    uint64_t recv_time_us;
    uint32_t latency_us;
    char     payload[64];
    bool     data_available;
    uint32_t inject_delay_ms;      /* IPC latency injection        */
} IpcChannel;

/** Running IPC latency statistics (Min / Max / Avg / P95) */
typedef struct {
    uint32_t samples[IPC_HISTORY_SIZE];
    uint32_t count;
    uint32_t head;
    uint32_t min_us;
    uint32_t max_us;
    uint32_t avg_us;
    uint32_t p95_us;
} IpcStats;

/** Execution Trace Event for the Gantt ring buffer */
typedef struct {
    uint64_t       timestamp_us;
    uint32_t       task_id;
    TraceEventType event_type;
    uint32_t       duration_us;
} TraceRecord;

/** Ring buffer of trace records (for Gantt & fault timeline) */
typedef struct {
    TraceRecord records[TRACE_BUFFER_SIZE];
    uint32_t    head;
    uint32_t    count;
} TraceRingBuffer;

/* ============================================================================
 * TCP TELEMETRY PACKET  (Pi 1 -> Pi 2, JSON newline-delimited)
 *
 * Format example:
 *   {"node":1,"timestamp":1234567890,"cpu":43.7,"led":0,
 *    "ipc":{"min":18,"max":127,"avg":42,"p95":71},
 *    "tasks":[
 *      {"id":1,"name":"Service_A","hb":120,"exec_us":14200,"miss":0,"state":1},
 *      {"id":2,"name":"Service_B","hb":60, "exec_us":25100,"miss":0,"state":1},
 *      {"id":3,"name":"Service_C","hb":24, "exec_us":40300,"miss":0,"state":1}
 *    ]}
 * ============================================================================ */

/* ============================================================================
 * PORTABLE MONOTONIC CLOCK UTILITIES
 * ============================================================================ */
static inline uint64_t get_time_us(void) {
#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(__QNX__)
    static LARGE_INTEGER freq;
    static int init = 0;
    if (!init) { QueryPerformanceFrequency(&freq); init = 1; }
    LARGE_INTEGER cnt;
    QueryPerformanceCounter(&cnt);
    return (uint64_t)((cnt.QuadPart * 1000000ULL) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000ULL) + ((uint64_t)ts.tv_nsec / 1000ULL);
#endif
}

static inline void sleep_ms(uint32_t ms) {
#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(__QNX__)
    Sleep(ms);
#else
    struct timespec req = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&req, NULL);
#endif
}

/* ============================================================================
 * PORTABLE REAL-TIME THREAD ABSTRACTION
 * ============================================================================ */
typedef void* (*thread_func_t)(void*);

#if defined(_WIN32) && !defined(__CYGWIN__) && !defined(__QNX__)

typedef HANDLE rt_thread_t;
typedef CRITICAL_SECTION rt_mutex_t;
typedef CONDITION_VARIABLE rt_cond_t;

#define RT_MUTEX_INIT(m)    InitializeCriticalSection(m)
#define RT_MUTEX_DESTROY(m) DeleteCriticalSection(m)
#define RT_MUTEX_LOCK(m)    EnterCriticalSection(m)
#define RT_MUTEX_UNLOCK(m)  LeaveCriticalSection(m)
#define RT_COND_INIT(c)     InitializeConditionVariable(c)
#define RT_COND_SIGNAL(c)   WakeConditionVariable(c)
#define RT_COND_WAIT(c,m)   SleepConditionVariableCS((c),(m),INFINITE)

typedef struct { thread_func_t func; void *arg; } _win_ta;
static inline DWORD WINAPI _win_tramp(LPVOID p) {
    _win_ta *a = (_win_ta*)p; thread_func_t f = a->func; void *arg = a->arg;
    free(a); f(arg); return 0;
}
static inline int rt_thread_create(rt_thread_t *t, int pri, thread_func_t fn, void *arg) {
    _win_ta *a = (_win_ta*)malloc(sizeof(_win_ta)); a->func = fn; a->arg = arg;
    *t = CreateThread(NULL, 0, _win_tramp, a, 0, NULL);
    if (!*t) return -1;
    int wp = THREAD_PRIORITY_NORMAL;
    if (pri >= 20) wp = THREAD_PRIORITY_TIME_CRITICAL;
    else if (pri >= 15) wp = THREAD_PRIORITY_HIGHEST;
    else if (pri >= 12) wp = THREAD_PRIORITY_ABOVE_NORMAL;
    else if (pri <= 5)  wp = THREAD_PRIORITY_BELOW_NORMAL;
    SetThreadPriority(*t, wp); return 0;
}
static inline void rt_thread_join(rt_thread_t t) {
    WaitForSingleObject(t, INFINITE); CloseHandle(t);
}

#else  /* QNX / POSIX */

typedef pthread_t        rt_thread_t;
typedef pthread_mutex_t  rt_mutex_t;
typedef pthread_cond_t   rt_cond_t;

#define RT_MUTEX_INIT(m) do { \
    pthread_mutexattr_t _a; pthread_mutexattr_init(&_a); \
    pthread_mutexattr_setprotocol(&_a, PTHREAD_PRIO_INHERIT); \
    pthread_mutex_init((m), &_a); pthread_mutexattr_destroy(&_a); \
} while(0)
#define RT_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#define RT_MUTEX_LOCK(m)    pthread_mutex_lock(m)
#define RT_MUTEX_UNLOCK(m)  pthread_mutex_unlock(m)
#define RT_COND_INIT(c)     pthread_cond_init((c), NULL)
#define RT_COND_SIGNAL(c)   pthread_cond_signal(c)
#define RT_COND_WAIT(c,m)   pthread_cond_wait((c),(m))

static inline int rt_thread_create(rt_thread_t *t, int pri, thread_func_t fn, void *arg) {
    pthread_attr_t attr;
    struct sched_param sp;
    pthread_attr_init(&attr);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    sp.sched_priority = pri;
    pthread_attr_setschedparam(&attr, &sp);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    int r = pthread_create(t, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    return r;
}
static inline void rt_thread_join(rt_thread_t t) { pthread_join(t, NULL); }

#endif  /* QNX / POSIX */

#endif /* SMART_CITY_COMMON_H */
