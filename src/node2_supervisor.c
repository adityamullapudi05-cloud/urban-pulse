/**
 * ============================================================================
 * node2_supervisor.c  —  Raspberry Pi 2  (Supervisor / Analytics Node)
 * Smart City RTOS Fault & Performance Monitoring Platform
 *
 * Build in QNX Momentics IDE  →  Deploy to Raspberry Pi 2
 * Run:  ./node2_supervisor  (optionally --port=5555)
 *
 * What this node does (design.md §2, §11-§16):
 *   - TCP Telemetry Server (listens for Pi 1 JSON telemetry on port 5555)
 *   - Fault Detection Engine: Heartbeat, Deadline, CPU, IPC, Node-disconnect
 *   - Analytics Engine: Min / Max / Avg / P95 for CPU and IPC metrics
 *   - Fault Map: live node/task fault hierarchy table
 *   - Gantt Recorder: timestamped execution record received from Pi 1
 *   - GPIO Health LEDs: Green / Yellow / Red on Pi 2 (Pi-local indication)
 *   - GPIO Input Buttons: level-based physical fault simulation
 *   - UART / Terminal Diagnostic CLI with all monitor> commands
 * ============================================================================
 */

#include "smart_city_common.h"

/* Forward declarations from hal_gpio.c (shared with Node 1) */
extern int   hal_gpio_init(void);
extern void  hal_gpio_set_led(HealthLedState state);
extern void* hal_gpio_poll_thread(void *arg);
extern void  hal_gpio_deinit(void);

/* ============================================================================
 * NODE 2 GLOBAL CONTEXT
 * ============================================================================ */
typedef struct {
    volatile bool running;
    uint32_t      node_id;         /* Always 2 */
    int           tcp_port;

    /* Mirror of Node 1 task state, received via TCP telemetry */
    TaskControlBlock remote_tasks[MAX_TASKS];
    float            remote_cpu_pct;
    HealthLedState   remote_led;

    /* Local analytics accumulators */
    float   cpu_history[100];
    uint32_t cpu_hist_head;
    uint32_t cpu_hist_count;
    float   cpu_min, cpu_max, cpu_avg, cpu_p95;

    IpcStats remote_ipc;   /* Latest IPC stats received from Pi 1 */

    /* Fault map for both nodes */
    FaultRecord  fault_map[MAX_ACTIVE_FAULTS];
    uint32_t     fault_counter;
    rt_mutex_t   fault_mutex;

    /* Health LED on Pi 2 */
    HealthLedState led_state;

    /* Gantt event log (simplified: store received task events) */
    TraceRingBuffer gantt_log;
    rt_mutex_t      gantt_mutex;

    /* Peer connection state */
    bool     node1_connected;
    uint64_t node1_last_rx_us;

    /* Analytics mutex */
    rt_mutex_t analytics_mutex;
} Node2Context;

static Node2Context g;

/* ============================================================================
 * HAL — GPIO LED wrapper (delegates to hal_gpio.c real hardware)
 * ============================================================================ */
static void hal_set_led(HealthLedState state) {
    g.led_state = state;
    hal_gpio_set_led(state);    /* Drives BCM2711 GPIO registers on QNX */
}

static const char* hal_led_str(void) {
    switch (g.led_state) {
        case LED_GREEN:  return "\033[1;32m[● GREEN  - HEALTHY]\033[0m";
        case LED_YELLOW: return "\033[1;33m[▲ YELLOW - WARNING]\033[0m";
        case LED_RED:    return "\033[1;31m[■ RED    - CRITICAL]\033[0m";
        default:         return "[?]";
    }
}

/* Forward declarations — defined later in FAULT DETECTION ENGINE section */
static void fault_register(uint32_t node_id, uint32_t task_id,
                            FaultType type, FaultSeverity sev, const char *desc);
static void fault_clear(uint32_t node_id, uint32_t task_id, FaultType type);

