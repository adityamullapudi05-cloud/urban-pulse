/**
 * ============================================================================
 * node1_workload.c  —  Raspberry Pi 1  (Monitored Workload Node)
 * Smart City RTOS Fault & Performance Monitoring Platform
 *
 * Build in QNX Momentics IDE  →  Deploy to Raspberry Pi 1
 * Run:  ./node1_workload --ip=<Pi2_IP>
 *
 * What this node does (design.md §2, §3, §4):
 *   - Runs 3 periodic city services with configurable periods/deadlines
 *   - Shared-memory IPC between Service A (producer) and Service B (consumer)
 *   - High-priority Monitoring Task samples heartbeat, deadline, CPU
 *   - Trace Collector ring buffer records every START / END / HB / FAULT event
 *   - TCP Telemetry Sender pushes JSON telemetry to Pi 2 every 500 ms
 *   - Fault Injection subsystem: starvation, deadline, CPU overload, IPC lag
 *   - GPIO Physical Interrupt Buttons → hardware-triggered fault injection
 * ============================================================================
 */

#include "smart_city_common.h"

/* Forward declarations from hal_gpio.c */
extern int   hal_gpio_init(void);
extern void  hal_gpio_set_led(HealthLedState state);
extern void* hal_gpio_poll_thread(void *arg);
extern void  hal_gpio_deinit(void);

/* ============================================================================
 * NODE 1 GLOBAL CONTEXT
 * ============================================================================ */
typedef struct {
    volatile bool running;
    uint32_t      node_id;         /* Always 1 */
    float         total_cpu_pct;

    /* Three periodic services */
    TaskControlBlock tasks[MAX_TASKS];
    rt_mutex_t       task_mutex;

    /* Shared-memory IPC channel (Service A -> Service B) */
    IpcChannel   ipc_channel;
    IpcStats     ipc_stats;
    rt_mutex_t   ipc_mutex;
    rt_cond_t    ipc_cond;

    /* Fault map & LED state */
    FaultRecord  fault_map[MAX_ACTIVE_FAULTS];
    uint32_t     fault_counter;
    rt_mutex_t   fault_mutex;
    HealthLedState led_state;

    /* Trace ring buffer */
    TraceRingBuffer trace_buf;
    rt_mutex_t      trace_mutex;

    /* Fault injection controls */
    bool     inject_cpu_overload;

    /* TCP client */
    char     supervisor_ip[64];
    int      supervisor_port;
    bool     tcp_connected;
} Node1Context;

static Node1Context g;

/* ============================================================================
 * HAL — GPIO LED wrapper (delegates to hal_gpio.c real hardware)
 * ============================================================================ */
static void hal_set_led(HealthLedState state) {
    g.led_state = state;
    hal_gpio_set_led(state);   /* Drives BCM2711 GPIO registers on QNX */
}

/* ============================================================================
 * GPIO LEVEL-BASED CALLBACK  —  NODE 1 (Workload Pi)
 * Called by hal_gpio_poll_thread() whenever a button pin CHANGES level.
 *
 * BEHAVIOR:
 *   pressed = true  → PIN went HIGH→LOW (GND contact held)  → Inject Fault
 *   pressed = false → PIN went LOW→HIGH (GND released)      → Clear, NORMAL
 *
 * Button Map (3 buttons, no clear button needed):
 *   0 = GPIO 23 (Pin 16) → Service B STARVATION  while held
 *   1 = GPIO 24 (Pin 18) → CPU OVERLOAD          while held
 *   2 = GPIO 25 (Pin 22) → Service A DEADLINE MISS while held
 * ============================================================================ */
static void gpio_button_callback(int btn_id, bool pressed) {
    switch (btn_id) {

        case 0:  /* GPIO 23 — Service B Task Starvation */
            g.tasks[1].inject_starvation = pressed;
            if (pressed) {
                printf("\n[GPIO] Pin 16 → GND HELD   : STARVATION active on Service_B\n");
                printf("                               Heartbeat frozen — fault will appear in ~400ms\n");
            } else {
                printf("\n[GPIO] Pin 16 → RELEASED   : Service_B heartbeat RESTORED — returning to NORMAL\n");
                /* Manually reset last heartbeat time so monitoring task
                   does not immediately re-trigger starvation on release */
                g.tasks[1].last_heartbeat_time_us = get_time_us();
            }
            printf("node1> "); fflush(stdout);
            break;

        case 1:  /* GPIO 24 — CPU Overload Burner */
            g.inject_cpu_overload = pressed;
            if (pressed) {
                printf("\n[GPIO] Pin 18 → GND HELD   : CPU OVERLOAD burner ACTIVE\n");
                printf("                               CPU will exceed 85%% threshold while contact is held\n");
            } else {
                printf("\n[GPIO] Pin 18 → RELEASED   : CPU burner OFF — CPU returning to NORMAL\n");
            }
            printf("node1> "); fflush(stdout);
            break;

        case 2:  /* GPIO 25 — Service A Deadline Miss */
            g.tasks[0].inject_exec_delay_ms = pressed ? 100 : 0;
            if (pressed) {
                printf("\n[GPIO] Pin 22 → GND HELD   : DEADLINE MISS active on Service_A\n");
                printf("                               +100ms delay added → exceeds 80ms deadline\n");
            } else {
                printf("\n[GPIO] Pin 22 → RELEASED   : Service_A deadline delay REMOVED — NORMAL\n");
            }
            printf("node1> "); fflush(stdout);
            break;

        default: break;
    }
}

