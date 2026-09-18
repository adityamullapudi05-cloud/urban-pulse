/**
 * ============================================================================
 * Smart City RTOS Fault & Performance Monitoring Platform
 * 
 * Target: Raspberry Pi 4/5 Cluster
 * Architecture: Distributed 2-Node (Node1 & Supervisor Node)
 * 
 * Description :
 *  - 3 Periodic Services (Service A: 100ms/80ms, Service B: 200ms/150ms, Service C: 500ms/400ms)
 *  - High-Priority Monitoring Task (100ms cycle, Heartbeat, Deadline, CPU Usage)
 *  - Shared Memory IPC Latency Tracking (Min, Max, Avg, P95)
 *  - Trace Collector & Ring Buffer with ASCII Gantt Chart
 *  - Fault Detector (Starvation, Deadline Miss, CPU Overload, IPC Latency, Network Loss)
 *  - GPIO 3-Color Health Indicator (Green = Healthy, Yellow = Warning, Red = Fault)
 *  - Non-blocking TCP Telemetry between Node 1 (Workload) and Node 2 (Supervisor)
 *  - Fault Injection Subsystem (Starvation, Execution Delay, CPU Burner, IPC Lag)
 *  - Interactive Diagnostic CLI
 * ============================================================================
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <inttypes.h>

#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>

#if defined(__QNX__) || defined(__QNXNTO__)
#include <sys/neutrino.h>
#include <sys/syspage.h>
#endif

#define FMT_U64 "%" PRIu64

/* ============================================================================
 * REAL-TIME THREADING & SYNCHRONIZATION (QNX / POSIX)
 * ============================================================================ */
typedef pthread_t rt_thread_t;
typedef pthread_mutex_t rt_mutex_t;
typedef pthread_cond_t rt_cond_t;
typedef void* (*thread_func_t)(void*);

#define RT_MUTEX_INIT(m) do { \
    pthread_mutexattr_t _a; \
    pthread_mutexattr_init(&_a); \
    pthread_mutexattr_setprotocol(&_a, PTHREAD_PRIO_INHERIT); \
    pthread_mutex_init((m), &_a); \
    pthread_mutexattr_destroy(&_a); \
} while(0)
#define RT_MUTEX_DESTROY(m)     pthread_mutex_destroy(m)
#define RT_MUTEX_LOCK(m)        pthread_mutex_lock(m)
#define RT_MUTEX_UNLOCK(m)      pthread_mutex_unlock(m)
#define RT_COND_INIT(c)         pthread_cond_init((c), NULL)
#define RT_COND_SIGNAL(c)       pthread_cond_signal(c)
#define RT_COND_WAIT(c, m)      pthread_cond_wait((c), (m))

static inline int rt_thread_create(rt_thread_t *thread, int priority, thread_func_t func, void *arg) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    struct sched_param param;
    param.sched_priority = priority;
    pthread_attr_setschedparam(&attr, &param);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

    int res = pthread_create(thread, &attr, func, arg);
    pthread_attr_destroy(&attr);
    return res;
}

static inline void rt_thread_join(rt_thread_t thread) {
    pthread_join(thread, NULL);
}

/* ============================================================================
 * CONFIGURATION CONSTANTS & REAL-TIME PRIORITIES
 * ============================================================================ */
#define PLATFORM_NAME               "SmartCity-QNX-v2.0"
#define DEFAULT_PORT                5555
#define MAX_TASKS                   3
#define TRACE_BUFFER_SIZE           256
#define IPC_HISTORY_SIZE            100
#define MAX_ACTIVE_FAULTS           16

/* Real-Time Priority Hierarchy */
#define PRIORITY_FAULT_DETECTOR     25  /* Highest: Fault safety */
#define PRIORITY_MONITOR            20  /* High: Sampling & metrics */
#define PRIORITY_SERVICE_A          15  /* Medium: Smart Traffic */
#define PRIORITY_SERVICE_B          14  /* Medium: Smart Grid */
#define PRIORITY_SERVICE_C          13  /* Medium: Environment */
#define PRIORITY_TELEMETRY          10  /* Normal: TCP Telemetry */
#define PRIORITY_CLI                5   /* Low: UART / CLI Diagnostics */

/* Thresholds */
#define HEARTBEAT_WARN_CYCLES       2
#define HEARTBEAT_FAULT_CYCLES      3
#define CPU_WARN_THRESHOLD          70.0f
#define CPU_CRIT_THRESHOLD          85.0f
#define IPC_WARN_LATENCY_US         1000   /* 1 ms */
#define IPC_CRIT_LATENCY_US         50000  /* 50 ms */

/* ============================================================================
 * ENUMS & CORE DATA STRUCTURES
 * ============================================================================ */
typedef enum {
    NODE_MODE_STANDALONE = 0,   /* Integrated single-node test */
    NODE_MODE_WORKLOAD,         /* Raspberry Pi 1: Monitored Workload Node */
    NODE_MODE_SUPERVISOR        /* Raspberry Pi 2: Supervisor & Analytics */
} NodeMode;

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
    LED_GREEN = 0,
    LED_YELLOW,
    LED_RED
} HealthLedState;

typedef enum {
    EVENT_TASK_START = 1,
    EVENT_TASK_END,
    EVENT_HEARTBEAT,
    EVENT_FAULT,
    EVENT_IPC_XFER
} TraceEventType;

/* Task Status & Control Block */
typedef struct {
    uint32_t task_id;
    char name[32];
    uint32_t period_ms;
    uint32_t deadline_ms;
    uint32_t priority;

    /* Dynamic runtime metrics */
    uint32_t heartbeat;
    uint32_t prev_heartbeat;
    uint32_t missed_heartbeats;
    uint64_t last_heartbeat_time_us;
    uint64_t last_start_us;
    uint64_t last_end_us;
    uint32_t last_exec_us;
    uint32_t max_exec_us;
    uint64_t total_exec_us;
    uint32_t total_cycles;
    uint32_t deadline_misses;
    float cpu_usage_pct;
    TaskState state;

    /* Fault Injection controls */
    bool inject_starvation;
    uint32_t inject_exec_delay_ms;
} TaskControlBlock;

