# UrbanPulse-QNX: Dual-Node Real-Time Fault & Performance Monitoring Platform
> **U**nified **R**eal-time **B**it-level **A**nalytics & **N**ode **P**erformance **U**nder **L**atency **S**upervisory **E**ngine

A distributed, mission-critical RTOS monitoring platform developed for **QNX Neutrino RTOS 8.0** across dual **Raspberry Pi 4** nodes. The system models smart city municipal services (adaptive traffic control, water distribution, environmental sensing) with deterministic scheduling, physical level-based GPIO fault injection, and live microsecond-resolution telemetry streaming.

---

## Architecture Overview

```
                      ETHERNET (100 Mbps / TCP 5555)
┌───────────────────────────────┐        ┌───────────────────────────────┐
│   RASPBERRY PI 1 (Workload)   │        │  RASPBERRY PI 2 (Supervisor)  │
│          192.168.1.1          │───────>│          192.168.1.2          │
├───────────────────────────────┤        ├───────────────────────────────┤
│ • Service A: Traffic Control  │        │ • Telemetry Ingestion Server  │
│ • Service B: Water Grid       │        │ • Real-time Task Mirror       │
│ • Service C: Air Quality      │        │ • CPU & Latency Analytics     │
│ • Monitoring Task (Deadlines) │        │ • Fault Map Engine (Tree)     │
│ • Level-Based GPIO Buttons    │        │ • Health Indicator LED Driver │
│ • Interactive CLI Dashboard   │        │ • Remote Diagnostic CLI       │
└───────────────────────────────┘        └───────────────────────────────┘
```

The system is separated into two decoupled real-time nodes:
1. **Node 1 (`node1`) — Workload Engine**: Executes real-time periodic municipal tasks under fixed-priority preemptive scheduling (RMS), tracks deadline compliance, monitors QNX Message Passing heartbeats, manages shared-memory IPC, reads physical button inputs, and streams JSON telemetry packets every 500 ms.
2. **Node 2 (`supervisor`) — Supervisor Engine**: Ingests telemetry over TCP, maintains mirrored task control blocks (TCBs), calculates statistical analytics (Min, Max, Avg, P95 latencies and CPU utilization), evaluates system-wide health, drives hardware tri-color status LEDs, and serves an interactive diagnostic CLI.

---

## Hardware GPIO Pinout & Wiring Table

Both boards (Raspberry Pi 1 and Raspberry Pi 2) share the **exact same physical pin layout** on the 40-pin header.

### A. Tri-Color Health LEDs (Outputs)

| LED Color | BCM GPIO Number | Physical Header Pin | Wiring Instructions |
| :--- | :--- | :--- | :--- |
| **🟢 GREEN** | **GPIO 17** | **Physical Pin 11** | Long leg (+) to Pin 11, Short leg (-) through $220\Omega$ resistor to GND (Pin 9) |
| **🟡 YELLOW** | **GPIO 27** | **Physical Pin 13** | Long leg (+) to Pin 13, Short leg (-) through $220\Omega$ resistor to GND (Pin 9) |
| **🔴 RED** | **GPIO 22** | **Physical Pin 15** | Long leg (+) to Pin 15, Short leg (-) through $220\Omega$ resistor to GND (Pin 9) |

> ⚠️ **Ground (GND) Pins available on Raspberry Pi:** Physical Pin 6, 9, 14, 20, 25, 30, 34, 39.

---

### B. Push-Buttons / Jumper Wire Pins (Inputs)

All button pins are configured with **internal pull-up resistors** in `hal_gpio.c`:
* **Default (Floating / Released):** Pin reads `HIGH` ($3.3\text{V}$) $\rightarrow$ System is **NORMAL / HEALTHY**.
* **Pressed (Contact with GND):** Pin pulled `LOW` ($0\text{V}$) $\rightarrow$ **FAULT ACTIVE** while contact is held.
* **Released (GND removed):** Pin returns `HIGH` $\rightarrow$ **FAULT AUTO-CLEARS** (Self-healing).