static const char* hal_led_str(void) {
    switch (g.led_state) {
        case LED_GREEN:  return "\033[1;32m[● GREEN  - HEALTHY]\033[0m";
        case LED_YELLOW: return "\033[1;33m[▲ YELLOW - WARNING]\033[0m";
        case LED_RED:    return "\033[1;31m[■ RED    - CRITICAL]\033[0m";
        default:         return "[?]";
    }
}

/* ============================================================================
 * TRACE COLLECTOR
 * ============================================================================ */
static void trace_record(uint32_t task_id, TraceEventType type, uint32_t dur_us) {
    RT_MUTEX_LOCK(&g.trace_mutex);
    uint32_t idx = g.trace_buf.head;
    g.trace_buf.records[idx].timestamp_us = get_time_us();
    g.trace_buf.records[idx].task_id      = task_id;
    g.trace_buf.records[idx].event_type   = type;
    g.trace_buf.records[idx].duration_us  = dur_us;
    g.trace_buf.head = (idx + 1) % TRACE_BUFFER_SIZE;
    if (g.trace_buf.count < TRACE_BUFFER_SIZE) g.trace_buf.count++;
    RT_MUTEX_UNLOCK(&g.trace_mutex);
}

/* ============================================================================
 * FAULT DETECTION ENGINE
 * ============================================================================ */
static void fault_register(uint32_t task_id, FaultType type,
                            FaultSeverity sev, const char *desc) {
    RT_MUTEX_LOCK(&g.fault_mutex);

    /* Suppress duplicate active faults */
    for (int i = 0; i < MAX_ACTIVE_FAULTS; i++) {
        if (g.fault_map[i].active &&
            g.fault_map[i].task_id == task_id &&
            g.fault_map[i].type == type) {
            g.fault_map[i].timestamp_us = get_time_us();
            RT_MUTEX_UNLOCK(&g.fault_mutex);
            return;
        }
    }

    int slot = -1;
    for (int i = 0; i < MAX_ACTIVE_FAULTS; i++) {
        if (!g.fault_map[i].active) { slot = i; break; }
    }
    if (slot != -1) {
        g.fault_map[slot].fault_id     = ++g.fault_counter;
        g.fault_map[slot].node_id      = g.node_id;
        g.fault_map[slot].task_id      = task_id;
        g.fault_map[slot].type         = type;
        g.fault_map[slot].severity     = sev;
        g.fault_map[slot].timestamp_us = get_time_us();
        g.fault_map[slot].active       = true;
        strncpy(g.fault_map[slot].description, desc,
                sizeof(g.fault_map[slot].description) - 1);

        trace_record(task_id, EVENT_FAULT, 0);

        if (sev == SEV_CRITICAL)                         hal_set_led(LED_RED);
        else if (sev == SEV_WARNING && g.led_state != LED_RED) hal_set_led(LED_YELLOW);
    }
    RT_MUTEX_UNLOCK(&g.fault_mutex);
}

static void fault_clear(uint32_t task_id, FaultType type) {
    RT_MUTEX_LOCK(&g.fault_mutex);
    bool has_crit = false, has_warn = false;
    for (int i = 0; i < MAX_ACTIVE_FAULTS; i++) {
        if (!g.fault_map[i].active) continue;
        if (g.fault_map[i].task_id == task_id && g.fault_map[i].type == type) {
            g.fault_map[i].active = false;
        } else {
            if (g.fault_map[i].severity == SEV_CRITICAL) has_crit = true;
            if (g.fault_map[i].severity == SEV_WARNING)  has_warn = true;
        }
    }
    if      (has_crit) hal_set_led(LED_RED);
    else if (has_warn) hal_set_led(LED_YELLOW);
    else               hal_set_led(LED_GREEN);
    RT_MUTEX_UNLOCK(&g.fault_mutex);
}

static void fault_clear_all(void) {
    RT_MUTEX_LOCK(&g.fault_mutex);
    for (int i = 0; i < MAX_ACTIVE_FAULTS; i++) g.fault_map[i].active = false;
    hal_set_led(LED_GREEN);
    RT_MUTEX_UNLOCK(&g.fault_mutex);
}