/* Active Fault Record */
typedef struct {
    uint32_t fault_id;
    uint32_t node_id;
    uint32_t task_id;
    FaultType type;
    FaultSeverity severity;
    uint64_t timestamp_us;
    char description[64];
    bool active;
} FaultRecord;

/* IPC Shared Memory Message Buffer */
typedef struct {
    uint32_t sequence;
    uint64_t send_time_us;
    uint64_t recv_time_us;
    uint32_t latency_us;
    char payload[64];
    bool data_available;
    uint32_t inject_delay_ms;
} IpcChannel;

/* IPC Latency Statistics (Min, Max, Avg, P95) */
typedef struct {
    uint32_t samples[IPC_HISTORY_SIZE];
    uint32_t count;
    uint32_t head;
    uint32_t min_us;
    uint32_t max_us;
    uint32_t avg_us;
    uint32_t p95_us;
} IpcStats;

/* Execution Trace Event for Gantt Chart */
typedef struct {
    uint64_t timestamp_us;
    uint32_t task_id;
    TraceEventType event_type;
    uint32_t duration_us;
} TraceRecord;

/* Ring Buffer for Trace Events */
typedef struct {
    TraceRecord records[TRACE_BUFFER_SIZE];
    uint32_t head;
    uint32_t count;
    rt_mutex_t lock;
} TraceRingBuffer;

/* System Global Context */
typedef struct {
    NodeMode mode;
    volatile bool running;
    uint32_t node_id;
    float total_cpu_pct;
    HealthLedState led_state;
    bool peer_connected;
    uint64_t last_peer_contact_us;

    /* Workloads */
    TaskControlBlock tasks[MAX_TASKS];
    rt_mutex_t task_mutex;

    /* IPC */
    IpcChannel ipc_channel;
    IpcStats ipc_stats;
    rt_mutex_t ipc_mutex;
    rt_cond_t ipc_cond;

    /* Fault Engine */
    FaultRecord fault_map[MAX_ACTIVE_FAULTS];
    uint32_t fault_counter;
    rt_mutex_t fault_mutex;

    /* Trace Buffer */
    TraceRingBuffer trace_buf;

    /* CPU Burner for Overload Injection */
    bool inject_cpu_overload;

    /* Network Telemetry */
    char peer_ip[64];
    int peer_port;
} SystemContext;

static SystemContext g_sys;

/* ============================================================================
 * HIGH-PRECISION MONOTONIC TIME UTILITIES
 * ============================================================================ */
static inline uint64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000ULL) + ((uint64_t)ts.tv_nsec / 1000ULL);
}

static inline void sleep_ms(uint32_t ms) {
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&req, NULL);
}

/* ============================================================================
 * HARDWARE ABSTRACTION LAYER (HAL): GPIO LEDs & UART
 * ============================================================================ */
static void hal_set_health_led(HealthLedState state) {
    g_sys.led_state = state;
    /* On physical Raspberry Pi under QNX:
     * Write to BCM GPIO pins: Green (17), Yellow (27), Red (22)
     */
}

static const char* hal_get_led_string(void) {
    switch (g_sys.led_state) {
        case LED_GREEN:  return "\033[1;32m[● GREEN - HEALTHY]\033[0m";
        case LED_YELLOW: return "\033[1;33m[▲ YELLOW - WARNING]\033[0m";
        case LED_RED:    return "\033[1;31m[■ RED - CRITICAL FAULT]\033[0m";
        default:         return "[UNKNOWN]";
    }
}

/* ============================================================================
 * TRACE COLLECTOR RING BUFFER
 * ============================================================================ */
static void trace_record_event(uint32_t task_id, TraceEventType type, uint32_t duration_us) {
    RT_MUTEX_LOCK(&g_sys.trace_buf.lock);
    uint32_t idx = g_sys.trace_buf.head;
    g_sys.trace_buf.records[idx].timestamp_us = get_time_us();
    g_sys.trace_buf.records[idx].task_id = task_id;
    g_sys.trace_buf.records[idx].event_type = type;
    g_sys.trace_buf.records[idx].duration_us = duration_us;

    g_sys.trace_buf.head = (idx + 1) % TRACE_BUFFER_SIZE;
    if (g_sys.trace_buf.count < TRACE_BUFFER_SIZE) {
        g_sys.trace_buf.count++;
    }
    RT_MUTEX_UNLOCK(&g_sys.trace_buf.lock);
}

/* ============================================================================
 * FAULT DETECTION ENGINE & FAULT MAP
 * ============================================================================ */
static void register_fault(uint32_t node_id, uint32_t task_id, FaultType type, 
                           FaultSeverity severity, const char* desc) {
    RT_MUTEX_LOCK(&g_sys.fault_mutex);

    /* Check if already active */
    for (int i = 0; i < MAX_ACTIVE_FAULTS; i++) {
        if (g_sys.fault_map[i].active && 
            g_sys.fault_map[i].task_id == task_id && 
            g_sys.fault_map[i].type == type) {
            g_sys.fault_map[i].timestamp_us = get_time_us();
            RT_MUTEX_UNLOCK(&g_sys.fault_mutex);
            return;
        }
    }

    /* Find empty slot */
    int slot = -1;
    for (int i = 0; i < MAX_ACTIVE_FAULTS; i++) {
        if (!g_sys.fault_map[i].active) {
            slot = i;
            break;
        }
    }

    if (slot != -1) {
        g_sys.fault_map[slot].fault_id = ++g_sys.fault_counter;
        g_sys.fault_map[slot].node_id = node_id;
        g_sys.fault_map[slot].task_id = task_id;
        g_sys.fault_map[slot].type = type;
        g_sys.fault_map[slot].severity = severity;
        g_sys.fault_map[slot].timestamp_us = get_time_us();
        strncpy(g_sys.fault_map[slot].description, desc, sizeof(g_sys.fault_map[slot].description) - 1);
        g_sys.fault_map[slot].active = true;

        trace_record_event(task_id, EVENT_FAULT, 0);

        if (severity == SEV_CRITICAL) {
            hal_set_health_led(LED_RED);
        } else if (severity == SEV_WARNING && g_sys.led_state != LED_RED) {
            hal_set_health_led(LED_YELLOW);
        }
    }
    RT_MUTEX_UNLOCK(&g_sys.fault_mutex);
}