/* ============================================================================
 * GPIO LEVEL-BASED CALLBACK  —  NODE 2 (Supervisor Pi)
 * Called by hal_gpio_poll_thread() whenever a button pin CHANGES level.
 *
 * BEHAVIOR:
 *   pressed = true  → PIN went HIGH→LOW (GND contact held)  → Simulate fault
 *   pressed = false → PIN went LOW→HIGH (GND released)      → Clear, NORMAL
 *
 * Button Map (Pi 2 — 3 buttons, simulates what supervisor would observe):
 *   0 = GPIO 23 (Pin 16) → Simulate NODE 1 DISCONNECTED while held
 *   1 = GPIO 24 (Pin 18) → Simulate IPC TIMEOUT          while held
 *   2 = GPIO 25 (Pin 22) → Simulate CPU OVERLOAD WARNING  while held
 * ============================================================================ */
static void gpio_button_callback(int btn_id, bool pressed) {
    switch (btn_id) {

        case 0:  /* GPIO 23 — Simulate Node 1 Disconnection */
            if (pressed) {
                g.node1_connected = false;
                fault_register(1, 0, FAULT_NODE_DISCONNECTED, SEV_CRITICAL,
                               "[GPIO] Node1 disconnect simulated via Pin 16");
                printf("\n[GPIO] Pin 16 → GND HELD   : NODE 1 DISCONNECTED (simulated)\n");
                printf("                               Red LED ON — check supervisor> faultmap\n");
            } else {
                g.node1_connected = true;
                g.node1_last_rx_us = get_time_us();
                fault_clear(1, 0, FAULT_NODE_DISCONNECTED);
                printf("\n[GPIO] Pin 16 → RELEASED   : Node 1 RECONNECTED — fault cleared, NORMAL\n");
            }
            printf("supervisor> "); fflush(stdout);
            break;

        case 1:  /* GPIO 24 — Simulate IPC Timeout */
            if (pressed) {
                fault_register(1, 2, FAULT_IPC_TIMEOUT, SEV_CRITICAL,
                               "[GPIO] IPC timeout simulated via Pin 18");
                printf("\n[GPIO] Pin 18 → GND HELD   : IPC TIMEOUT active (simulated)\n");
                printf("                               Yellow/Red LED ON while contact is held\n");
            } else {
                fault_clear(1, 2, FAULT_IPC_TIMEOUT);
                printf("\n[GPIO] Pin 18 → RELEASED   : IPC timeout CLEARED — returning to NORMAL\n");
            }
            printf("supervisor> "); fflush(stdout);
            break;

        case 2:  /* GPIO 25 — Simulate CPU Overload */
            if (pressed) {
                fault_register(1, 0, FAULT_CPU_OVERLOAD, SEV_WARNING,
                               "[GPIO] CPU overload simulated via Pin 22");
                printf("\n[GPIO] Pin 22 → GND HELD   : CPU OVERLOAD WARNING active (simulated)\n");
                printf("                               Yellow LED ON while contact is held\n");
            } else {
                fault_clear(1, 0, FAULT_CPU_OVERLOAD);
                printf("\n[GPIO] Pin 22 → RELEASED   : CPU overload CLEARED — returning to NORMAL\n");
            }
            printf("supervisor> "); fflush(stdout);
            break;

        default: break;
    }
}

/* ============================================================================
 * FAULT DETECTION ENGINE
 * ============================================================================ */