/* ============================================================================
 * IPC LATENCY PROFILER (Min / Max / Avg / P95)
 * ============================================================================ */
static void ipc_record_latency(uint32_t lat_us) {
    IpcStats *s = &g.ipc_stats;
    s->samples[s->head] = lat_us;
    s->head = (s->head + 1) % IPC_HISTORY_SIZE;
    if (s->count < IPC_HISTORY_SIZE) s->count++;

    uint32_t mn = 0xFFFFFFFF, mx = 0;
    uint64_t sum = 0;
    uint32_t tmp[IPC_HISTORY_SIZE];
    memcpy(tmp, s->samples, sizeof(uint32_t) * s->count);

    for (uint32_t i = 0; i < s->count; i++) {
        if (tmp[i] < mn) mn = tmp[i];
        if (tmp[i] > mx) mx = tmp[i];
        sum += tmp[i];
    }
    /* Insertion sort for P95 */
    for (uint32_t i = 1; i < s->count; i++) {
        uint32_t key = tmp[i]; int j = (int)i - 1;
        while (j >= 0 && tmp[j] > key) { tmp[j+1] = tmp[j]; j--; }
        tmp[j+1] = key;
    }
    uint32_t p95 = tmp[(uint32_t)(s->count * 0.95f)];

    s->min_us = (s->count > 0) ? mn : 0;
    s->max_us = mx;
    s->avg_us = (s->count > 0) ? (uint32_t)(sum / s->count) : 0;
    s->p95_us = p95;

    /* IPC fault thresholds */
    if (lat_us >= IPC_CRIT_LATENCY_US)
        fault_register(2, FAULT_IPC_TIMEOUT, SEV_CRITICAL, "IPC Latency Critical (>50ms)");
    else if (lat_us >= IPC_WARN_LATENCY_US)
        fault_register(2, FAULT_IPC_TIMEOUT, SEV_WARNING,  "IPC Latency Elevated (>1ms)");
    else
        fault_clear(2, FAULT_IPC_TIMEOUT);
}

/* ============================================================================
 * WORKLOAD SERVICE THREADS
 * ============================================================================ */

/* ---- Service A: Smart Traffic Controller  (100 ms period / 80 ms deadline) */
static void* service_a_thread(void *arg) {
    (void)arg;
    TaskControlBlock *t = &g.tasks[0];

    while (g.running) {
        uint64_t t_start = get_time_us();
        trace_record(t->task_id, EVENT_TASK_START, 0);

        /* Simulated workload: traffic sensor telemetry processing */
        uint32_t delay = 15 + t->inject_exec_delay_ms;
        sleep_ms(delay);

        /* Produce IPC message for Service B via shared-memory channel */
        RT_MUTEX_LOCK(&g.ipc_mutex);
        if (!g.ipc_channel.data_available) {
            g.ipc_channel.sequence++;
            g.ipc_channel.send_time_us = get_time_us();
            snprintf(g.ipc_channel.payload, sizeof(g.ipc_channel.payload),
                     "TRAFFIC:Sector_%u_Flow=%u", (t->heartbeat % 4) + 1, rand() % 100);
            g.ipc_channel.data_available = true;
            RT_COND_SIGNAL(&g.ipc_cond);
        }
        RT_MUTEX_UNLOCK(&g.ipc_mutex);

        uint64_t t_end  = get_time_us();
        uint32_t exec   = (uint32_t)(t_end - t_start);
        t->last_exec_us = exec;
        t->total_exec_us += exec;
        t->total_cycles++;
        if (exec > t->max_exec_us) t->max_exec_us = exec;
        trace_record(t->task_id, EVENT_TASK_END, exec);

        if (!t->inject_starvation) {
            t->heartbeat++;
            t->last_heartbeat_time_us = get_time_us();
            trace_record(t->task_id, EVENT_HEARTBEAT, 0);
        }

        /* Deadline check */
        if (exec > t->deadline_ms * 1000U) {
            t->deadline_misses++;
            fault_register(t->task_id, FAULT_DEADLINE_MISS, SEV_WARNING,
                           "Service A exceeded 80 ms deadline");
        } else {
            fault_clear(t->task_id, FAULT_DEADLINE_MISS);
        }

        uint32_t elapsed = (uint32_t)((get_time_us() - t_start) / 1000);
        if (elapsed < t->period_ms) sleep_ms(t->period_ms - elapsed);
    }
    return NULL;
}