static void clear_fault_type(uint32_t task_id, FaultType type) {
    RT_MUTEX_LOCK(&g_sys.fault_mutex);
    bool has_critical = false;
    bool has_warning = false;

    for (int i = 0; i < MAX_ACTIVE_FAULTS; i++) {
        if (g_sys.fault_map[i].active) {
            if (g_sys.fault_map[i].task_id == task_id && g_sys.fault_map[i].type == type) {
                g_sys.fault_map[i].active = false;
            } else {
                if (g_sys.fault_map[i].severity == SEV_CRITICAL) has_critical = true;
                if (g_sys.fault_map[i].severity == SEV_WARNING) has_warning = true;
            }
        }
    }

    if (has_critical) hal_set_health_led(LED_RED);
    else if (has_warning) hal_set_health_led(LED_YELLOW);
    else hal_set_health_led(LED_GREEN);

    RT_MUTEX_UNLOCK(&g_sys.fault_mutex);
}

static void clear_all_faults(void) {
    RT_MUTEX_LOCK(&g_sys.fault_mutex);
    for (int i = 0; i < MAX_ACTIVE_FAULTS; i++) {
        g_sys.fault_map[i].active = false;
    }
    hal_set_health_led(LED_GREEN);
    RT_MUTEX_UNLOCK(&g_sys.fault_mutex);
}

/* ============================================================================
 * SHARED MEMORY IPC & LATENCY PROFILER
 * ============================================================================ */
static void ipc_record_latency(uint32_t latency_us) {
    IpcStats *st = &g_sys.ipc_stats;
    st->samples[st->head] = latency_us;
    st->head = (st->head + 1) % IPC_HISTORY_SIZE;
    if (st->count < IPC_HISTORY_SIZE) st->count++;

    uint32_t min = 0xFFFFFFFF;
    uint32_t max = 0;
    uint64_t sum = 0;

    uint32_t temp[IPC_HISTORY_SIZE];
    memcpy(temp, st->samples, sizeof(uint32_t) * st->count);

    for (uint32_t i = 0; i < st->count; i++) {
        uint32_t val = temp[i];
        if (val < min) min = val;
        if (val > max) max = val;
        sum += val;
    }

    /* Small sort for P95 */
    for (uint32_t i = 0; i < st->count; i++) {
        for (uint32_t j = i + 1; j < st->count; j++) {
            if (temp[i] > temp[j]) {
                uint32_t t = temp[i];
                temp[i] = temp[j];
                temp[j] = t;
            }
        }
    }

    uint32_t p95_idx = (uint32_t)(st->count * 0.95f);
    if (p95_idx >= st->count && st->count > 0) p95_idx = st->count - 1;

    st->min_us = (st->count > 0) ? min : 0;
    st->max_us = max;
    st->avg_us = (st->count > 0) ? (uint32_t)(sum / st->count) : 0;
    st->p95_us = temp[p95_idx];

    /* Evaluate IPC fault thresholds */
    if (latency_us >= IPC_CRIT_LATENCY_US) {
        register_fault(g_sys.node_id, 2, FAULT_IPC_TIMEOUT, SEV_CRITICAL, "IPC Latency Critical (>50ms)");
    } else if (latency_us >= IPC_WARN_LATENCY_US) {
        register_fault(g_sys.node_id, 2, FAULT_IPC_TIMEOUT, SEV_WARNING, "IPC Latency Elevated (>1ms)");
    } else {
        clear_fault_type(2, FAULT_IPC_TIMEOUT);
    }
}

/* ============================================================================
 * WORKLOAD SERVICES (Node 1)
 * ============================================================================ */

/* Service A: Smart Traffic Controller (Period: 100ms, Deadline: 80ms) */
static void* service_a_thread(void* arg) {
    (void)arg;
    TaskControlBlock *t = &g_sys.tasks[0];

    while (g_sys.running) {
        uint64_t t_start = get_time_us();
        t->last_start_us = t_start;
        trace_record_event(t->task_id, EVENT_TASK_START, 0);

        /* Simulating traffic sensor analysis */
        uint32_t work_delay = 15; /* Base ~15ms */
        if (t->inject_exec_delay_ms > 0) {
            work_delay += t->inject_exec_delay_ms;
        }
        sleep_ms(work_delay);

        /* Write to Shared Memory IPC for Service B */
        RT_MUTEX_LOCK(&g_sys.ipc_mutex);
        if (!g_sys.ipc_channel.data_available) {
            g_sys.ipc_channel.sequence++;
            g_sys.ipc_channel.send_time_us = get_time_us();
            snprintf(g_sys.ipc_channel.payload, sizeof(g_sys.ipc_channel.payload), 
                     "TRAFFIC:GridSector_%u_Flow=%u", (t->heartbeat % 4) + 1, rand() % 100);
            g_sys.ipc_channel.data_available = true;
            RT_COND_SIGNAL(&g_sys.ipc_cond);
        }
        RT_MUTEX_UNLOCK(&g_sys.ipc_mutex);

        uint64_t t_end = get_time_us();
        t->last_end_us = t_end;
        uint32_t exec_us = (uint32_t)(t_end - t_start);
        t->last_exec_us = exec_us;
        t->total_exec_us += exec_us;
        t->total_cycles++;
        if (exec_us > t->max_exec_us) t->max_exec_us = exec_us;

        trace_record_event(t->task_id, EVENT_TASK_END, exec_us);

        if (!t->inject_starvation) {
            t->heartbeat++;
            trace_record_event(t->task_id, EVENT_HEARTBEAT, 0);
        }

        if (exec_us > (t->deadline_ms * 1000)) {
            t->deadline_misses++;
            register_fault(g_sys.node_id, t->task_id, FAULT_DEADLINE_MISS, SEV_WARNING, 
                           "Service A exceeded 80ms deadline");
        } else {
            clear_fault_type(t->task_id, FAULT_DEADLINE_MISS);
        }

        uint32_t elapsed_ms = (uint32_t)((get_time_us() - t_start) / 1000);
        if (elapsed_ms < t->period_ms) {
            sleep_ms(t->period_ms - elapsed_ms);
        }
    }
    return NULL;
}