static void fault_register(uint32_t node_id, uint32_t task_id,
                            FaultType type, FaultSeverity sev, const char *desc) {
    RT_MUTEX_LOCK(&g.fault_mutex);

    for (int i = 0; i < MAX_ACTIVE_FAULTS; i++) {
        if (g.fault_map[i].active &&
            g.fault_map[i].node_id == node_id &&
            g.fault_map[i].task_id == task_id &&
            g.fault_map[i].type   == type) {
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
        g.fault_map[slot].node_id      = node_id;
        g.fault_map[slot].task_id      = task_id;
        g.fault_map[slot].type         = type;
        g.fault_map[slot].severity     = sev;
        g.fault_map[slot].timestamp_us = get_time_us();
        g.fault_map[slot].active       = true;
        strncpy(g.fault_map[slot].description, desc, sizeof(g.fault_map[slot].description)-1);

        if (sev == SEV_CRITICAL)                          hal_set_led(LED_RED);
        else if (sev == SEV_WARNING && g.led_state != LED_RED) hal_set_led(LED_YELLOW);
    }
    RT_MUTEX_UNLOCK(&g.fault_mutex);
}

static void fault_clear(uint32_t node_id, uint32_t task_id, FaultType type) {
    RT_MUTEX_LOCK(&g.fault_mutex);
    bool has_crit = false, has_warn = false;
    for (int i = 0; i < MAX_ACTIVE_FAULTS; i++) {
        if (!g.fault_map[i].active) continue;
        if (g.fault_map[i].node_id == node_id &&
            g.fault_map[i].task_id == task_id &&
            g.fault_map[i].type   == type) {
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
 * ANALYTICS ENGINE — CPU statistics (Min / Max / Avg / P95)
 * ============================================================================ */
static void analytics_update_cpu(float cpu) {
    RT_MUTEX_LOCK(&g.analytics_mutex);
    g.cpu_history[g.cpu_hist_head] = cpu;
    g.cpu_hist_head = (g.cpu_hist_head + 1) % 100;
    if (g.cpu_hist_count < 100) g.cpu_hist_count++;

    float mn = 200.0f, mx = 0.0f, sum = 0.0f;
    float tmp[100];
    uint32_t n = g.cpu_hist_count;
    memcpy(tmp, g.cpu_history, sizeof(float) * n);

    for (uint32_t i = 0; i < n; i++) {
        if (tmp[i] < mn) mn = tmp[i];
        if (tmp[i] > mx) mx = tmp[i];
        sum += tmp[i];
    }
    /* Insertion sort for P95 */
    for (uint32_t i = 1; i < n; i++) {
        float key = tmp[i]; int j = (int)i - 1;
        while (j >= 0 && tmp[j] > key) { tmp[j+1] = tmp[j]; j--; }
        tmp[j+1] = key;
    }
    g.cpu_min = (n > 0) ? mn : 0;
    g.cpu_max = mx;
    g.cpu_avg = (n > 0) ? sum / (float)n : 0;
    g.cpu_p95 = tmp[(uint32_t)(n * 0.95f)];
    RT_MUTEX_UNLOCK(&g.analytics_mutex);
}

/* ============================================================================
 * FAULT DETECTOR ENGINE — Evaluates received telemetry for fault conditions
 * design.md §11, §12
 * ============================================================================ */
static void fault_detector_evaluate(void) {
    float cpu = g.remote_cpu_pct;

    /* CPU overload faults */
    if (cpu >= CPU_CRIT_THRESHOLD) {
        char d[64]; snprintf(d, sizeof(d), "Node1 CPU Critical: %.1f%%", cpu);
        fault_register(1, 0, FAULT_CPU_OVERLOAD, SEV_CRITICAL, d);
    } else if (cpu >= CPU_WARN_THRESHOLD) {
        char d[64]; snprintf(d, sizeof(d), "Node1 CPU Elevated: %.1f%%", cpu);
        fault_register(1, 0, FAULT_CPU_OVERLOAD, SEV_WARNING, d);
    } else {
        fault_clear(1, 0, FAULT_CPU_OVERLOAD);
    }

    /* Per-task heartbeat & deadline fault detection from received task states */
    for (int i = 0; i < MAX_TASKS; i++) {
        TaskControlBlock *t = &g.remote_tasks[i];
        if (t->task_id == 0) continue; /* Not yet received */

        if (t->state == TASK_STATE_STARVED) {
            char d[64]; snprintf(d, sizeof(d), "Node1 Task '%s' STARVED", t->name);
            fault_register(1, t->task_id, FAULT_TASK_STARVATION, SEV_CRITICAL, d);
        } else if (t->state == TASK_STATE_WARNING) {
            char d[64]; snprintf(d, sizeof(d), "Node1 Task '%s' HB Warning", t->name);
            fault_register(1, t->task_id, FAULT_TASK_STARVATION, SEV_WARNING, d);
        } else {
            fault_clear(1, t->task_id, FAULT_TASK_STARVATION);
        }

        if (t->deadline_misses > 0) {
            char d[64]; snprintf(d, sizeof(d), "Node1 Task '%s' Deadline Miss (%u)", t->name, t->deadline_misses);
            fault_register(1, t->task_id, FAULT_DEADLINE_MISS, SEV_WARNING, d);
        } else {
            fault_clear(1, t->task_id, FAULT_DEADLINE_MISS);
        }
    }

    /* IPC latency faults */
    if (g.remote_ipc.p95_us >= IPC_CRIT_LATENCY_US) {
        fault_register(1, 2, FAULT_IPC_TIMEOUT, SEV_CRITICAL, "Node1 IPC Latency Critical (P95 >50ms)");
    } else if (g.remote_ipc.p95_us >= IPC_WARN_LATENCY_US) {
        fault_register(1, 2, FAULT_IPC_TIMEOUT, SEV_WARNING, "Node1 IPC Latency Elevated (P95 >1ms)");
    } else {
        fault_clear(1, 2, FAULT_IPC_TIMEOUT);
    }

    /* Node connection watchdog */
    uint64_t age_ms = (get_time_us() - g.node1_last_rx_us) / 1000;
    if (age_ms > 3000) {  /* 3 seconds without telemetry = disconnected */
        g.node1_connected = false;
        fault_register(1, 0, FAULT_NODE_DISCONNECTED, SEV_CRITICAL, "Node 1 Telemetry Timeout (>3s)");
    }
}

/* ============================================================================
 * SIMPLE JSON PARSER — Extracts key numeric fields from Pi 1 telemetry
 * For production: replace with a proper cJSON parser
 * ============================================================================ */
static float json_get_float(const char *json, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0.0f;
    p += strlen(search);
    return (float)atof(p);
}

static int json_get_int(const char *json, const char *key) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    return atoi(p);
}

static void parse_telemetry(const char *json) {
    RT_MUTEX_LOCK(&g.analytics_mutex);

    g.remote_cpu_pct  = json_get_float(json, "cpu");
    g.remote_led      = (HealthLedState)json_get_int(json, "led");

    /* IPC block */
    const char *ipc_p = strstr(json, "\"ipc\":");
    if (ipc_p) {
        g.remote_ipc.min_us = (uint32_t)json_get_int(ipc_p, "min");
        g.remote_ipc.max_us = (uint32_t)json_get_int(ipc_p, "max");
        g.remote_ipc.avg_us = (uint32_t)json_get_int(ipc_p, "avg");
        g.remote_ipc.p95_us = (uint32_t)json_get_int(ipc_p, "p95");
    }

    /* Task entries */
    const char *tp = strstr(json, "\"tasks\":");
    if (tp) {
        for (int i = 0; i < MAX_TASKS; i++) {
            tp = strchr(tp, '{');
            if (!tp) break;
            int id   = json_get_int(tp, "id");
            int hb   = json_get_int(tp, "hb");
            int eu   = json_get_int(tp, "exec_us");
            int miss = json_get_int(tp, "miss");
            int st   = json_get_int(tp, "state");
            if (id >= 1 && id <= MAX_TASKS) {
                int idx = id - 1;
                g.remote_tasks[idx].task_id       = (uint32_t)id;
                g.remote_tasks[idx].heartbeat     = (uint32_t)hb;
                g.remote_tasks[idx].last_exec_us  = (uint32_t)eu;
                g.remote_tasks[idx].deadline_misses = (uint32_t)miss;
                g.remote_tasks[idx].state         = (TaskState)st;

                /* Copy task names on first receive */
                if (!g.remote_tasks[idx].name[0]) {
                    char namekey[16]; snprintf(namekey, sizeof(namekey), "\"name\":");
                    const char *np = strstr(tp, namekey);
                    if (np) {
                        np = strchr(np, '"') + 1; /* skip key quote */
                        np = strchr(np, '"') + 1; /* skip to value */
                        int k = 0;
                        while (*np && *np != '"' && k < 31)
                            g.remote_tasks[idx].name[k++] = *np++;
                        g.remote_tasks[idx].name[k] = '\0';
                    }
                }
            }
            tp++;
        }
    }
    RT_MUTEX_UNLOCK(&g.analytics_mutex);

    analytics_update_cpu(g.remote_cpu_pct);
    fault_detector_evaluate();
}

/* ============================================================================
 * TCP TELEMETRY SERVER  (Pi 2 listens; Pi 1 connects and sends)
 * design.md §9, §10
 * ============================================================================ */
static void* telemetry_server_thread(void *arg) {
    (void)arg;
    int srv = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return NULL; }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)g.tcp_port);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); return NULL;
    }
    listen(srv, 2);
    printf("[Supervisor] TCP server listening on port %d ...\n", g.tcp_port);

    char rx[2048];

    while (g.running) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int conn = (int)accept(srv, (struct sockaddr*)&cli_addr, &cli_len);
        if (conn < 0) continue;

        printf("[Supervisor] Node 1 connected from %s\n", inet_ntoa(cli_addr.sin_addr));
        g.node1_connected   = true;
        g.node1_last_rx_us  = get_time_us();
        fault_clear(1, 0, FAULT_NODE_DISCONNECTED);

        int remain = 0;
        while (g.running) {
            int n = recv(conn, rx + remain, (int)(sizeof(rx) - 1 - remain), 0);
            if (n <= 0) break;
            remain += n;
            rx[remain] = '\0';

            /* Process complete newline-delimited JSON packets */
            char *nl;
            char *start = rx;
            while ((nl = strchr(start, '\n')) != NULL) {
                *nl = '\0';
                parse_telemetry(start);
                g.node1_last_rx_us = get_time_us();
                start = nl + 1;
            }
            remain = (int)strlen(start);
            memmove(rx, start, remain + 1);
        }

        printf("[Supervisor] Node 1 disconnected.\n");
        g.node1_connected = false;
        fault_register(1, 0, FAULT_NODE_DISCONNECTED, SEV_CRITICAL, "Node 1 TCP Connection Lost");