/* ---- Service B: Smart Grid / IPC Consumer  (200 ms period / 150 ms deadline) */
static void* service_b_thread(void *arg) {
    (void)arg;
    TaskControlBlock *t = &g.tasks[1];

    while (g.running) {
        uint64_t t_start = get_time_us();
        trace_record(t->task_id, EVENT_TASK_START, 0);

        /* Consume IPC message from Service A */
        RT_MUTEX_LOCK(&g.ipc_mutex);
        if (g.ipc_channel.data_available) {
            if (g.ipc_channel.inject_delay_ms > 0)
                sleep_ms(g.ipc_channel.inject_delay_ms);

            uint64_t t_recv = get_time_us();
            uint32_t lat    = (uint32_t)(t_recv - g.ipc_channel.send_time_us);
            g.ipc_channel.recv_time_us   = t_recv;
            g.ipc_channel.latency_us     = lat;
            g.ipc_channel.data_available = false;
            ipc_record_latency(lat);
            trace_record(t->task_id, EVENT_IPC_XFER, lat);
        }
        RT_MUTEX_UNLOCK(&g.ipc_mutex);

        /* Simulated workload: grid load-balancing computation */
        uint32_t delay = 25 + t->inject_exec_delay_ms;
        sleep_ms(delay);

        uint64_t t_end  = get_time_us();
        uint32_t exec   = (uint32_t)(t_end - t_start);
        t->last_exec_us = exec;
        t->total_exec_us += exec;
        t->total_cycles++;
        if (exec > t->max_exec_us) t->max_exec_us = exec;
        trace_record(t->task_id, EVENT_TASK_END, exec);

        if (!t->inject_starvation) {
            t->heartbeat++;
            t->last_heartbeat_time_us = get_time_us();
            trace_record(t->task_id, EVENT_HEARTBEAT, 0);
        }

        if (exec > t->deadline_ms * 1000U) {
            t->deadline_misses++;
            fault_register(t->task_id, FAULT_DEADLINE_MISS, SEV_WARNING,
                           "Service B exceeded 150 ms deadline");
        } else {
            fault_clear(t->task_id, FAULT_DEADLINE_MISS);
        }

        uint32_t elapsed = (uint32_t)((get_time_us() - t_start) / 1000);
        if (elapsed < t->period_ms) sleep_ms(t->period_ms - elapsed);
    }
    return NULL;
}

/* ---- Service C: Environmental Sensor  (500 ms period / 400 ms deadline) */
static void* service_c_thread(void *arg) {
    (void)arg;
    TaskControlBlock *t = &g.tasks[2];

    while (g.running) {
        uint64_t t_start = get_time_us();
        trace_record(t->task_id, EVENT_TASK_START, 0);

        /* Simulated workload: air-quality & noise-level filtering */
        uint32_t delay = 40 + t->inject_exec_delay_ms;
        sleep_ms(delay);

        uint64_t t_end  = get_time_us();
        uint32_t exec   = (uint32_t)(t_end - t_start);
        t->last_exec_us = exec;
        t->total_exec_us += exec;
        t->total_cycles++;
        if (exec > t->max_exec_us) t->max_exec_us = exec;
        trace_record(t->task_id, EVENT_TASK_END, exec);

        if (!t->inject_starvation) {
            t->heartbeat++;
            t->last_heartbeat_time_us = get_time_us();
            trace_record(t->task_id, EVENT_HEARTBEAT, 0);
        }

        if (exec > t->deadline_ms * 1000U) {
            t->deadline_misses++;
            fault_register(t->task_id, FAULT_DEADLINE_MISS, SEV_WARNING,
                           "Service C exceeded 400 ms deadline");
        } else {
            fault_clear(t->task_id, FAULT_DEADLINE_MISS);
        }

        uint32_t elapsed = (uint32_t)((get_time_us() - t_start) / 1000);
        if (elapsed < t->period_ms) sleep_ms(t->period_ms - elapsed);
    }
    return NULL;
}

/* ---- CPU Burner: Overload Fault Injection */
static void* cpu_burner_thread(void *arg) {
    (void)arg;
    volatile double d = 0.0;
    while (g.running) {
        if (g.inject_cpu_overload) {
            uint64_t s = get_time_us();
            while (get_time_us() - s < 80000ULL) d += sin(d + 1.2345);
            sleep_ms(20);
        } else {
            sleep_ms(200);
        }
    }
    return (void*)0;
}

/* ============================================================================
 * HIGH-PRIORITY MONITORING TASK  (design.md §4)
 * Samples heartbeat staleness, execution time, deadline, and CPU utilisation
 * ============================================================================ */
