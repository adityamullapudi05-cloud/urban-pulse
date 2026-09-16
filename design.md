# Smart City RTOS Fault & Performance Monitoring Platform --- Design

## User Requirement

Smart City RTOS Fault & Performance Monitoring Platform

> Monitor task health, CPU utilization, IPC latency and system faults
> across multiple city services.\
> QNX Raspberry Pi 4/5 Cluster.\
> CAN/Ethernet, GPIO LEDs, UART.\
> Build a supervisory platform capable of detecting task starvation,
> deadline misses, CPU overload, IPC delays and service failures across
> distributed nodes. CAN/Ethernet provides node communication, GPIO
> indicates system health and UART provides diagnostic access.\
> Monitoring Task (High), Trace Collector, Fault Detector (Highest),
> Analytics.\
> RTOS Monitoring, Fault Detection, Distributed Diagnostics.\
> Shared Memory + TCP.\
> Sampling, Heartbeat, Fault Detection.\
> Gantt Timeline, CPU/IPC Metrics, Fault Map.\
> CLI Mandatory.

User has two Raspberry Pis.

------------------------------------------------------------------------

# Proposed Architecture

Use a 2-node distributed QNX RTOS monitoring platform:

-   **Raspberry Pi 1 --- Monitored Workload Node**
-   **Raspberry Pi 2 --- Monitoring/Supervisor Node**
-   Ethernet between the two
-   UART for CLI/debug access
-   GPIO LEDs for health indication
-   Shared memory for local IPC
-   TCP for inter-node communication

``` text
                 ┌─────────────────────────────┐
                 │ Raspberry Pi 1              │
                 │ MONITORED NODE              │
                 │                             │
                 │ ┌─────────────┐             │
                 │ │ Service A   │             │
                 │ ├─────────────┤             │
                 │ │ Service B   │             │
                 │ ├─────────────┤             │
                 │ │ Service C   │             │
                 │ └─────────────┘             │
                 │       │                     │
                 │       ▼                     │
                 │ Monitoring Task             │
                 │       │                     │
                 │       ▼                     │
                 │ Trace Collector             │
                 │       │                     │
                 │       ▼                     │
                 │ TCP Telemetry ──────────────┼──── Ethernet
                 └─────────────────────────────┘
                                                  │
                                                  ▼
                 ┌─────────────────────────────┐
                 │ Raspberry Pi 2              │
                 │ SUPERVISOR / ANALYTICS NODE │
                 │                             │
                 │ TCP Receiver                │
                 │       │                     │
                 │       ▼                     │
                 │ Fault Detector              │
                 │       │                     │
                 │       ▼                     │
                 │ Analytics Engine            │
                 │       ├── CPU Metrics       │
                 │       ├── IPC Metrics       │
                 │       ├── Fault Map         │
                 │       └── Gantt Timeline     │
                 │                             │
                 │ CLI                         │
                 │ UART                        │
                 │ GPIO LEDs                   │
                 └─────────────────────────────┘
```

------------------------------------------------------------------------

# 1. Requirements to Demonstrate

  Requirement               Implementation
  ------------------------- -----------------------------------------------------
  Task health               Monitor periodic tasks
  CPU utilization           Measure CPU busy/idle time
  IPC latency               Measure shared-memory/message communication latency
  Task starvation           Detect task that stops responding
  Deadline miss             Detect task execution beyond deadline
  CPU overload              Detect sustained CPU utilization above threshold
  Service failure           Heartbeat + fault detection
  Distributed diagnostics   Send telemetry over Ethernet
  Gantt timeline            Record task execution intervals
  CPU/IPC metrics           Calculate and display statistics
  Fault map                 Show active faults by node/task
  CLI                       Commands for monitoring/debugging
  GPIO                      Green/yellow/red system health
  UART                      Diagnostic console

------------------------------------------------------------------------

# 2. Raspberry Pi Responsibilities

## Raspberry Pi 1 --- RTOS Workload Node

Suggested processes/components:

``` text
Node 1
│
├── Task_Manager
├── Service_A
├── Service_B
├── Service_C
├── Monitoring_Task
├── Trace_Collector
├── Telemetry
└── Fault_Injection
```

## Raspberry Pi 2 --- Supervisor