/* Service B: Smart Power Grid / IPC Receiver (Period: 200ms, Deadline: 150ms) */
static void* service_b_thread(void* arg) {
    (void)arg;
    TaskControlBlock *t = &g_sys.tasks[1];

    while (g_sys.running) {
        uint64_t t_start = get_time_us();
        t->last_start_us = t_start;
        trace_record_event(t->task_id, EVENT_TASK_START, 0);

        /* Read Shared Memory IPC */
        RT_MUTEX_LOCK(&g_sys.ipc_mutex);
        if (g_sys.ipc_channel.data_available) {
            if (g_sys.ipc_channel.inject_delay_ms > 0) {
                sleep_ms(g_sys.ipc_channel.inject_delay_ms);
            }
            uint64_t t_recv = get_time_us();
            g_sys.ipc_channel.recv_time_us = t_recv;
            uint32_t lat_us = (uint32_t)(t_recv - g_sys.ipc_channel.send_time_us);
            g_sys.ipc_channel.latency_us = lat_us;
            g_sys.ipc_channel.data_available = false;

            ipc_record_latency(lat_us);
            trace_record_event(t->task_id, EVENT_IPC_XFER, lat_us);
        }
        RT_MUTEX_UNLOCK(&g_sys.ipc_mutex);

        /* Simulating Grid optimization computation */
        uint32_t work_delay = 25; /* Base ~25ms */
        if (t->inject_exec_delay_ms > 0) {
            work_delay += t->inject_exec_delay_ms;
        }
        sleep_ms(work_delay);

        uint64_t t_end = get_time_us();
        t->last_end_us = t_end;
        uint32_t exec_us = (uint32_t)(t_end - t_start);
        t->last_exec_us = exec_us;
        t->total_exec_us += exec_us;
        t->total_cycles++;
        if (exec_us > t->max_exec_us) t->max_exec_us = exec_us;

        trace_record_event(t->task_id, EVENT_TASK_END, exec_us);

        if (!t->inject_starvation) {
            t->heartbeat++;
            trace_record_event(t->task_id, EVENT_HEARTBEAT, 0);
        }

        if (exec_us > (t->deadline_ms * 1000)) {
            t->deadline_misses++;
            register_fault(g_sys.node_id, t->task_id, FAULT_DEADLINE_MISS, SEV_WARNING, 
                           "Service B exceeded 150ms deadline");
        } else {
            clear_fault_type(t->task_id, FAULT_DEADLINE_MISS);
        }

        uint32_t elapsed_ms = (uint32_t)((get_time_us() - t_start) / 1000);
        if (elapsed_ms < t->period_ms) {
            sleep_ms(t->period_ms - elapsed_ms);
        }
    }
    return NULL;
}

/* Service C: Environmental Monitoring (Period: 500ms, Deadline: 400ms) */
static void* service_c_thread(void* arg) {
    (void)arg;
    TaskControlBlock *t = &g_sys.tasks[2];

    while (g_sys.running) {
        uint64_t t_start = get_time_us();
        t->last_start_us = t_start;
        trace_record_event(t->task_id, EVENT_TASK_START, 0);

        uint32_t work_delay = 40; /* Base ~40ms */
        if (t->inject_exec_delay_ms > 0) {
            work_delay += t->inject_exec_delay_ms;
        }
        sleep_ms(work_delay);

        uint64_t t_end = get_time_us();
        t->last_end_us = t_end;
        uint32_t exec_us = (uint32_t)(t_end - t_start);
        t->last_exec_us = exec_us;
        t->total_exec_us += exec_us;
        t->total_cycles++;
        if (exec_us > t->max_exec_us) t->max_exec_us = exec_us;

        trace_record_event(t->task_id, EVENT_TASK_END, exec_us);

        if (!t->inject_starvation) {
            t->heartbeat++;
            trace_record_event(t->task_id, EVENT_HEARTBEAT, 0);
        }

        if (exec_us > (t->deadline_ms * 1000)) {
            t->deadline_misses++;
            register_fault(g_sys.node_id, t->task_id, FAULT_DEADLINE_MISS, SEV_WARNING, 
                           "Service C exceeded 400ms deadline");
        } else {
            clear_fault_type(t->task_id, FAULT_DEADLINE_MISS);
        }

        uint32_t elapsed_ms = (uint32_t)((get_time_us() - t_start) / 1000);
        if (elapsed_ms < t->period_ms) {
            sleep_ms(t->period_ms - elapsed_ms);
        }
    }
    return NULL;
}

/* Simulated CPU Burner Thread for Overload Fault Injection */
static void* cpu_burner_thread(void* arg) {
    (void)arg;
    volatile double dummy = 0.0;
    while (g_sys.running) {
        if (g_sys.inject_cpu_overload) {
            uint64_t start = get_time_us();
            while ((get_time_us() - start) < 80000ULL) {
                dummy += sin(dummy + 1.2345);
            }
            sleep_ms(20);
        } else {
            sleep_ms(200);
        }
    }
    return (void*)0;
}

/* ============================================================================
 * HIGH-PRIORITY MONITORING TASK (Node 1)
 * ============================================================================ */