static void* monitoring_task_thread(void *arg) {
    (void)arg;
    uint64_t prev_time = get_time_us();

    while (g.running) {
        sleep_ms(MONITOR_INTERVAL_MS);
        uint64_t now       = get_time_us();
        uint64_t window_us = now - prev_time;
        prev_time = now;

        float work_us = 0.0f;

        for (int i = 0; i < MAX_TASKS; i++) {
            TaskControlBlock *t = &g.tasks[i];

            /* Period-aware heartbeat staleness detection */
            uint64_t age_ms   = (now - t->last_heartbeat_time_us) / 1000;
            uint32_t warn_ms  = t->period_ms * 2;
            uint32_t crit_ms  = t->period_ms * 3;

            if (age_ms >= crit_ms) {
                t->state = TASK_STATE_STARVED;
                char desc[64];
                snprintf(desc, sizeof(desc), "Task '%s' STARVED (%u ms silent)", t->name, (unsigned)age_ms);
                fault_register(t->task_id, FAULT_TASK_STARVATION, SEV_CRITICAL, desc);
            } else if (age_ms >= warn_ms) {
                t->state = TASK_STATE_WARNING;
                char desc[64];
                snprintf(desc, sizeof(desc), "Task '%s' HB slow (%u ms)", t->name, (unsigned)age_ms);
                fault_register(t->task_id, FAULT_TASK_STARVATION, SEV_WARNING, desc);
            } else {
                if (t->state == TASK_STATE_STARVED || t->state == TASK_STATE_WARNING) {
                    t->state = TASK_STATE_RUNNING;
                    fault_clear(t->task_id, FAULT_TASK_STARVATION);
                }
            }

            /* Per-task CPU duty-cycle estimate */
            if (window_us > 0) {
                float duty = ((float)t->last_exec_us / (float)(t->period_ms * 1000)) * 100.0f;
                t->cpu_usage_pct = (duty > 100.0f) ? 100.0f : duty;
                work_us += t->cpu_usage_pct * 0.01f * (float)window_us;
            }
        }

        /* Aggregate CPU utilisation */
        float cpu = (window_us > 0) ? (work_us / (float)window_us * 100.0f) : 0.0f;
        if (g.inject_cpu_overload) cpu += 60.0f;
        if (cpu > 100.0f) cpu = 100.0f;
        g.total_cpu_pct = cpu;

        /* CPU overload fault thresholds */
        if (cpu >= CPU_CRIT_THRESHOLD) {
            char d[64]; snprintf(d, sizeof(d), "CPU Overload Critical: %.1f%%", cpu);
            fault_register(0, FAULT_CPU_OVERLOAD, SEV_CRITICAL, d);
        } else if (cpu >= CPU_WARN_THRESHOLD) {
            char d[64]; snprintf(d, sizeof(d), "CPU Elevated: %.1f%%", cpu);
            fault_register(0, FAULT_CPU_OVERLOAD, SEV_WARNING, d);
        } else {
            fault_clear(0, FAULT_CPU_OVERLOAD);
        }
    }
    return NULL;
}

/* ============================================================================
 * TCP TELEMETRY SENDER  (Pi 1 -> Pi 2)
 * Non-blocking reconnect loop; drops packets when disconnected
 * ============================================================================ */
static void build_json(char *buf, size_t sz) {
    snprintf(buf, sz,
        "{\"node\":1,\"timestamp\":" FMT_U64 ","
        "\"cpu\":%.1f,\"led\":%d,"
        "\"ipc\":{\"min\":%u,\"max\":%u,\"avg\":%u,\"p95\":%u},"
        "\"tasks\":["
        "{\"id\":%u,\"name\":\"%s\",\"hb\":%u,\"exec_us\":%u,\"miss\":%u,\"state\":%d},"
        "{\"id\":%u,\"name\":\"%s\",\"hb\":%u,\"exec_us\":%u,\"miss\":%u,\"state\":%d},"
        "{\"id\":%u,\"name\":\"%s\",\"hb\":%u,\"exec_us\":%u,\"miss\":%u,\"state\":%d}"
        "]}\n",
        (uint64_t)get_time_us(),
        g.total_cpu_pct, (int)g.led_state,
        g.ipc_stats.min_us, g.ipc_stats.max_us, g.ipc_stats.avg_us, g.ipc_stats.p95_us,
        g.tasks[0].task_id, g.tasks[0].name, g.tasks[0].heartbeat,
            g.tasks[0].last_exec_us, g.tasks[0].deadline_misses, (int)g.tasks[0].state,
        g.tasks[1].task_id, g.tasks[1].name, g.tasks[1].heartbeat,
            g.tasks[1].last_exec_us, g.tasks[1].deadline_misses, (int)g.tasks[1].state,
        g.tasks[2].task_id, g.tasks[2].name, g.tasks[2].heartbeat,
            g.tasks[2].last_exec_us, g.tasks[2].deadline_misses, (int)g.tasks[2].state
    );
}