``` text
Node 2
│
├── TCP_Receiver
├── Fault_Detector
├── Analytics
├── Gantt_Recorder
├── Fault_Map
└── CLI
```

This separation keeps workload generation and supervisory analytics
independent.

------------------------------------------------------------------------

# 3. Workload Services

Create three periodic QNX services.

Example:

### Service A

``` text
Period   = 100 ms
Deadline = 80 ms
```

### Service B

``` text
Period   = 200 ms
Deadline = 150 ms
```

### Service C

``` text
Period   = 500 ms
Deadline = 400 ms
```

Each service generates a heartbeat.

Example status structure:

``` c
typedef struct
{
    uint32_t task_id;
    uint32_t heartbeat;
    uint64_t timestamp;
    uint32_t execution_time;
    uint32_t deadline;
    uint8_t status;
} TaskStatus;
```

------------------------------------------------------------------------

# 4. Monitoring Task

The Monitoring Task should run at high priority and periodically check:

-   Task heartbeat
-   Task execution time
-   Task deadline
-   CPU utilization
-   IPC latency
-   Task state

Conceptual priority structure:

``` text
Fault Detector       Highest
Monitoring Task      High
Service A            Medium
Service B            Medium
Service C            Medium
Background           Low
```

The monitoring task can sample every 100 ms initially.

------------------------------------------------------------------------

# 5. Heartbeat Detection

Each service increments a heartbeat counter.

Example:

``` text
Service A

Heartbeat:
1
2
3
4
5
6
...
```

The Monitoring Task stores the previous heartbeat value.

If the heartbeat does not change for too many monitoring cycles:

``` text
current_heartbeat == previous_heartbeat
```

then the task is considered stalled/starved.

Example project rule:

``` text
Allowed heartbeat timeout = 300 ms

100 ms → OK
200 ms → OK
300 ms → WARNING
400 ms → FAULT
```

The exact threshold should be configurable.

------------------------------------------------------------------------

# 6. Deadline-Miss Detection

For each task, record:

``` text
start_time
    ↓
task execution
    ↓
end_time
```

Calculate:

``` text
execution_time = end_time - start_time
```

Then:

``` c
if (execution_time > deadline)
    deadline_miss++;
```

Example:

``` text
Service A

12 ms
15 ms
14 ms
82 ms  ← DEADLINE MISS
13 ms
```

Analytics example:

``` text
Service A
Executions:      500
Deadline misses: 3
Max execution:   82 ms
Average:         14.2 ms
```

------------------------------------------------------------------------

# 7. CPU Utilization

Start with application-level measurements, then add system-level QNX
measurements.

Example:

``` text
Total monitoring window = 1 second

Task A = 150 ms
Task B = 220 ms
Task C = 100 ms
```

Approximate workload CPU utilization:

``` text
47%
```

Display:

``` text
CPU UTILIZATION

Node 1
----------------
CPU:       43.7%
Service A: 12.3%
Service B: 18.2%
Service C:  8.4%
Other:      4.8%
```

Use configurable application thresholds, for example:

``` text
< 70%       NORMAL
70–85%      WARNING
> 85%       OVERLOAD
```

These thresholds are project-defined, not QNX standards.

------------------------------------------------------------------------

# 8. IPC Latency

Use shared memory for local IPC on Raspberry Pi 1.

Concept:

``` text
Service A
    ↓
Shared Memory
    ↓
Service B
```

Timestamp the send/write and receive/read operations:

``` text
t1 = message written
t2 = message received
```

Then:

``` text
IPC latency = t2 - t1
```

Collect:

-   Minimum
-   Maximum
-   Average
-   P95

Example output:

``` text
Shared Memory IPC

Average:    42 us
Minimum:    18 us
Maximum:   127 us
P95:         71 us
```

------------------------------------------------------------------------

# 9. TCP Communication

Connect the two Raspberry Pis over Ethernet:

``` text
Pi 1 ───────── Ethernet ───────── Pi 2
```

Pi 1 periodically sends telemetry.

For the first prototype, JSON is easiest to debug:

``` json
{
  "node": 1,
  "timestamp": 12345678,
  "cpu": 43.7,
  "tasks": [
    {
      "id": 1,
      "name": "Service_A",
      "heartbeat": 120,
      "exec_us": 14200,
      "deadline_miss": 0,
      "status": "OK"
    }
  ]
}
```