| Button | BCM GPIO | Physical Header Pin | Connect To |
| :--- | :--- | :--- | :--- |
| **Button 1** | **GPIO 23** | **Physical Pin 16** | One leg to Pin 16, other leg to GND (Pin 14) |
| **Button 2** | **GPIO 24** | **Physical Pin 18** | One leg to Pin 18, other leg to GND (Pin 20) |
| **Button 3** | **GPIO 25** | **Physical Pin 22** | One leg to Pin 22, other leg to GND (Pin 25) |

---

## Health LED Indications: When Do They Turn ON & OFF?

The RTOS health engine evaluates active faults every cycle and drives the LEDs according to strict priority:

$$\mathbf{\color{red}CRITICAL\ (RED)} \quad>\quad \mathbf{\color{orange}WARNING\ (YELLOW)} \quad>\quad \mathbf{\color{green}HEALTHY\ (GREEN)}$$

### 🟢 GREEN LED (Healthy Baseline)
* **When it turns ON:**
  * System boot when all services run nominally.
  * All active tasks meet deadlines and report heartbeats on schedule.
  * Total CPU $< 70\%$ and IPC latency $\le 220\text{ ms}$.
  * Held buttons are released or faults cleared via `clear` CLI command.
* **When it turns OFF:**
  * Immediately when any Warning (Yellow) or Critical (Red) fault occurs.

### 🟡 YELLOW LED (Warning State)
* **When it turns ON:**
  * **On Node 1:**
    * **Deadline Miss:** Service A, B, or C execution exceeds its deadline (e.g., Service A $> 80\text{ ms}$).
    * **Slow Heartbeat:** A service heartbeat is delayed by $> 2\times$ its period.
    * **Elevated CPU:** Total CPU load is between $70.0\%$ and $84.9\%$.
    * **Elevated IPC:** Inter-task transfer latency is between $220\text{ ms}$ and $300\text{ ms}$.
  * **On Supervisor (Node 2):**
    * Automatically turns Yellow within $500\text{ ms}$ when telemetry reports any of the above warnings from Node 1.
    * Simulated via Supervisor Button 3 (Pin 22).
* **When it turns OFF:**
  * When the deadline overrun clears (returns to Green), OR when an escalating Critical fault turns the Red LED ON.

### 🔴 RED LED (Critical Safety Fault)
* **When it turns ON:**
  * **On Node 1:**
    * **Task Starvation:** Service B (or any task) fails to report a heartbeat for $\ge 3\times$ its period ($600\text{ ms}$ silence).
    * **CPU Overload:** Total CPU utilization $\ge 85.0\%$.
    * **IPC Latency Critical:** Shared-memory queue latency $\ge 300\text{ ms}$.
  * **On Supervisor (Node 2):**
    * **Node 1 Disconnected:** No telemetry packet received for $> 2\text{ seconds}$ (Ethernet cable pulled or Node 1 stopped).
    * Telemetry reports remote task starvation or CPU overload $\ge 85\%$.
    * Simulated via Supervisor Button 1 (Pin 16) or Button 2 (Pin 18).
* **When it turns OFF:**
  * When heartbeats resume, Node 1 reconnects, or buttons are released.

---

## Push-Button Functionality on Each Board

### Board 1: Node 1 (Workload Pi)