static void* telemetry_sender_thread(void *arg) {
    (void)arg;
    char buf[1024];

    while (g.running) {
        int sock = (int)socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) { sleep_ms(1000); continue; }

        struct sockaddr_in sv = {0};
        sv.sin_family = AF_INET;
        sv.sin_port   = htons((uint16_t)g.supervisor_port);
        sv.sin_addr.s_addr = inet_addr(g.supervisor_ip);

        printf("[Telemetry] Connecting to Supervisor %s:%d ...\n",
               g.supervisor_ip, g.supervisor_port);
        if (connect(sock, (struct sockaddr*)&sv, sizeof(sv)) < 0) {
            perror("[Telemetry] Connect error");
#if defined(_WIN32) && !defined(__CYGWIN__)
            closesocket(sock);
#else
            close(sock);
#endif
            sleep_ms(2000); continue;
        }

        printf("[Telemetry] Connected to Pi 2 Supervisor.\n");
        g.tcp_connected = true;

        while (g.running) {
            build_json(buf, sizeof(buf));
            if (send(sock, buf, (int)strlen(buf), 0) <= 0) {
                printf("[Telemetry] Supervisor connection lost.\n");
                break;
            }
            sleep_ms(TELEMETRY_INTERVAL_MS);
        }

        g.tcp_connected = false;
#if defined(_WIN32) && !defined(__CYGWIN__)
        closesocket(sock);
#else
        close(sock);
#endif
        sleep_ms(1000);
    }
    return NULL;
}

/* ============================================================================
 * FAULT INJECTION + DIAGNOSTIC CLI  (UART / Terminal)
 * design.md §14, §17
 * ============================================================================ */
static void cli_status(void) {
    printf("\n=======================================================\n");
    printf("   NODE 1 — WORKLOAD NODE STATUS\n");
    printf("=======================================================\n");
    printf(" Platform         : %s\n", PLATFORM_NAME);
    printf(" Health LED       : %s\n", hal_led_str());
    printf(" CPU Utilization  : %.1f %%\n", g.total_cpu_pct);
    printf(" Supervisor Link  : %s\n", g.tcp_connected ? "\033[32mCONNECTED\033[0m" : "\033[31mDISCONNECTED\033[0m");
    printf(" Active Faults    : %u\n", g.fault_counter);
    printf(" Uptime           : " FMT_U64 " ms\n", (uint64_t)(get_time_us() / 1000));
    printf("=======================================================\n");
}

static void cli_tasks(void) {
    printf("\n%-4s  %-22s  %-8s  %-6s  %-8s  %-8s  %-10s  %-5s\n",
           "ID", "NAME", "STATE", "PRIO", "PERIOD", "DEADLN", "EXEC_US", "HB");
    printf("-------------------------------------------------------------------------------------\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        TaskControlBlock *t = &g.tasks[i];
        const char *st = (t->state == TASK_STATE_STARVED) ? "\033[31mSTARVED\033[0m" :
                         (t->state == TASK_STATE_WARNING) ? "\033[33mWARNING\033[0m" : "RUNNING";
        printf("%-4u  %-22s  %-8s  %-6u  %-4u ms  %-4u ms  %-10u  %-5u\n",
               t->task_id, t->name, st, t->priority,
               t->period_ms, t->deadline_ms, t->last_exec_us, t->heartbeat);
    }
    printf("-------------------------------------------------------------------------------------\n");
}

static void cli_cpu(void) {
    printf("\n--- CPU UTILIZATION ---\n");
    printf(" Total : %.1f %%\n", g.total_cpu_pct);
    for (int i = 0; i < MAX_TASKS; i++) {
        TaskControlBlock *t = &g.tasks[i];
        printf("  %-22s : %5.1f %%  (last=%u us  max=%u us  cycles=%u)\n",
               t->name, t->cpu_usage_pct, t->last_exec_us, t->max_exec_us, t->total_cycles);
    }
    printf(" CPU Overload Injected: %s\n", g.inject_cpu_overload ? "YES" : "NO");
}

static void cli_ipc(void) {
    IpcStats *s = &g.ipc_stats;
    printf("\n--- IPC LATENCY STATS (Shared Memory A -> B) ---\n");
    printf(" Samples : %u\n", s->count);
    printf(" Min     : %u us\n", s->min_us);
    printf(" Max     : %u us\n", s->max_us);
    printf(" Avg     : %u us\n", s->avg_us);
    printf(" P95     : %u us\n", s->p95_us);
    printf(" Delay Injected : %u ms\n", g.ipc_channel.inject_delay_ms);
}

static void cli_faults(void) {
    printf("\n================ FAULT MAP — NODE 1 ================\n");
    bool any = false;
    for (int i = 0; i < MAX_ACTIVE_FAULTS; i++) {
        FaultRecord *f = &g.fault_map[i];
        if (!f->active) continue;
        any = true;
        const char *sv = (f->severity == SEV_CRITICAL) ? "\033[31mCRIT\033[0m" : "\033[33mWARN\033[0m";
        printf("  [%s] #%u Task:%u | %s\n", sv, f->fault_id, f->task_id, f->description);
    }
    if (!any) printf("  No active faults. System healthy.\n");
    printf("=====================================================\n");
}

