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
1. **Node 1 (`node1_workload`) — Workload Engine**: Executes real-time periodic municipal tasks under fixed-priority preemptive scheduling (RMS), tracks deadline compliance, monitors heartbeats, reads physical button inputs, and streams binary telemetry packets every 500 ms.
2. **Node 2 (`node2_supervisor`) — Supervisor Engine**: Ingests telemetry, maintains mirrored task control blocks (TCBs), calculates statistical analytics (P95 latencies, moving average CPU utilization), evaluates system-wide health, drives hardware tri-color status LEDs, and serves an interactive diagnostic CLI.

---

## Key Technical Features

* **Strict Priority Preemption**: 11 dedicated threads across both nodes configured with distinct QNX real-time priorities (10 to 25) to prevent priority inversion and guarantee bounded execution.
* **Direct Hardware GPIO Access**: Uses QNX physical memory mapping (`mmap_device_io`) to interface directly with BCM2711 peripheral registers for zero-latency button polling and LED health indicators.
* **Level-Based Physical Fault Injection**: Physical contact switches wired to Raspberry Pi GPIOs simulate real-world failure modes (contact = fault active; release = instant autonomous recovery).
* **Deterministic Failure Recovery**: Automated recovery algorithms for thread starvation, simulated deadlocks, CPU overload bursts, and communication link loss.
* **High-Precision Telemetry**: Microsecond-resolution timestamping via monotonic hardware timers (`ClockTime` / `get_time_us()`).

---

## Threading & Priority Matrix

| Node | Thread Name | Priority | Role |
|---|---|:---:|---|
| **Pi 1** | `hal_gpio_poll_thread` | **25** | High-speed hardware button polling (20ms debounce) |
| **Pi 1** | `monitoring_task_thread` | **24** | Real-time deadline checker & heartbeat watchdog |
| **Pi 1** | `service_a_thread` | **20** | Adaptive Traffic Control (Period: 50ms, Deadline: 15ms) |
| **Pi 1** | `service_b_thread` | **18** | Water Grid Pressure Management (Period: 100ms) |
| **Pi 1** | `service_c_thread` | **15** | Air Quality & Environmental Sensing (Period: 250ms) |
| **Pi 1** | `telemetry_sender_thread`| **14** | TCP client streaming binary telemetry |
| **Pi 1** | `cli_thread` | **10** | Interactive local management shell |
| **Pi 2** | `hal_gpio_poll_thread` | **25** | Supervisor hardware button polling |
| **Pi 2** | `telemetry_server_thread`| **22** | Non-blocking TCP server receiver & unpacker |
| **Pi 2** | `fault_evaluator_thread` | **20** | Health matrix analyzer & LED state machine |
| **Pi 2** | `cli_thread` | **10** | Interactive supervisor diagnostic shell |

---

## Project Structure

```
smart_city/
├── src/
│   ├── smart_city_common.h   # Shared structs, telemetry packet format, priorities
│   ├── hal_gpio.c            # BCM2711 memory-mapped GPIO driver & polling loop
│   ├── node1_workload.c      # Workload node (Tasks A/B/C, monitors, TCP client)
│   └── node2_supervisor.c    # Supervisor node (TCP server, analytics, fault map)
├── build/                    # Compiled aarch64le QNX binaries
│   ├── node1_workload        # Binary for Raspberry Pi 1
│   └── node2_supervisor      # Binary for Raspberry Pi 2
├── Makefile                  # QNX Momentics / qcc cross-compilation build file
├── design.md                 # Full systems architecture specification
├── testing_guide.md          # Hardware wiring, network setup, & testing runbook
├── .gitignore                # Git ignore rules for QNX development
└── README.md                 # Project executive documentation
```

---

## Quick Start

### 1. Build the Binaries
Using the QNX Momentics IDE or command line with QNX SDP 8.0:
```bash
make all
```
Binaries will be generated in `build/aarch64le-debug/`.

### 2. Deploy to Hardware
Detailed step-by-step instructions for GPIO pin wiring, static IP setup (`genet0`), binary transfer (`scp`), and running the verification tests are located in:

📖 **[Full Testing & Deployment Guide](file:///c:/Users/adity/ide-8.0-workspace/smart_city/testing_guide.md)**

---

## Interactive CLI Dashboards

Both nodes feature an integrated interactive real-time shell:

* **Node 1 Workload Shell (`node1>`)**:
  * `status`: Global workload status, uptime, health state
  * `tasks`: Execution timings, missed deadlines, heartbeats
  * `inject <fault>`: Software fault injection (`starve`, `deadline`, `cpu`)
  * `clear`: Clear all injected faults

* **Node 2 Supervisor Shell (`supervisor>`)**:
  * `status`: Global cluster health, remote link status, LED indicator
  * `tasks`: Live mirrored telemetry table of all remote tasks
  * `cpu`: Remote CPU statistics (Current, Min, Max, Average, P95)
  * `ipc`: IPC latency statistics
  * `faults`: Hierarchical fault map across all cluster components
