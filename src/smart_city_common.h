/**
 * ============================================================================
 * smart_city_common.h
 * Smart City RTOS Fault & Performance Monitoring Platform
 *
 * Shared definitions between:
 *   node1.c       - Raspberry Pi 1 (Monitored Workload Node)
 *   supervisor.c  - Raspberry Pi 2 (Supervisor / Analytics Node)
 *
 * Target: Raspberry Pi 4
 * ============================================================================
 */

#ifndef SMART_CITY_COMMON_H
#define SMART_CITY_COMMON_H

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/time.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <inttypes.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>

#if defined(__QNX__) || defined(__QNXNTO__)
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <devctl.h>
#include <sys/procfs.h>
#endif

/* 64-bit printf format — use PRIu64 from inttypes.h for portability */
#define FMT_U64 "%" PRIu64

/* ============================================================================
 * CONFIGURATION & NETWORK
 * ============================================================================ */
#define PLATFORM_NAME          "Smart City RTOS Fault & Performance Monitoring Platform"
#define TELEMETRY_PORT         5555        /* TCP port Pi1 -> Pi2 */
#define TELEMETRY_INTERVAL_MS  500         /* Send every 500 ms */
#define MONITOR_INTERVAL_MS    100         /* Monitoring task period */
#define MAX_TASKS              3
#define TRACE_BUFFER_SIZE      256
#define IPC_HISTORY_SIZE       100
#define MAX_ACTIVE_FAULTS      16

/* ============================================================================
 * REAL-TIME PRIORITY HIERARCHY 
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
 * FAULT THRESHOLDS 
 * ============================================================================ */
#define CPU_WARN_THRESHOLD       70.0f    /* % */
#define CPU_CRIT_THRESHOLD       85.0f    /* % */
#define IPC_WARN_LATENCY_US      220000U   /* 220 ms (> 200 ms Service B period) */
#define IPC_CRIT_LATENCY_US      300000U   /* 300 ms (critical queue stall)      */

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
 * POSIX / QNX SHARED MEMORY METRICS (shm_open + mmap)
 * Requirement: Shared memory for high-frequency metrics (CPU%, IPC latency)
 * ============================================================================ */
#define SHM_METRICS_NAME  "/smart_city_metrics"

typedef struct {
    uint64_t         timestamp_us;
    float            total_cpu_pct;
    IpcStats         ipc_stats;
    IpcChannel       ipc_channel;
    TaskControlBlock tasks[MAX_TASKS];
} SharedMetricsBlock;

static inline SharedMetricsBlock* rt_shm_create(int *out_fd) {
    int fd = shm_open(SHM_METRICS_NAME, O_RDWR | O_CREAT, 0666);
    if (fd < 0) return NULL;

    if (ftruncate(fd, sizeof(SharedMetricsBlock)) != 0) {
        close(fd);
        shm_unlink(SHM_METRICS_NAME);
        return NULL;
    }

    void *ptr = mmap(NULL, sizeof(SharedMetricsBlock), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        close(fd);
        shm_unlink(SHM_METRICS_NAME);
        return NULL;
    }

    if (out_fd) *out_fd = fd;
    return (SharedMetricsBlock*)ptr;
}

static inline SharedMetricsBlock* rt_shm_attach(int *out_fd) {
    int fd = shm_open(SHM_METRICS_NAME, O_RDONLY, 0444);
    if (fd < 0) return NULL;

    void *ptr = mmap(NULL, sizeof(SharedMetricsBlock), PROT_READ, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    if (out_fd) *out_fd = fd;
    return (SharedMetricsBlock*)ptr;
}

static inline void rt_shm_destroy(SharedMetricsBlock *shm, int fd) {
    if (shm && shm != MAP_FAILED) {
        munmap(shm, sizeof(SharedMetricsBlock));
    }
    if (fd >= 0) {
        close(fd);
    }
    shm_unlink(SHM_METRICS_NAME);
}

/* ============================================================================
 * QNX NATIVE MESSAGE PASSING HEARTBEAT (MsgSend / MsgReceive)
 * Requirement: Tasks send MsgSend() heartbeats to Fault Detector Channel
 * ============================================================================ */
#define MSG_TYPE_HEARTBEAT      0x01

typedef struct {
    uint16_t type;              /* MSG_TYPE_HEARTBEAT */
    uint32_t task_id;           /* Task ID (1, 2, 3) */
    uint64_t timestamp_us;      /* Microsecond timestamp */
} HeartbeatMsg;

typedef struct {
    uint16_t status;            /* 0 = OK */
} HeartbeatReply;

#if defined(__QNX__) || defined(__QNXNTO__)

static inline int rt_channel_create(int flags) {
    return ChannelCreate(flags);
}
static inline int rt_connect_attach(uint32_t nd, pid_t pid, int chid, unsigned index, int flags) {
    return ConnectAttach(nd, pid, chid, index, flags);
}
static inline int rt_msg_send(int coid, const void *smsg, size_t sbytes, void *rmsg, size_t rbytes) {
    return MsgSend(coid, smsg, (int)sbytes, rmsg, (int)rbytes);
}
static inline int rt_msg_receive(int chid, void *rmsg, size_t rbytes, void *info) {
    return MsgReceive(chid, rmsg, (int)rbytes, (struct _msg_info*)info);
}
static inline int rt_msg_reply(int rcvid, int status, const void *rmsg, size_t rbytes) {
    return MsgReply(rcvid, status, rmsg, (int)rbytes);
}
static inline int rt_connect_detach(int coid) {
    return ConnectDetach(coid);
}
static inline int rt_channel_destroy(int chid) {
    return ChannelDestroy(chid);
}

#else

/* Portable POSIX Simulation Wrapper for Non-QNX Test Builds */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  has_msg;
    pthread_cond_t  has_reply;
    bool            msg_pending;
    bool            reply_pending;
    HeartbeatMsg    msg;
    HeartbeatReply  reply;
    int             reply_status;
    bool            active;
} _posix_msg_ipc_t;