static void cli_trace(void) {
    printf("\n--- TRACE / GANTT TIMELINE (last 20 events) ---\n");
    RT_MUTEX_LOCK(&g.trace_mutex);
    int n = (int)((g.trace_buf.count > 20) ? 20 : g.trace_buf.count);
    int s = (g.trace_buf.head - n + TRACE_BUFFER_SIZE) % TRACE_BUFFER_SIZE;
    for (int i = 0; i < n; i++) {
        TraceRecord *r = &g.trace_buf.records[(s + i) % TRACE_BUFFER_SIZE];
        const char *tn = (r->task_id == 1) ? "Service_A" :
                         (r->task_id == 2) ? "Service_B" : "Service_C";
        const char *ev = (r->event_type == EVENT_TASK_START) ? "START" :
                         (r->event_type == EVENT_TASK_END)   ? "END  " :
                         (r->event_type == EVENT_HEARTBEAT)  ? "HB   " :
                         (r->event_type == EVENT_FAULT)      ? "\033[31mFAULT\033[0m" : "IPC  ";
        printf("  +%06lu us | %-10s | %s | %u us\n",
               (unsigned long)(r->timestamp_us % 10000000ULL), tn, ev, r->duration_us);
    }
    RT_MUTEX_UNLOCK(&g.trace_mutex);
}

static void cli_help(void) {
    printf("\nNODE 1 — Diagnostic Commands:\n");
    printf("  status                       Global health & LED state\n");
    printf("  tasks                        All task metrics\n");
    printf("  cpu                          CPU utilization breakdown\n");
    printf("  ipc                          Shared-memory IPC latency stats\n");
    printf("  faults                       Active fault map\n");
    printf("  trace                        Gantt execution timeline\n");
    printf("  inject starve  <1-3>         Freeze heartbeat (task starvation)\n");
    printf("  inject deadline <1-3> <ms>   Inflate task execution time\n");
    printf("  inject cpu <on|off>          Toggle CPU overload burner\n");
    printf("  inject ipc <delay_ms>        Inject IPC channel latency\n");
    printf("  clear                        Clear all faults & injections\n");
    printf("  exit                         Shutdown node\n\n");
}

static void* cli_thread(void *arg) {
    (void)arg;
    char line[128];
    printf("\n***  NODE 1 (Workload) — RTOS Diagnostic CLI  ***\n");
    printf("    Type 'help' for available commands.\n\n");

    while (g.running) {
        printf("node1> "); fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\r\n")] = 0;
        if (!strlen(line)) continue;

        if      (!strcmp(line, "help"))    cli_help();
        else if (!strcmp(line, "status"))  cli_status();
        else if (!strcmp(line, "tasks"))   cli_tasks();
        else if (!strcmp(line, "cpu"))     cli_cpu();
        else if (!strcmp(line, "ipc"))     cli_ipc();
        else if (!strcmp(line, "faults"))  cli_faults();
        else if (!strcmp(line, "trace"))   cli_trace();
        else if (!strcmp(line, "clear")) {
            fault_clear_all();
            for (int i = 0; i < MAX_TASKS; i++) {
                g.tasks[i].inject_starvation    = false;
                g.tasks[i].inject_exec_delay_ms = 0;
            }
            g.inject_cpu_overload           = false;
            g.ipc_channel.inject_delay_ms   = 0;
            printf("[CLI] All faults cleared.\n");
        }
        else if (!strncmp(line, "inject starve ", 14)) {
            int tid = atoi(line + 14);
            if (tid >= 1 && tid <= MAX_TASKS) {
                g.tasks[tid-1].inject_starvation = true;
                printf("[Inject] Starvation active on Task %d (%s).\n", tid, g.tasks[tid-1].name);
            }
        }
        else if (!strncmp(line, "inject deadline ", 16)) {
            int tid = 0, ms = 0;
            if (sscanf(line + 16, "%d %d", &tid, &ms) == 2 && tid >= 1 && tid <= MAX_TASKS) {
                g.tasks[tid-1].inject_exec_delay_ms = ms;
                printf("[Inject] %d ms deadline delay on Task %d (%s).\n", ms, tid, g.tasks[tid-1].name);
            }
        }
        else if (!strncmp(line, "inject cpu ", 11)) {
            g.inject_cpu_overload = (!strcmp(line + 11, "on"));
            printf("[Inject] CPU burner %s.\n", g.inject_cpu_overload ? "ON" : "OFF");
        }
        else if (!strncmp(line, "inject ipc ", 11)) {
            g.ipc_channel.inject_delay_ms = (uint32_t)atoi(line + 11);
            printf("[Inject] IPC delay = %u ms.\n", g.ipc_channel.inject_delay_ms);
        }
        else if (!strcmp(line, "exit") || !strcmp(line, "quit")) {
            g.running = false; break;
        }
        else printf("Unknown command. Type 'help'.\n");
    }
    return NULL;
}