static void* monitoring_task_thread(void* arg) {
    (void)arg;
    const uint32_t monitor_interval_ms = 100;
    uint64_t prev_check_time = get_time_us();

    while (g_sys.running) {
        sleep_ms(monitor_interval_ms);
        uint64_t now = get_time_us();
        uint64_t window_us = now - prev_check_time;
        prev_check_time = now;

        float total_work_us = 0.0f;

        for (int i = 0; i < MAX_TASKS; i++) {
            TaskControlBlock *t = &g_sys.tasks[i];

            /* Check heartbeat */
            if (t->heartbeat == t->prev_heartbeat) {
                t->missed_heartbeats++;
                if (t->missed_heartbeats >= HEARTBEAT_FAULT_CYCLES) {
                    t->state = TASK_STATE_STARVED;
                    char desc[64];
                    snprintf(desc, sizeof(desc), "Task '%s' Starvation (HB frozen at %u)", t->name, t->heartbeat);
                    register_fault(g_sys.node_id, t->task_id, FAULT_TASK_STARVATION, SEV_CRITICAL, desc);
                } else if (t->missed_heartbeats >= HEARTBEAT_WARN_CYCLES) {
                    t->state = TASK_STATE_WARNING;
                    char desc[64];
                    snprintf(desc, sizeof(desc), "Task '%s' HB Warning (%u missed)", t->name, t->missed_heartbeats);
                    register_fault(g_sys.node_id, t->task_id, FAULT_TASK_STARVATION, SEV_WARNING, desc);
                }
            } else {
                t->prev_heartbeat = t->heartbeat;
                t->missed_heartbeats = 0;
                if (t->state == TASK_STATE_STARVED || t->state == TASK_STATE_WARNING) {
                    t->state = TASK_STATE_RUNNING;
                    clear_fault_type(t->task_id, FAULT_TASK_STARVATION);
                }
            }

            if (window_us > 0) {
                float duty = ((float)t->last_exec_us / (float)(t->period_ms * 1000)) * 100.0f;
                t->cpu_usage_pct = (duty > 100.0f) ? 100.0f : duty;
                total_work_us += (t->cpu_usage_pct * 0.01f * (float)window_us);
            }
        }

        float base_cpu = (total_work_us / (float)window_us) * 100.0f;
        if (g_sys.inject_cpu_overload) {
            base_cpu += 60.0f;
        }
        if (base_cpu > 100.0f) base_cpu = 100.0f;
        g_sys.total_cpu_pct = base_cpu;

        if (g_sys.total_cpu_pct >= CPU_CRIT_THRESHOLD) {
            char desc[64];
            snprintf(desc, sizeof(desc), "CPU Overload Critical: %.1f%% (>%.0f%%)", 
                     g_sys.total_cpu_pct, CPU_CRIT_THRESHOLD);
            register_fault(g_sys.node_id, 0, FAULT_CPU_OVERLOAD, SEV_CRITICAL, desc);
        } else if (g_sys.total_cpu_pct >= CPU_WARN_THRESHOLD) {
            char desc[64];
            snprintf(desc, sizeof(desc), "CPU Elevated: %.1f%% (>%.0f%%)", 
                     g_sys.total_cpu_pct, CPU_WARN_THRESHOLD);
            register_fault(g_sys.node_id, 0, FAULT_CPU_OVERLOAD, SEV_WARNING, desc);
        } else {
            clear_fault_type(0, FAULT_CPU_OVERLOAD);
        }
    }
    return NULL;
}

/* ============================================================================
 * TELEMETRY PIPELINE (TCP Client / Server)
 * ============================================================================ */
static void build_telemetry_json(char* buf, size_t max_len) {
    snprintf(buf, max_len,
        "{\"node\":%u,\"timestamp\":" FMT_U64 ",\"cpu\":%.1f,\"led\":%d,"
        "\"ipc\":{\"min\":%u,\"max\":%u,\"avg\":%u,\"p95\":%u},"
        "\"tasks\":["
        "{\"id\":%u,\"name\":\"%s\",\"hb\":%u,\"exec_us\":%u,\"miss\":%u,\"state\":%d},"
        "{\"id\":%u,\"name\":\"%s\",\"hb\":%u,\"exec_us\":%u,\"miss\":%u,\"state\":%d},"
        "{\"id\":%u,\"name\":\"%s\",\"hb\":%u,\"exec_us\":%u,\"miss\":%u,\"state\":%d}"
        "]}\n",
        g_sys.node_id,
        (uint64_t)get_time_us(),
        g_sys.total_cpu_pct,
        (int)g_sys.led_state,
        g_sys.ipc_stats.min_us, g_sys.ipc_stats.max_us, g_sys.ipc_stats.avg_us, g_sys.ipc_stats.p95_us,
        g_sys.tasks[0].task_id, g_sys.tasks[0].name, g_sys.tasks[0].heartbeat, g_sys.tasks[0].last_exec_us, g_sys.tasks[0].deadline_misses, (int)g_sys.tasks[0].state,
        g_sys.tasks[1].task_id, g_sys.tasks[1].name, g_sys.tasks[1].heartbeat, g_sys.tasks[1].last_exec_us, g_sys.tasks[1].deadline_misses, (int)g_sys.tasks[1].state,
        g_sys.tasks[2].task_id, g_sys.tasks[2].name, g_sys.tasks[2].heartbeat, g_sys.tasks[2].last_exec_us, g_sys.tasks[2].deadline_misses, (int)g_sys.tasks[2].state
    );
}

/* Telemetry Sender Thread (Node 1 -> Node 2 via TCP) */
static void* telemetry_sender_thread(void* arg) {
    (void)arg;
    char buffer[1024];

    while (g_sys.running) {
        int sock = (int)socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            sleep_ms(1000);
            continue;
        }

        struct sockaddr_in serv_addr;
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons((uint16_t)g_sys.peer_port);
        serv_addr.sin_addr.s_addr = inet_addr(g_sys.peer_ip);

        printf("[Telemetry] Connecting to Supervisor at %s:%d...\n", g_sys.peer_ip, g_sys.peer_port);
        if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            close(sock);
            sleep_ms(2000);
            continue;
        }

        printf("[Telemetry] Connected to Supervisor successfully!\n");
        g_sys.peer_connected = true;

        while (g_sys.running) {
            build_telemetry_json(buffer, sizeof(buffer));
            int sent = send(sock, buffer, (int)strlen(buffer), 0);
            if (sent <= 0) {
                printf("[Telemetry] Connection to supervisor lost.\n");
                break;
            }
            sleep_ms(500);
        }

        g_sys.peer_connected = false;
        close(sock);
        sleep_ms(1000);
    }
    return NULL;
}