| Push-Button | Pin | Hardware Action | Result on Node 1 LED | Result on Supervisor LED |
| :--- | :--- | :--- | :--- | :--- |
| **Button 1** | **Pin 16** (GPIO 23) | **Hold to GND** | 🔴 **RED LED ON** *(Service B Starved)* | 🔴 **RED LED ON** *(Propagated over TCP)* |
| | | **Release** | 🟢 **GREEN LED restores** | 🟢 **GREEN LED restores** |
| **Button 2** | **Pin 18** (GPIO 24) | **Hold to GND** | 🔴 **RED LED ON** *(CPU Overload $>85\%$)* | 🔴 **RED LED ON** *(Propagated over TCP)* |
| | | **Release** | 🟢 **GREEN LED restores** | 🟢 **GREEN LED restores** |
| **Button 3** | **Pin 22** (GPIO 25) | **Hold to GND** | 🟡 **YELLOW LED ON** *(Service A Deadline Miss)* | 🟡 **YELLOW LED ON** *(Propagated over TCP)* |
| | | **Release** | 🟢 **GREEN LED restores** | 🟢 **GREEN LED restores** |

### Board 2: Supervisor (Supervisor Pi)

| Push-Button | Pin | Hardware Action | Result on Supervisor LED |
| :--- | :--- | :--- | :--- |
| **Button 1** | **Pin 16** (GPIO 23) | **Hold to GND** | 🔴 **RED LED ON** *(Simulates Node 1 Disconnect)* |
| | | **Release** | 🟢 **GREEN LED restores** *(Link Restored)* |
| **Button 2** | **Pin 18** (GPIO 24) | **Hold to GND** | 🔴 **RED LED ON** *(Simulates Remote IPC Timeout)* |
| | | **Release** | 🟢 **GREEN LED restores** |
| **Button 3** | **Pin 22** (GPIO 25) | **Hold to GND** | 🟡 **YELLOW LED ON** *(Simulates CPU Overload Warning)* |
| | | **Release** | 🟢 **GREEN LED restores** |

---

## Shared-Memory IPC Latency Analytics (Min / Max / Avg / P95)

When executing `ipc` on either node:
```text
--- IPC LATENCY (Received from Node 1) ---
 Min  : 184002 us
 Max  : 184016 us
 Avg  : 184008 us
 P95  : 184015 us
```