/* ============================================================================
 * MAIN — Node 1 Entry Point
 * ============================================================================ */
int main(int argc, char *argv[]) {
    memset(&g, 0, sizeof(g));
    g.running       = true;
    g.node_id       = 1;
    g.led_state     = LED_GREEN;
    g.supervisor_port = TELEMETRY_PORT;
    strncpy(g.supervisor_ip, "169.254.178.16", sizeof(g.supervisor_ip) - 1); /* Default Pi 2 IP */

    bool enable_gpio = true;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--ip=", 5))   strncpy(g.supervisor_ip, argv[i]+5, sizeof(g.supervisor_ip)-1);
        if (!strncmp(argv[i], "--port=", 7)) g.supervisor_port = atoi(argv[i]+7);
        if (!strcmp(argv[i], "--no-gpio"))   enable_gpio = false;
    }

#if defined(_WIN32) && !defined(__CYGWIN__)
    WSADATA ws; WSAStartup(MAKEWORD(2,2), &ws);
#endif

    RT_MUTEX_INIT(&g.task_mutex);
    RT_MUTEX_INIT(&g.ipc_mutex);
    RT_MUTEX_INIT(&g.fault_mutex);
    RT_MUTEX_INIT(&g.trace_mutex);
    RT_COND_INIT(&g.ipc_cond);

    /* --- Task Control Blocks: design.md §3 --- */
    g.tasks[0] = (TaskControlBlock){
        .task_id = 1, .period_ms = 100, .deadline_ms = 80,
        .priority = PRIORITY_SERVICE_A, .state = TASK_STATE_RUNNING };
    strncpy(g.tasks[0].name, "Service_A (Traffic)", 31);
    g.tasks[0].last_heartbeat_time_us = get_time_us();

    g.tasks[1] = (TaskControlBlock){
        .task_id = 2, .period_ms = 200, .deadline_ms = 150,
        .priority = PRIORITY_SERVICE_B, .state = TASK_STATE_RUNNING };
    strncpy(g.tasks[1].name, "Service_B (Grid)", 31);
    g.tasks[1].last_heartbeat_time_us = get_time_us();

    g.tasks[2] = (TaskControlBlock){
        .task_id = 3, .period_ms = 500, .deadline_ms = 400,
        .priority = PRIORITY_SERVICE_C, .state = TASK_STATE_RUNNING };
    strncpy(g.tasks[2].name, "Service_C (Env)", 31);
    g.tasks[2].last_heartbeat_time_us = get_time_us();

    printf("===============================================================\n");
    printf(" %s — NODE 1 (Workload Node)\n", PLATFORM_NAME);
    printf(" Supervisor IP : %s:%d\n", g.supervisor_ip, g.supervisor_port);
    printf(" GPIO Enabled  : %s\n", enable_gpio ? "YES (BCM2711 direct I/O)" : "NO (--no-gpio)");
    printf("===============================================================\n");

    /* Initialize GPIO hardware (LEDs + input buttons) if enabled */
    if (enable_gpio) {
        hal_gpio_init();
    }

    rt_thread_t th_sa, th_sb, th_sc, th_mon, th_burn, th_tcp, th_gpio, th_cli;
    rt_thread_create(&th_sa,   PRIORITY_SERVICE_A,   service_a_thread,       NULL);
    rt_thread_create(&th_sb,   PRIORITY_SERVICE_B,   service_b_thread,       NULL);
    rt_thread_create(&th_sc,   PRIORITY_SERVICE_C,   service_c_thread,       NULL);
    rt_thread_create(&th_mon,  PRIORITY_MONITOR,     monitoring_task_thread, NULL);
    rt_thread_create(&th_burn, PRIORITY_CLI,         cpu_burner_thread,      NULL);
    rt_thread_create(&th_tcp,  PRIORITY_TELEMETRY,   telemetry_sender_thread, NULL);
    if (enable_gpio) {
        /* GPIO button polling thread — triggers gpio_button_callback() on change */
        rt_thread_create(&th_gpio, PRIORITY_FAULT_DETECTOR, hal_gpio_poll_thread,
                         (void*)gpio_button_callback);
    }
    rt_thread_create(&th_cli,  PRIORITY_CLI,         cli_thread,             NULL);

    rt_thread_join(th_cli);
    g.running = false;
    sleep_ms(500);
    hal_gpio_deinit();    /* Turn off LEDs, release GPIO memory map */

#if defined(_WIN32) && !defined(__CYGWIN__)
    WSACleanup();
#endif
    return 0;
}