#if defined(_WIN32) && !defined(__CYGWIN__)
        closesocket(conn);
#else
        close(conn);
#endif
    }

#if defined(_WIN32) && !defined(__CYGWIN__)
    closesocket(srv);
#else
    close(srv);
#endif
    return NULL;
}

/* ============================================================================
 * DIAGNOSTIC CLI — Supervisor (UART / Terminal)
 * design.md §14 — monitor> commands
 * ============================================================================ */
static void cli_status(void) {
    printf("\n=======================================================\n");
    printf("   NODE 2 — SUPERVISOR STATUS\n");
    printf("=======================================================\n");
    printf(" Platform         : %s\n", PLATFORM_NAME);
    printf(" Health LED       : %s\n", hal_led_str());
    printf(" Node 1 Link      : %s\n",
           g.node1_connected ? "\033[32mCONNECTED\033[0m" : "\033[31mDISCONNECTED\033[0m");
    printf(" Node 1 CPU       : %.1f %%\n", g.remote_cpu_pct);
    printf(" Active Faults    : %u\n", g.fault_counter);
    printf(" Uptime           : " FMT_U64 " ms\n", (uint64_t)(get_time_us() / 1000));
    printf("=======================================================\n");
}

static void cli_nodes(void) {
    printf("\n--- NODE CONNECTIVITY ---\n");
    printf(" Node 1 (Workload) : %s\n",
           g.node1_connected ? "\033[32mONLINE\033[0m" : "\033[31mOFFLINE\033[0m");
    printf(" Node 2 (Supervisor) : LOCAL\n");
    if (g.node1_connected) {
        uint64_t age = (get_time_us() - g.node1_last_rx_us) / 1000;
        printf(" Last Telemetry    : " FMT_U64 " ms ago\n", (uint64_t)age);
    }
}