Later, use a binary protocol if performance or bandwidth requires it.

------------------------------------------------------------------------

# 10. Telemetry Pipeline

Pi 1:

``` text
Task data
   ↓
Monitoring Task
   ↓
Trace Collector
   ↓
Telemetry Buffer
   ↓
TCP
   ↓
Ethernet
```

Pi 2:

``` text
TCP
 ↓
Telemetry Receiver
 ↓
Parser
 ↓
Fault Detector
 ↓
Analytics
```

------------------------------------------------------------------------

# 11. Fault Detector

Implement a dedicated Fault Detector with:

``` text
Fault Detector
│
├── Heartbeat failure
├── Deadline miss
├── CPU overload
├── IPC timeout
├── TCP connection loss
└── Task/service failure
```

Suggested fault types:

``` c
typedef enum
{
    FAULT_NONE,
    FAULT_TASK_STARVATION,
    FAULT_DEADLINE_MISS,
    FAULT_CPU_OVERLOAD,
    FAULT_IPC_TIMEOUT,
    FAULT_SERVICE_FAILURE,
    FAULT_NODE_DISCONNECTED
} FaultType;
```

Suggested event structure:

``` c
typedef struct
{
    uint32_t node_id;
    uint32_t task_id;
    FaultType type;
    uint64_t timestamp;
    uint32_t severity;
} FaultEvent;
```

------------------------------------------------------------------------

# 12. Fault Severity

Use:

``` text
INFO
WARNING
CRITICAL
```

Example project-defined rules:

``` text
CPU > 70%
    ↓
WARNING

CPU > 85%
    ↓
CRITICAL
```

Heartbeat:

``` text
1 missed heartbeat
    ↓
WARNING

3 consecutive missed heartbeats
    ↓
CRITICAL
```

All thresholds should be configurable.

------------------------------------------------------------------------

# 13. GPIO Health Indicators

Use three LEDs:

``` text
GPIO
 │
 ├── Green  → System Healthy
 ├── Yellow → Warning
 └── Red    → Critical Fault
```

Examples:

``` text
Normal:
GREEN ON

Warning:
YELLOW ON

Critical:
RED ON
```

Red can blink for an active critical fault.

------------------------------------------------------------------------

# 14. UART Diagnostic CLI

Create a command-line interface accessible through UART.

Example:

``` text
monitor>
```

Commands:

``` text
help
status
tasks
cpu
ipc
faults
trace
nodes
clear
reset
watch
```

Example:

``` text
monitor> status

SYSTEM STATUS
------------------------
Node 1       ONLINE
Node 2       ONLINE

CPU          43.7 %
IPC          42 us
Faults       0
Services     3/3
System       HEALTHY
```

Task command:

``` text
monitor> tasks

ID   NAME        STATE       CPU       DEADLINE   HB
-------------------------------------------------------
1    Service_A   RUNNING     12.3%     14ms       1250
2    Service_B   RUNNING     18.2%     32ms       625
3    Service_C   RUNNING      8.4%     21ms       250
```

------------------------------------------------------------------------

# 15. Gantt Timeline

Record task execution intervals.

``` text
Time →
0       10      20      30      40      50 ms

Task A  ████
Task B          ██████
Task C                  ███
Task A                         ████
Task B                              █████
```

Store trace records such as:

``` text
timestamp
task_id
event
```

Example:

``` text
1000234, Service_A, START
1000312, Service_A, END
1000450, Service_B, START
1000620, Service_B, END
```

Initially, export the trace to the PC and generate a Gantt chart there
rather than building a graphical UI directly on QNX.

------------------------------------------------------------------------

# 16. Fault Map

Supervisor maintains a node/task fault map:

``` text
FAULT MAP
──────────────────────────────────

Node 1
 ├── Service A     OK
 ├── Service B     WARNING
 │      └── Deadline Miss
 └── Service C     OK

Node 2
 └── Supervisor    OK
```

If Pi 1 disconnects:

``` text
FAULT MAP

Node 1
 └── NODE DISCONNECTED

Node 2
 └── SUPERVISOR OK
```

------------------------------------------------------------------------

# 17. Fault Injection

Fault injection is essential for demonstrating that the monitoring
system actually detects faults.