static _posix_msg_ipc_t _g_msg_ipc;

static inline int rt_channel_create(int flags) {
    (void)flags;
    pthread_mutex_init(&_g_msg_ipc.lock, NULL);
    pthread_cond_init(&_g_msg_ipc.has_msg, NULL);
    pthread_cond_init(&_g_msg_ipc.has_reply, NULL);
    _g_msg_ipc.msg_pending = false;
    _g_msg_ipc.reply_pending = false;
    _g_msg_ipc.active = true;
    return 1;
}
static inline int rt_connect_attach(uint32_t nd, pid_t pid, int chid, unsigned index, int flags) {
    (void)nd; (void)pid; (void)chid; (void)index; (void)flags;
    return 1;
}
static inline int rt_msg_send(int coid, const void *smsg, size_t sbytes, void *rmsg, size_t rbytes) {
    (void)coid;
    pthread_mutex_lock(&_g_msg_ipc.lock);
    if (!_g_msg_ipc.active) { pthread_mutex_unlock(&_g_msg_ipc.lock); return -1; }
    memcpy(&_g_msg_ipc.msg, smsg, (sbytes > sizeof(HeartbeatMsg)) ? sizeof(HeartbeatMsg) : sbytes);
    _g_msg_ipc.msg_pending = true;
    _g_msg_ipc.reply_pending = false;
    pthread_cond_signal(&_g_msg_ipc.has_msg);
    while (!_g_msg_ipc.reply_pending && _g_msg_ipc.active) {
        pthread_cond_wait(&_g_msg_ipc.has_reply, &_g_msg_ipc.lock);
    }
    if (rmsg && rbytes > 0) {
        memcpy(rmsg, &_g_msg_ipc.reply, (rbytes > sizeof(HeartbeatReply)) ? sizeof(HeartbeatReply) : rbytes);
    }
    int status = _g_msg_ipc.reply_status;
    pthread_mutex_unlock(&_g_msg_ipc.lock);
    return status;
}
static inline int rt_msg_receive(int chid, void *rmsg, size_t rbytes, void *info) {
    (void)chid; (void)info;
    pthread_mutex_lock(&_g_msg_ipc.lock);
    while (!_g_msg_ipc.msg_pending && _g_msg_ipc.active) {
        pthread_cond_wait(&_g_msg_ipc.has_msg, &_g_msg_ipc.lock);
    }
    if (!_g_msg_ipc.active) { pthread_mutex_unlock(&_g_msg_ipc.lock); return -1; }
    if (rmsg && rbytes > 0) {
        memcpy(rmsg, &_g_msg_ipc.msg, (rbytes > sizeof(HeartbeatMsg)) ? sizeof(HeartbeatMsg) : rbytes);
    }
    _g_msg_ipc.msg_pending = false;
    pthread_mutex_unlock(&_g_msg_ipc.lock);
    return 1;
}
static inline int rt_msg_reply(int rcvid, int status, const void *rmsg, size_t rbytes) {
    (void)rcvid;
    pthread_mutex_lock(&_g_msg_ipc.lock);
    if (rmsg && rbytes > 0) {
        memcpy(&_g_msg_ipc.reply, rmsg, (rbytes > sizeof(HeartbeatReply)) ? sizeof(HeartbeatReply) : rbytes);
    }
    _g_msg_ipc.reply_status = status;
    _g_msg_ipc.reply_pending = true;
    pthread_cond_signal(&_g_msg_ipc.has_reply);
    pthread_mutex_unlock(&_g_msg_ipc.lock);
    return 0;
}
static inline int rt_connect_detach(int coid) { (void)coid; return 0; }
static inline int rt_channel_destroy(int chid) {
    (void)chid;
    pthread_mutex_lock(&_g_msg_ipc.lock);
    _g_msg_ipc.active = false;
    pthread_cond_broadcast(&_g_msg_ipc.has_msg);
    pthread_cond_broadcast(&_g_msg_ipc.has_reply);
    pthread_mutex_unlock(&_g_msg_ipc.lock);
    return 0;
}
#endif

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
 * HIGH-RESOLUTION TIMING & QNX PROCFS / DEVCTL SAMPLING (Requirement A)
 * Uses ClockCycles() / clock_gettime() and /proc/<pid>/as devctl() calls
 * ============================================================================ */