static void cli_tasks(void) {
    printf("\n--- NODE 1 TASK STATUS (Remote Mirror) ---\n");
    printf("%-4s  %-22s  %-10s  %-10s  %-6s  %-6s\n",
           "ID", "NAME", "STATE", "EXEC_US", "MISS", "HB");
    printf("--------------------------------------------------------------------\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        TaskControlBlock *t = &g.remote_tasks[i];
        if (t->task_id == 0) { printf("  (No data yet for task %d)\n", i+1); continue; }
        const char *st = (t->state == TASK_STATE_STARVED) ? "\033[31mSTARVED\033[0m" :
                         (t->state == TASK_STATE_WARNING) ? "\033[33mWARNING\033[0m" :
                         (t->state == TASK_STATE_RUNNING) ? "RUNNING" : "STOPPED";
        printf("%-4u  %-22s  %-10s  %-10u  %-6u  %-6u\n",
               t->task_id, t->name, st, t->last_exec_us, t->deadline_misses, t->heartbeat);
    }
    printf("--------------------------------------------------------------------\n");
}

static void cli_cpu(void) {
    printf("\n--- CPU ANALYTICS (Node 1 Remote) ---\n");
    printf(" Current  : %.1f %%\n",  g.remote_cpu_pct);
    printf(" Min      : %.1f %%\n",  g.cpu_min);
    printf(" Max      : %.1f %%\n",  g.cpu_max);
    printf(" Average  : %.1f %%\n",  g.cpu_avg);
    printf(" P95      : %.1f %%\n",  g.cpu_p95);
    printf(" Samples  : %u\n", g.cpu_hist_count);
    printf(" Threshold: WARN=%.0f%%  CRIT=%.0f%%\n", CPU_WARN_THRESHOLD, CPU_CRIT_THRESHOLD);
}