Create:

``` text
Fault Injection
│
├── CPU overload
├── Task starvation
├── Deadline miss
├── IPC delay
├── Service stop
└── Network disconnect
```

## Task starvation

Stop updating Service B's heartbeat.

Expected:

``` text
Service B heartbeat timeout
        ↓
Fault Detector
        ↓
TASK STARVATION
        ↓
Yellow/Red LED
        ↓
CLI alert
```

## Deadline miss

Artificially increase execution time.

Example:

``` text
Normal:
15 ms

Injected:
100 ms
```

With:

``` text
Deadline = 50 ms
```

Expected:

``` text
DEADLINE MISS
```

## CPU overload

Create a computational workload.

Expected progression:

``` text
45%
 ↓
65%
 ↓
82%
 ↓
92%
```

Detector:

``` text
CPU OVERLOAD
```

## IPC delay

Inject a delay into the IPC path.

Example:

``` text
Normal → ~40 us
Fault   → 100+ ms
```

Detector:

``` text
IPC DELAY
```

## Service failure

Terminate a service:

``` text
kill Service_C
```

Supervisor detects heartbeat failure and reports service failure.

## Network failure

Disconnect Ethernet.

Expected:

``` text
TCP timeout
     ↓
Node 1 OFFLINE
     ↓
NODE DISCONNECTED
```

------------------------------------------------------------------------

# 18. Recommended Project Structure

``` text
smart-city-monitor/
│
├── node1/
│   ├── service_a/
│   ├── service_b/
│   ├── service_c/
│   ├── monitor/
│   ├── trace/
│   ├── telemetry/
│   └── fault_injection/
│
├── node2/
│   ├── tcp_receiver/
│   ├── fault_detector/
│   ├── analytics/
│   ├── gantt/
│   ├── fault_map/
│   └── cli/
│
├── common/
│   ├── protocol.h
│   ├── task_info.h
│   ├── fault.h
│   └── telemetry.h
│
└── docs/
    ├── architecture
    ├── test_plan
    └── results
```

------------------------------------------------------------------------

# 19. Development Plan

Do not build the whole platform at once. Build it in phases.

## Phase 1 --- QNX Basics

Verify on both Raspberry Pis:

``` text
SSH
Networking
Processes
Threads
Priorities
Timers
Mutexes
Shared memory
Sockets
UART
GPIO
```

## Phase 2 --- Multi-task Workload

On Pi 1 create:

``` text
Service A
Service B
Service C
```

with different periods and priorities.

## Phase 3 --- Monitoring

Implement:

``` text
Monitoring Task
      ↓
Heartbeat
      ↓
Execution time
      ↓
Deadline monitoring
```

Do not worry about Pi 2 yet.

## Phase 4 --- IPC

Implement:

``` text
Service A
    ↕
Shared Memory
    ↕
Service B
```

Measure IPC latency.

## Phase 5 --- Trace Collector

Add:

``` text
START event
END event
HEARTBEAT event
FAULT event
```

Store events in a ring buffer.

## Phase 6 --- Ethernet

Connect:

``` text
Pi 1 ←→ Pi 2
```

Implement TCP telemetry.

Initially send:

``` text
CPU
heartbeat
task status
faults
timestamp
```

## Phase 7 --- Fault Detector

Implement:

``` text
Heartbeat failure
Deadline miss
CPU overload
IPC timeout
Service failure
Node disconnect
```

## Phase 8 --- GPIO + UART

Implement:

``` text
Green  → Normal
Yellow → Warning
Red    → Critical
```

CLI:

``` text
status
tasks
cpu
ipc
faults
trace
```

## Phase 9 --- Analytics

Calculate:

``` text
Average
Minimum
Maximum
P95
Fault count
Deadline miss count
```

for CPU, IPC and task performance.

## Phase 10 --- Gantt + Fault Map

Complete the visualization and reporting layer.

------------------------------------------------------------------------

# 20. Final Demonstration

## Step 1 --- Normal Operation

``` text
Pi 1 → services running
Pi 2 → monitoring

CPU = 42%
IPC = 40 us
Faults = 0
```

Green LED.

## Step 2 --- Deadline Fault

Inject CPU load.

``` text
Service A
Deadline = 50 ms
Actual = 83 ms
```