/* Telemetry Receiver Server Thread (Node 2 Supervisor) */
static void* telemetry_receiver_thread(void* arg) {
    (void)arg;
    int server_fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return NULL;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons((uint16_t)g_sys.peer_port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        close(server_fd);
        return NULL;
    }

    listen(server_fd, 2);
    printf("[Supervisor] Listening for Node 1 Telemetry on port %d...\n", g_sys.peer_port);

    while (g_sys.running) {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int new_socket = (int)accept(server_fd, (struct sockaddr*)&client_addr, &addrlen);
        if (new_socket < 0) continue;

        printf("[Supervisor] Connected to Node 1 Workload!\n");
        g_sys.peer_connected = true;
        clear_fault_type(1, FAULT_NODE_DISCONNECTED);

        char rx_buf[1024];
        while (g_sys.running) {
            int bytes = recv(new_socket, rx_buf, sizeof(rx_buf) - 1, 0);
            if (bytes <= 0) break;
            rx_buf[bytes] = '\0';
            g_sys.last_peer_contact_us = get_time_us();

            float remote_cpu = 0.0f;
            int led = 0;
            uint64_t dummy_ts = 0;
            if (sscanf(rx_buf, "{\"node\":%*u,\"timestamp\":" FMT_U64 ",\"cpu\":%f,\"led\":%d", &dummy_ts, &remote_cpu, &led) >= 2) {
                g_sys.total_cpu_pct = remote_cpu;
                hal_set_health_led((HealthLedState)led);
            }
        }

        printf("[Supervisor] Node 1 communication dropped!\n");
        g_sys.peer_connected = false;
        register_fault(1, 0, FAULT_NODE_DISCONNECTED, SEV_CRITICAL, "Node 1 Communication Disconnected");
        close(new_socket);
    }

    close(server_fd);
    return NULL;
}

/* ============================================================================
 * DIAGNOSTIC CLI & VISUALIZATION (GANTT, FAULT MAP, METRICS)
 * ============================================================================ */

static void cli_show_status(void) {
    printf("\n=======================================================\n");
    printf("         SMART CITY RTOS SYSTEM STATUS                 \n");
    printf("=======================================================\n");
    printf(" Node Mode        : %s\n", 
           (g_sys.mode == NODE_MODE_STANDALONE) ? "STANDALONE (Integrated)" :
           (g_sys.mode == NODE_MODE_WORKLOAD)   ? "NODE 1 (Workload Node)" : "NODE 2 (Supervisor)");
    printf(" Health Indicator : %s\n", hal_get_led_string());
    printf(" Total CPU Load   : %5.1f %%\n", g_sys.total_cpu_pct);
    printf(" Peer Link        : %s\n", g_sys.peer_connected ? "\033[32mCONNECTED\033[0m" : "\033[31mDISCONNECTED\033[0m");
    printf(" Active Faults    : %u\n", g_sys.fault_counter);
    printf(" Monotonic Time   : " FMT_U64 " ms\n", (uint64_t)(get_time_us() / 1000));
    printf("=======================================================\n");
}

static void cli_show_tasks(void) {
    printf("\n%-4s %-20s %-10s %-8s %-12s %-12s %-10s %-6s\n",
           "ID", "NAME", "STATE", "PRIORITY", "PERIOD", "DEADLINE", "EXEC_US", "HB");
    printf("----------------------------------------------------------------------------------------\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        TaskControlBlock *t = &g_sys.tasks[i];
        const char *state_str = "RUNNING";
        if (t->state == TASK_STATE_STARVED) state_str = "\033[31mSTARVED\033[0m";
        else if (t->state == TASK_STATE_WARNING) state_str = "\033[33mWARNING\033[0m";

        printf("%-4u %-20s %-10s %-8u %-4ums      %-4ums       %-10u %-6u\n",
               t->task_id, t->name, state_str, t->priority,
               t->period_ms, t->deadline_ms, t->last_exec_us, t->heartbeat);
    }
    printf("----------------------------------------------------------------------------------------\n");
}

static void cli_show_cpu(void) {
    printf("\n--- CPU UTILIZATION METRICS ---\n");
    printf(" Total CPU: %5.1f %%\n", g_sys.total_cpu_pct);
    for (int i = 0; i < MAX_TASKS; i++) {
        TaskControlBlock *t = &g_sys.tasks[i];
        printf("  * %-20s : %5.1f %% (Exec: %u us / Max: %u us)\n", 
               t->name, t->cpu_usage_pct, t->last_exec_us, t->max_exec_us);
    }
    printf(" Overload Injected: %s\n", g_sys.inject_cpu_overload ? "YES" : "NO");
}

static void cli_show_ipc(void) {
    IpcStats *st = &g_sys.ipc_stats;
    printf("\n--- SHARED MEMORY IPC LATENCY STATS ---\n");
    printf(" Sample Count  : %u\n", st->count);
    printf(" Minimum       : %u us\n", st->min_us);
    printf(" Maximum       : %u us\n", st->max_us);
    printf(" Average       : %u us\n", st->avg_us);
    printf(" 95th Pctile   : %u us\n", st->p95_us);
    printf(" Injected Delay: %u ms\n", g_sys.ipc_channel.inject_delay_ms);
}

static void cli_show_fault_map(void) {
    printf("\n================ FAULT MAP ================\n");
    bool found = false;
    for (int i = 0; i < MAX_ACTIVE_FAULTS; i++) {
        FaultRecord *f = &g_sys.fault_map[i];
        if (f->active) {
            found = true;
            const char* sev_str = "\033[33mWARN\033[0m";
            if (f->severity == SEV_CRITICAL) sev_str = "\033[31mCRIT\033[0m";

            printf("[%s] ID:%u Node:%u Task:%u | %s (at " FMT_U64 " ms)\n",
                   sev_str, f->fault_id, f->node_id, f->task_id, 
                   f->description, (uint64_t)(f->timestamp_us / 1000));
        }
    }
    if (!found) {
        printf(" No active faults detected. System healthy.\n");
    }
    printf("===========================================\n");
}