### What This Represents
1. **Producer:** Service A (Traffic Sensor Task, runs every **100 ms**) produces telemetry, timestamps it (`send_time_us`), and writes to shared memory `/smart_city_metrics`.
2. **Consumer:** Service B (Power Grid Task, runs every **200 ms**) consumes the message on its next cycle and timestamps it (`recv_time_us`).
3. **Queue Latency:** Calculated as $\Delta t = t_\text{recv} - t_\text{send} \approx 184\text{ ms}$ (representing the message queueing interval until Service B's 200 ms periodic timer wakes it up).
4. **Statistical Distribution:**
   * **Min / Max:** Range of observed delivery times.
   * **Avg:** Moving arithmetic mean over the last 100 samples.
   * **P95:** 95th percentile (95% of all transfers completed under this threshold).
5. **Zero Jitter:** The variance between Min ($184002\ \mu\text{s}$) and Max ($184016\ \mu\text{s}$) is only **$14\ \mu\text{s}$ ($0.014\text{ ms}$)**, proving the microsecond determinism of the QNX Neutrino RTOS scheduler.

---

## Threading & Priority Matrix

| Node | Thread Name | Priority | Scheduling | Role |
|---|---|:---:|:---:|---|
| **Pi 1** | `heartbeat_receiver_thread` | **25** | `SCHED_FIFO` | QNX MsgReceive() heartbeat detector |
| **Pi 1** | `hal_gpio_poll_thread` | **25** | `SCHED_FIFO` | Hardware button polling (50ms debounce) |
| **Pi 1** | `monitoring_task_thread` | **20** | `SCHED_FIFO` | Periodic health & CPU sampler (100ms cycle) |
| **Pi 1** | `service_a_thread` | **15** | `SCHED_FIFO` | Traffic Task (Period: 100ms, Deadline: 80ms) |
| **Pi 1** | `service_b_thread` | **14** | `SCHED_FIFO` | Grid Task (Period: 200ms, Deadline: 150ms) |
| **Pi 1** | `service_c_thread` | **13** | `SCHED_FIFO` | Environmental Task (Period: 500ms, Deadline: 400ms) |
| **Pi 1** | `telemetry_sender_thread`| **10** | `SCHED_FIFO` | TCP client streaming telemetry (500ms cycle) |
| **Pi 1** | `cpu_burner_thread` | **5** | `SCHED_FIFO` | Controllable CPU load generator |
| **Pi 1** | `cli_thread` | **5** | `SCHED_FIFO` | Interactive diagnostic terminal shell |
| **Pi 2** | `supervisor_monitor_thread`| **20** | `SCHED_FIFO` | Node 1 watchdog & local CPU profiler |
| **Pi 2** | `hal_gpio_poll_thread` | **25** | `SCHED_FIFO` | Hardware simulation button polling |
| **Pi 2** | `telemetry_server_thread`| **10** | `SCHED_FIFO` | TCP server receiver & packet deserializer |
| **Pi 2** | `cli_thread` | **5** | `SCHED_FIFO` | Interactive supervisor diagnostic shell |

---

## Project Structure

```
smart_city/
├── src/
│   ├── smart_city_common.h   # Shared headers, IPC structures, thresholds, priorities
│   ├── hal_gpio.c            # BCM2711 memory-mapped GPIO driver & polling loop
│   ├── node1.c               # Workload node (Tasks A/B/C, monitors, TCP client)
│   └── supervisor.c          # Supervisor node (TCP server, analytics, fault map)
├── build/                    # Compiled aarch64le QNX binaries
│   ├── node1                 # Binary for Raspberry Pi 1
│   └── supervisor            # Binary for Raspberry Pi 2
├── Makefile                  # Cross-compilation build file (qcc -Vgcc_ntoaarch64le)
├── GPIO_HARDWARE_GUIDE.md    # Standalone 3-minute physical demo & test runbook
├── testing_guide.md          # Comprehensive deployment & network setup guide
├── .gitignore                # Git ignore rules for QNX development
└── README.md                 # Project documentation
```

---

## Quick Start

### 1. Build the Binaries
Using the QNX Momentics IDE or command line with QNX SDP 8.0 / 7.1:
```bash
make all
```
Binaries are generated in `build/aarch64le-debug/node1` and `build/aarch64le-debug/supervisor`.

### 2. Deploy to Hardware
Transfer the binaries to both Raspberry Pis via `scp`:
```bash
# On your build workstation:
scp build/aarch64le-debug/node1 root@169.254.120.15:/tmp/
scp build/aarch64le-debug/supervisor root@169.254.178.16:/tmp/
```

### 3. Run the Nodes
* **On Pi 2 (Supervisor)**:
  ```bash
  cd /tmp
  ./supervisor
  ```
* **On Pi 1 (Workload Node)**:
  ```bash
  cd /tmp
  ./node1 --ip=169.254.178.16 --port=5555
  ```

---

## Interactive CLI Dashboards

Both nodes feature an integrated interactive real-time shell:

* **Node 1 Workload Shell (`node1>`)**:
  * `status`: Global workload status, active fault count, uptime, shared memory
  * `tasks`: Live task execution times, deadline misses, heartbeat count
  * `cpu`: Clean CPU analytics (Min, Max, Avg, P95)
  * `ipc`: Shared-memory IPC latency statistics (Min, Max, Avg, P95)
  * `inject <starve|deadline|cpu>`: Software fault injection
  * `clear`: Clear all active faults

* **Node 2 Supervisor Shell (`supervisor>`)**:
  * `status`: Global cluster health, remote link status, active fault count
  * `nodes`: Node connectivity overview
  * `tasks`: Live mirrored telemetry table of all remote tasks
  * `cpu`: Local and remote CPU analytics (Min, Max, Avg, P95)
  * `ipc`: Remote IPC latency received from Node 1
  * `faults`: Hierarchical fault map across all cluster components
  * `clear`: Clear all registered faults