System reports:

``` text
DEADLINE MISS
```

Yellow LED.

## Step 3 --- Starvation

Stop Service B heartbeat.

``` text
Service B
Heartbeat timeout
```

System reports:

``` text
TASK STARVATION
```

## Step 4 --- Network Failure

Disconnect Ethernet.

Pi 2 reports:

``` text
NODE 1 OFFLINE
```

Red LED.

## Step 5 --- Restore

Reconnect Ethernet.

``` text
Node 1 → ONLINE
Services → RUNNING
Fault → CLEARED
```

------------------------------------------------------------------------

# 21. Requirement-to-Implementation Mapping

  -----------------------------------------------------------------------
  Given Requirement                   Planned Implementation
  ----------------------------------- -----------------------------------
  Smart City RTOS Fault & Performance Distributed QNX monitoring system
  Monitoring Platform                 

  Monitor task health                 Heartbeat + task state

  CPU utilization                     CPU statistics

  IPC latency                         Shared-memory latency measurement

  System faults                       Fault event framework

  Multiple city services              Service A/B/C

  QNX                                 QNX on Raspberry Pi

  Raspberry Pi 4/5                    Two Raspberry Pis

  CAN/Ethernet                        Ethernet initially; CAN can be
                                      added

  GPIO LEDs                           Health indicator

  UART                                Diagnostic CLI

  Task starvation                     Heartbeat timeout

  Deadline misses                     Execution vs deadline

  CPU overload                        CPU threshold detection

  IPC delays                          IPC timeout/latency

  Service failures                    Process/heartbeat monitoring

  Monitoring Task                     High-priority monitor

  Trace Collector                     Event/ring-buffer collector

  Fault Detector                      Dedicated fault engine

  Analytics                           Supervisor statistics

  RTOS Monitoring                     QNX task monitoring

  Distributed Diagnostics             Pi-to-Pi TCP

  Shared Memory                       Local IPC

  TCP                                 Inter-node telemetry

  Sampling                            Periodic monitoring

  Heartbeat                           Service health

  Fault Detection                     Fault engine

  Gantt Timeline                      Trace visualization

  CPU/IPC Metrics                     Analytics

  Fault Map                           Supervisor

  CLI Mandatory                       UART/terminal CLI
  -----------------------------------------------------------------------

------------------------------------------------------------------------

# 22. Final Target Architecture

``` text
                    SMART CITY
                 RTOS SUPERVISOR
                       │
        ┌──────────────┴──────────────┐
        │                             │
     RASPBERRY PI 1              RASPBERRY PI 2
      QNX NODE                    QNX SUPERVISOR
        │                             │
 ┌──────┼──────┐                      │
 │      │      │                      │
 S-A    S-B    S-C                 TCP RX
 │      │      │                      │
 └──────┼──────┘                      ▼
        │                       FAULT DETECTOR
        ▼                             │
 MONITORING TASK                      ▼
        │                          ANALYTICS
        ├── Heartbeat                 │
        ├── CPU                       ├── CPU metrics
        ├── Deadline                  ├── IPC metrics
        ├── IPC                       ├── Fault map
        └── Task state                └── Gantt
        │
        ▼
 TRACE COLLECTOR
        │
        ▼
      TCP
        │
══════════════════════════════════════════
                 Ethernet
══════════════════════════════════════════
        │
        ▼
     SUPERVISOR

GPIO:
GREEN  = NORMAL
YELLOW = WARNING
RED    = CRITICAL

UART:
monitor> status
monitor> tasks
monitor> cpu
monitor> ipc
monitor> faults
monitor> trace
```

------------------------------------------------------------------------

# 23. Recommended Starting Point

The implementation should start with **Raspberry Pi 1**:

1.  Create three periodic QNX services.
2.  Assign priorities and periods.
3.  Add a high-priority Monitoring Task.
4.  Implement heartbeat monitoring.
5.  Implement execution-time measurement.
6.  Implement deadline-miss detection.
7.  Verify all of this on Pi 1.
8.  Then add shared-memory IPC.
9.  Then bring up Pi 2.
10. Finally implement TCP, Fault Detector, Analytics, CLI, GPIO, Gantt
    and Fault Map.

This minimizes integration risk and gives a working milestone at each
stage.