static void cli_show_gantt(void) {
    printf("\n--- TASK EXECUTION TIMELINE (GANTT) ---\n");
    RT_MUTEX_LOCK(&g_sys.trace_buf.lock);
    int count = (g_sys.trace_buf.count > 20) ? 20 : g_sys.trace_buf.count;
    int start_idx = (g_sys.trace_buf.head - count + TRACE_BUFFER_SIZE) % TRACE_BUFFER_SIZE;

    printf("Timeline (Most recent %d events):\n", count);
    for (int i = 0; i < count; i++) {
        int idx = (start_idx + i) % TRACE_BUFFER_SIZE;
        TraceRecord *r = &g_sys.trace_buf.records[idx];

        const char *tname = "Task_?";
        if (r->task_id == 1) tname = "Service_A";
        else if (r->task_id == 2) tname = "Service_B";
        else if (r->task_id == 3) tname = "Service_C";

        const char *ev_str = "EVENT";
        if (r->event_type == EVENT_TASK_START) ev_str = "START";
        else if (r->event_type == EVENT_TASK_END)   ev_str = "END  ";
        else if (r->event_type == EVENT_HEARTBEAT)  ev_str = "HB   ";
        else if (r->event_type == EVENT_FAULT)      ev_str = "\033[31mFAULT\033[0m";
        else if (r->event_type == EVENT_IPC_XFER)   ev_str = "IPC  ";

        printf("  +%06lu us | %-10s | %-5s | Dur: %5u us\n",
               (unsigned long)(r->timestamp_us % 10000000ULL), tname, ev_str, r->duration_us);
    }
    RT_MUTEX_UNLOCK(&g_sys.trace_buf.lock);
}

static void cli_help(void) {
    printf("\nAvailable Diagnostic Commands:\n");
    printf("  status                     - Global system health & LED indicator\n");
    printf("  tasks                      - List tasks with execution times & deadlines\n");
    printf("  cpu                        - CPU utilization per service and overall\n");
    printf("  ipc                        - Shared memory IPC latency metrics (Min/Max/Avg/P95)\n");
    printf("  faults / faultmap          - Display active fault hierarchy\n");
    printf("  gantt / trace              - Show task execution timeline\n");
    printf("  inject starve <1-3>        - Freeze heartbeat of task (Starvation)\n");
    printf("  inject deadline <1-3> <ms> - Artificially inflate execution time\n");
    printf("  inject cpu <on|off>        - Toggle CPU burner overload\n");
    printf("  inject ipc <delay_ms>      - Inject latency into IPC channel\n");
    printf("  clear                      - Clear all faults & restore healthy state\n");
    printf("  help                       - Show available commands\n");
    printf("  exit                       - Terminate monitoring platform\n\n");
}

static void* cli_thread(void* arg) {
    (void)arg;
    char line[128];

    printf("\n");
    printf("****************************************************************\n");
    printf("* Smart City RTOS Fault & Performance Supervisory Terminal     *\n");
    printf("* Type 'help' for available diagnostic commands.               *\n");
    printf("****************************************************************\n");

    while (g_sys.running) {
        printf("monitor> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\r\n")] = 0;

        if (strlen(line) == 0) continue;

        if (strcmp(line, "help") == 0) {
            cli_help();
        } else if (strcmp(line, "status") == 0) {
            cli_show_status();
        } else if (strcmp(line, "tasks") == 0) {
            cli_show_tasks();
        } else if (strcmp(line, "cpu") == 0) {
            cli_show_cpu();
        } else if (strcmp(line, "ipc") == 0) {
            cli_show_ipc();
        } else if (strcmp(line, "faults") == 0 || strcmp(line, "faultmap") == 0) {
            cli_show_fault_map();
        } else if (strcmp(line, "gantt") == 0 || strcmp(line, "trace") == 0) {
            cli_show_gantt();
        } else if (strcmp(line, "clear") == 0) {
            clear_all_faults();
            for (int i = 0; i < MAX_TASKS; i++) {
                g_sys.tasks[i].inject_starvation = false;
                g_sys.tasks[i].inject_exec_delay_ms = 0;
            }
            g_sys.inject_cpu_overload = false;
            g_sys.ipc_channel.inject_delay_ms = 0;
            printf("[CLI] All faults cleared and normal state restored.\n");
        } else if (strncmp(line, "inject starve ", 14) == 0) {
            int tid = atoi(line + 14);
            if (tid >= 1 && tid <= MAX_TASKS) {
                g_sys.tasks[tid - 1].inject_starvation = true;
                printf("[Fault Injection] Heartbeat freeze injected for Task %u (%s).\n", 
                       tid, g_sys.tasks[tid - 1].name);
            } else {
                printf("Invalid task id (1-%d)\n", MAX_TASKS);
            }
        } else if (strncmp(line, "inject deadline ", 16) == 0) {
            int tid = 0, delay = 0;
            if (sscanf(line + 16, "%d %d", &tid, &delay) == 2 && tid >= 1 && tid <= MAX_TASKS) {
                g_sys.tasks[tid - 1].inject_exec_delay_ms = delay;
                printf("[Fault Injection] Injected %d ms delay into Task %u (%s).\n", 
                       delay, tid, g_sys.tasks[tid - 1].name);
            } else {
                printf("Usage: inject deadline <task_id 1-3> <delay_ms>\n");
            }
        } else if (strncmp(line, "inject cpu ", 11) == 0) {
            if (strcmp(line + 11, "on") == 0) {
                g_sys.inject_cpu_overload = true;
                printf("[Fault Injection] CPU Overload burner ACTIVE.\n");
            } else {
                g_sys.inject_cpu_overload = false;
                printf("[Fault Injection] CPU Overload burner DEACTIVATED.\n");
            }
        } else if (strncmp(line, "inject ipc ", 11) == 0) {
            int delay = atoi(line + 11);
            g_sys.ipc_channel.inject_delay_ms = delay;
            printf("[Fault Injection] IPC delay set to %d ms.\n", delay);
        } else if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
            g_sys.running = false;
            break;
        } else {
            printf("Unknown command '%s'. Type 'help' for options.\n", line);
        }
    }
    return NULL;
}