static inline uint64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000ULL) + ((uint64_t)ts.tv_nsec / 1000ULL);
}

static inline uint64_t get_clock_cycles(void) {
#if defined(__QNX__) || defined(__QNXNTO__)
    return ClockCycles();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
#endif
}

static inline void sleep_ms(uint32_t ms) {
    struct timespec req = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&req, NULL);
}

typedef struct {
    uint32_t num_threads;
    uint64_t utime_ns;
    uint64_t stime_ns;
    uint64_t cpu_time_ns;
    int32_t  process_state;
    float    sampled_cpu_pct;
} QnxProcSample;

static inline int qnx_procfs_sample_cpu(pid_t pid, QnxProcSample *sample) {
    if (!sample) return -1;
    memset(sample, 0, sizeof(QnxProcSample));

#if defined(__QNX__) || defined(__QNXNTO__)
    char path[64];
    if (pid <= 0) pid = getpid();
    snprintf(path, sizeof(path), "/proc/%d/as", (int)pid);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    procfs_info pinfo;
    if (devctl(fd, DCMD_PROC_INFO, &pinfo, sizeof(pinfo), NULL) == 0) {
        sample->num_threads = pinfo.num_threads;
        sample->utime_ns = (uint64_t)pinfo.utime;
        sample->stime_ns = (uint64_t)pinfo.stime;
        sample->cpu_time_ns = sample->utime_ns + sample->stime_ns;
        sample->process_state = pinfo.flags;
    }

    procfs_status pstatus;
    pstatus.tid = 1;
    if (devctl(fd, DCMD_PROC_STATUS, &pstatus, sizeof(pstatus), NULL) == 0) {
        sample->process_state = pstatus.state;
    }

    close(fd);
    return 0;
#else
    (void)pid;
    sample->num_threads = 8;
    sample->process_state = 1;
    return 0;
#endif
}

/* ============================================================================
 * REAL-TIME THREAD ABSTRACTION (QNX / POSIX)
 * ============================================================================ */
typedef void* (*thread_func_t)(void*);

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

/* ============================================================================
 * QNX / POSIX HARDWARE INTERVAL TIMERS (Requirement D)
 * Uses timer_create() and timer_settime() with CLOCK_MONOTONIC and signals
 * (eliminates timing drift and busy-polling)
 * ============================================================================ */
#ifndef SIGRTMIN
#define SIGRTMIN 34
#endif

#define SIG_TIMER_SERVICE_A   (SIGRTMIN + 1)
#define SIG_TIMER_SERVICE_B   (SIGRTMIN + 2)
#define SIG_TIMER_SERVICE_C   (SIGRTMIN + 3)
#define SIG_TIMER_MONITOR     (SIGRTMIN + 4)
#define SIG_TIMER_SUPERVISOR  (SIGRTMIN + 5)

typedef struct {
    timer_t   timer_id;
    sigset_t  sig_set;
    int       sig_no;
    bool      active;
} rt_periodic_timer_t;

static inline void rt_timers_block_signals(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIG_TIMER_SERVICE_A);
    sigaddset(&set, SIG_TIMER_SERVICE_B);
    sigaddset(&set, SIG_TIMER_SERVICE_C);
    sigaddset(&set, SIG_TIMER_MONITOR);
    sigaddset(&set, SIG_TIMER_SUPERVISOR);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}

static inline int rt_timer_create(rt_periodic_timer_t *t, int sig_no, uint32_t period_ms) {
    if (!t) return -1;
    t->sig_no = sig_no;
    t->active = false;

    sigemptyset(&t->sig_set);
    sigaddset(&t->sig_set, sig_no);
    pthread_sigmask(SIG_BLOCK, &t->sig_set, NULL);

    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify          = SIGEV_SIGNAL;
    sev.sigev_signo           = sig_no;
    sev.sigev_value.sival_ptr = &t->timer_id;

    if (timer_create(CLOCK_MONOTONIC, &sev, &t->timer_id) != 0) {
        return -1;
    }

    struct itimerspec its;
    its.it_value.tv_sec     = (time_t)(period_ms / 1000);
    its.it_value.tv_nsec    = (long)(period_ms % 1000) * 1000000L;
    its.it_interval.tv_sec  = its.it_value.tv_sec;
    its.it_interval.tv_nsec = its.it_value.tv_nsec;

    if (timer_settime(t->timer_id, 0, &its, NULL) != 0) {
        timer_delete(t->timer_id);
        return -1;
    }
    t->active = true;
    return 0;
}

static inline void rt_timer_wait(rt_periodic_timer_t *t) {
    if (!t || !t->active) return;
    int sig;
    sigwait(&t->sig_set, &sig);
}

static inline void rt_timer_destroy(rt_periodic_timer_t *t) {
    if (t && t->active) {
        timer_delete(t->timer_id);
        t->active = false;
    }
}

#endif /* SMART_CITY_COMMON_H */