static void cli_ipc(void) {
    printf("\n--- IPC LATENCY (Received from Node 1) ---\n");
    printf(" Min  : %u us\n", g.remote_ipc.min_us);
    printf(" Max  : %u us\n", g.remote_ipc.max_us);
    printf(" Avg  : %u us\n", g.remote_ipc.avg_us);
    printf(" P95  : %u us\n", g.remote_ipc.p95_us);
}

static void cli_faultmap(void) {
    printf("\n==================== FAULT MAP ====================\n");
    printf(" NODE 1 (Workload)  :  %s\n",
           g.node1_connected ? "\033[32mONLINE\033[0m" : "\033[31mOFFLINE\033[0m");
    for (int i = 0; i < MAX_TASKS; i++) {
        TaskControlBlock *t = &g.remote_tasks[i];
        if (t->task_id == 0) continue;
        const char *ts = (t->state == TASK_STATE_STARVED) ? "\033[31mSTARVED\033[0m" :
                         (t->state == TASK_STATE_WARNING) ? "\033[33mWARNING\033[0m" : "\033[32mOK\033[0m";
        printf("   ├── %-22s  %s  (deadline misses: %u)\n", t->name, ts, t->deadline_misses);
    }
    printf(" NODE 2 (Supervisor) : LOCAL OK\n");
    printf("\n Active Fault Records:\n");
    bool any = false;
    for (int i = 0; i < MAX_ACTIVE_FAULTS; i++) {
        FaultRecord *f = &g.fault_map[i];
        if (!f->active) continue;
        any = true;
        const char *sv = (f->severity == SEV_CRITICAL) ? "\033[31mCRIT\033[0m" : "\033[33mWARN\033[0m";
        printf("   [%s] #%u N%u/T%u | %s\n",
               sv, f->fault_id, f->node_id, f->task_id, f->description);
    }
    if (!any) printf("   No active faults.\n");
    printf("====================================================\n");
}