/* ============================================================================
 * INITIALIZATION & MAIN ENTRY POINT
 * ============================================================================ */
static void init_system(NodeMode mode, const char* peer_ip, int peer_port) {
    memset(&g_sys, 0, sizeof(g_sys));
    g_sys.mode = mode;
    g_sys.running = true;
    g_sys.node_id = (mode == NODE_MODE_SUPERVISOR) ? 2 : 1;
    g_sys.led_state = LED_GREEN;
    g_sys.peer_port = peer_port ? peer_port : DEFAULT_PORT;
    if (peer_ip) strncpy(g_sys.peer_ip, peer_ip, sizeof(g_sys.peer_ip) - 1);
    else strcpy(g_sys.peer_ip, "127.0.0.1");

    RT_MUTEX_INIT(&g_sys.task_mutex);
    RT_MUTEX_INIT(&g_sys.ipc_mutex);
    RT_MUTEX_INIT(&g_sys.fault_mutex);
    RT_MUTEX_INIT(&g_sys.trace_buf.lock);
    RT_COND_INIT(&g_sys.ipc_cond);

    /* Task TCBs  */
    /* Service A */
    g_sys.tasks[0].task_id = 1;
    strncpy(g_sys.tasks[0].name, "Service_A (Traffic)", sizeof(g_sys.tasks[0].name) - 1);
    g_sys.tasks[0].period_ms = 100;
    g_sys.tasks[0].deadline_ms = 80;
    g_sys.tasks[0].priority = PRIORITY_SERVICE_A;
    g_sys.tasks[0].state = TASK_STATE_RUNNING;

    /* Service B */
    g_sys.tasks[1].task_id = 2;
    strncpy(g_sys.tasks[1].name, "Service_B (Grid)", sizeof(g_sys.tasks[1].name) - 1);
    g_sys.tasks[1].period_ms = 200;
    g_sys.tasks[1].deadline_ms = 150;
    g_sys.tasks[1].priority = PRIORITY_SERVICE_B;
    g_sys.tasks[1].state = TASK_STATE_RUNNING;

    /* Service C */
    g_sys.tasks[2].task_id = 3;
    strncpy(g_sys.tasks[2].name, "Service_C (Env)", sizeof(g_sys.tasks[2].name) - 1);
    g_sys.tasks[2].period_ms = 500;
    g_sys.tasks[2].deadline_ms = 400;
    g_sys.tasks[2].priority = PRIORITY_SERVICE_C;
    g_sys.tasks[2].state = TASK_STATE_RUNNING;
}

int main(int argc, char* argv[]) {
    NodeMode mode = NODE_MODE_STANDALONE;
    const char *peer_ip = "127.0.0.1";
    int peer_port = DEFAULT_PORT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--workload") == 0 || strcmp(argv[i], "--node1") == 0) {
            mode = NODE_MODE_WORKLOAD;
        } else if (strcmp(argv[i], "--supervisor") == 0 || strcmp(argv[i], "--node2") == 0) {
            mode = NODE_MODE_SUPERVISOR;
        } else if (strcmp(argv[i], "--standalone") == 0) {
            mode = NODE_MODE_STANDALONE;
        } else if (strncmp(argv[i], "--ip=", 5) == 0) {
            peer_ip = argv[i] + 5;
        } else if (strncmp(argv[i], "--port=", 7) == 0) {
            peer_port = atoi(argv[i] + 7);
        } else {
            printf("Usage: %s [--standalone | --workload | --supervisor] [--ip=x.x.x.x] [--port=5555]\n", argv[0]);
            return 0;
        }
    }


    init_system(mode, peer_ip, peer_port);

    printf("===============================================================\n");
    printf(" Starting Smart City RTOS Platform (%s)\n", PLATFORM_NAME);
    printf(" Mode: %s\n", (mode == NODE_MODE_STANDALONE) ? "STANDALONE" :
                          (mode == NODE_MODE_WORKLOAD) ? "WORKLOAD (Pi 1)" : "SUPERVISOR (Pi 2)");
    printf("===============================================================\n");

    rt_thread_t th_sa, th_sb, th_sc, th_mon, th_burn, th_net, th_cli;

    if (mode == NODE_MODE_WORKLOAD || mode == NODE_MODE_STANDALONE) {
        rt_thread_create(&th_sa, PRIORITY_SERVICE_A, service_a_thread, NULL);
        rt_thread_create(&th_sb, PRIORITY_SERVICE_B, service_b_thread, NULL);
        rt_thread_create(&th_sc, PRIORITY_SERVICE_C, service_c_thread, NULL);
        rt_thread_create(&th_mon, PRIORITY_MONITOR, monitoring_task_thread, NULL);
        rt_thread_create(&th_burn, PRIORITY_CLI, cpu_burner_thread, NULL);
    }

    if (mode == NODE_MODE_WORKLOAD) {
        rt_thread_create(&th_net, PRIORITY_TELEMETRY, telemetry_sender_thread, NULL);
    } else if (mode == NODE_MODE_SUPERVISOR) {
        rt_thread_create(&th_net, PRIORITY_TELEMETRY, telemetry_receiver_thread, NULL);
    }

    /* Start CLI diagnostics console */
    rt_thread_create(&th_cli, PRIORITY_CLI, cli_thread, NULL);

    /* Wait for CLI exit */
    rt_thread_join(th_cli);

    printf("[System] Terminating all services and cleaning up...\n");
    g_sys.running = false;
    sleep_ms(500);

    return 0;
}