static void cli_help(void) {
    printf("\nNODE 2 SUPERVISOR — Diagnostic Commands:\n");
    printf("  status                 Global supervisor status & LED\n");
    printf("  nodes                  Node connectivity overview\n");
    printf("  tasks                  Remote Node 1 task mirror\n");
    printf("  cpu                    CPU analytics (min/max/avg/p95)\n");
    printf("  ipc                    IPC latency received from Node 1\n");
    printf("  faults / faultmap      Full fault map (all nodes & tasks)\n");
    printf("  clear                  Clear all active faults\n");
    printf("  exit                   Shutdown supervisor\n\n");
}

static void* cli_thread(void *arg) {
    (void)arg;
    char line[128];
    printf("\n***  NODE 2 (Supervisor) — RTOS Diagnostic CLI  ***\n");
    printf("    Waiting for Node 1 telemetry on port %d ...\n", g.tcp_port);
    printf("    Type 'help' for available commands.\n\n");

    while (g.running) {
        printf("supervisor> "); fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\r\n")] = 0;
        if (!strlen(line)) continue;

        if      (!strcmp(line, "help"))                cli_help();
        else if (!strcmp(line, "status"))              cli_status();
        else if (!strcmp(line, "nodes"))               cli_nodes();
        else if (!strcmp(line, "tasks"))               cli_tasks();
        else if (!strcmp(line, "cpu"))                 cli_cpu();
        else if (!strcmp(line, "ipc"))                 cli_ipc();
        else if (!strcmp(line,"faults") || !strcmp(line,"faultmap")) cli_faultmap();
        else if (!strcmp(line, "clear")) {
            fault_clear_all();
            printf("[CLI] All faults cleared.\n");
        }
        else if (!strcmp(line, "exit") || !strcmp(line, "quit")) {
            g.running = false; break;
        }
        else printf("Unknown command. Type 'help'.\n");
    }
    return NULL;
}

/* ============================================================================
 * MAIN — Node 2 Entry Point
 * ============================================================================ */
int main(int argc, char *argv[]) {
    memset(&g, 0, sizeof(g));
    g.running   = true;
    g.node_id   = 2;
    g.led_state = LED_GREEN;
    g.tcp_port  = TELEMETRY_PORT;
    g.cpu_min   = 100.0f;

    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--port=", 7)) g.tcp_port = atoi(argv[i]+7);
    }

#if defined(_WIN32) && !defined(__CYGWIN__)
    WSADATA ws; WSAStartup(MAKEWORD(2,2), &ws);
#endif

    RT_MUTEX_INIT(&g.fault_mutex);
    RT_MUTEX_INIT(&g.gantt_mutex);
    RT_MUTEX_INIT(&g.analytics_mutex);

    printf("===============================================================\n");
    printf(" %s — NODE 2 (Supervisor Node)\n", PLATFORM_NAME);
    printf(" Listening on TCP port : %d\n", g.tcp_port);
    printf("===============================================================\n");

    /* Initialize GPIO hardware (LEDs + 3 input buttons) */
    hal_gpio_init();

    rt_thread_t th_tcp, th_gpio, th_cli;
    rt_thread_create(&th_tcp,  PRIORITY_TELEMETRY,      telemetry_server_thread, NULL);
    /* GPIO poll thread — level-based fault simulation via physical buttons */
    rt_thread_create(&th_gpio, PRIORITY_FAULT_DETECTOR, hal_gpio_poll_thread,
                     (void*)gpio_button_callback);
    rt_thread_create(&th_cli,  PRIORITY_CLI,            cli_thread,             NULL);

    rt_thread_join(th_cli);
    g.running = false;
    sleep_ms(500);
    hal_gpio_deinit();    /* Turn off LEDs, release GPIO memory map */

#if defined(_WIN32) && !defined(__CYGWIN__)
    WSACleanup();
#endif
    return 0;
}
