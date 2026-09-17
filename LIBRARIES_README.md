# Libraries and Dependencies Reference Guide
### UrbanPulse-QNX Smart City RTOS Monitoring Platform

This document details all external, operating system, runtime, and hardware abstraction libraries utilized across the **UrbanPulse-QNX** dual-node real-time application.

---

## 1. Executive Summary

| Category | Primary Libraries / APIs | Primary Purpose |
| :--- | :--- | :--- |
| **Linker Libraries (QNX)** | `libsocket`, `libm`, `libc` | Network sockets, floating-point math, standard C runtime |
| **RTOS Kernel APIs** | `sys/neutrino.h`, `sys/syspage.h`, `hw/inout.h`, `sys/mman.h` | Microkernel timers, system page metrics, direct register I/O, device memory mapping |
| **POSIX Real-Time Extensions** | `pthread`, `sched.h`, `time.h`, `sys/socket.h` | Real-time `SCHED_FIFO` threads, priority inheritance mutexes, monotonic clocks, BSD sockets |
| **C Standard Runtime (ISO C99/C11)**| `stdint.h`, `inttypes.h`, `stdbool.h`, `stdio.h`, `string.h`, `stdlib.h`, `errno.h` | Fixed-width types, portable 64-bit formatting, memory management, buffer processing |

---

## 2. Linker Libraries (`Makefile`)

The following libraries are linked during compilation using the QNX `qcc` compiler (`qcc -Vgcc_nto$(PLATFORM)`):

### 2.1 `-lsocket` (QNX BSD Socket Library)
* **Header Files**: `<sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>`, `<netdb.h>`
* **Source Files Using It**: [`src/node1_workload.c`](file:///c:/Users/adity/ide-8.0-workspace/smart_city/src/node1_workload.c), [`src/node2_supervisor.c`](file:///c:/Users/adity/ide-8.0-workspace/smart_city/src/node2_supervisor.c), [`src/smart_city.c`](file:///c:/Users/adity/ide-8.0-workspace/smart_city/src/smart_city.c)
* **Purpose**:
  * Implements Berkeley-compliant TCP/IP client and server networking.
  * Node 1 connects via non-blocking TCP socket to port `5555` to stream real-time JSON telemetry every 500 ms.
  * Node 2 binds, listens, and accepts incoming telemetry stream with zero-copy stream chunk reassembly (`recv`, `send`, `setsockopt(SO_REUSEADDR)`).

### 2.2 `-lm` (Standard C Math Library)
* **Header Files**: `<math.h>`
* **Source Files Using It**: [`src/node1_workload.c`](file:///c:/Users/adity/ide-8.0-workspace/smart_city/src/node1_workload.c), [`src/node2_supervisor.c`](file:///c:/Users/adity/ide-8.0-workspace/smart_city/src/node2_supervisor.c)
* **Purpose**:
  * Provides trigonometric, floating-point, and statistical computation functions (`sinf`, `cosf`, `sqrt`, `fabs`).
  * Used for generating synthetic municipal sensor workloads (environmental sensor drift, fluid dynamics modeling for water pressure grids).
  * Used in supervisor analytics for computing 95th percentile (P95) latency bounds and moving averages.

### 2.3 `libc` (Standard C & POSIX Runtime)
* **Header Files**: Integrated across all C standards and POSIX headers.
* **Purpose**:
  * Built-in QNX runtime library linked by default by `qcc`.
  * Provides core memory allocation (`malloc`, `free`), string manipulation (`memmove`, `strncpy`, `strchr`), formatted I/O (`printf`, `snprintf`, `sscanf`), and POSIX threading primitives.

### 2.4 `-lprofilingS` (Optional Profile Build)
* **Purpose**:
  * Activated when compiling with `BUILD_PROFILE=profile` in the [`Makefile`](file:///c:/Users/adity/ide-8.0-workspace/smart_city/Makefile#L33).
  * Provides function call instrumentation and execution trace profiling (`-finstrument-functions`).

---

## 3. QNX Neutrino RTOS Native Microkernel APIs

These headers interface directly with the QNX Neutrino microkernel (`procnto`):

| Header | Description & Specific Functions | Architectural Role |
| :--- | :--- | :--- |
| `<sys/neutrino.h>` | QNX microkernel message passing & timing: `ClockCycles()`, `ChannelCreate()`, `ConnectAttach()`, `MsgSend()`, `MsgReceive()`, `MsgReply()` | **High-Res Cycles & Heartbeat IPC**: `ClockCycles()` provides sub-nanosecond hardware CPU cycle counting. Tasks send heartbeat messages via `MsgSend()`. Fault Detector channel receives via `MsgReceive()` and replies via `MsgReply()`. |
| `<sys/procfs.h>`, `<devctl.h>` | QNX process filesystem interface: `/proc/<pid>/as`, `devctl()`, `DCMD_PROC_INFO`, `DCMD_PROC_STATUS` | **Health & CPU Sampling (Requirement A)**: Samples true kernel CPU execution ticks (`utime`, `stime`), active thread states, and thread counts directly from `/proc/<pid>/as`. |
| `<sys/syspage.h>` | Access to the QNX system page (`_syspage_ptr`) | Querying hardware topology, CPU core frequencies, and hardware clock resolution. |
| `<hw/inout.h>` | QNX raw port and memory I/O primitives: `in32()`, `out32()` | Direct 32-bit atomic register reads and writes to BCM2711 GPIO controller registers. |
| `<sys/mman.h>` | Device memory mapping: `mmap_device_io()`, `munmap_device_io()` | Maps physical SoC memory (`BCM2711_GPIO_BASE = 0xFE200000`) into virtual user address space for zero-overhead GPIO control. |

---

## 4. POSIX Real-Time & System Libraries

The application adheres to POSIX.1b real-time extensions, providing deterministic execution:

### 4.1 POSIX Threads (`<pthread.h>`)
* **Thread Creation & Attributes**:
  * Configures threads with `PTHREAD_EXPLICIT_SCHED` and `SCHED_FIFO` policy.
  * Allows assigning static real-time priority levels (ranging from 10 to 25) directly mapped to the QNX scheduler.
* **Priority Inversion Protection**:
  * Mutexes are initialized with `PTHREAD_PRIO_INHERIT` protocol:
    ```c
    pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    ```
  * Prevents priority inversion across shared memory task queues, trace buffers, and fault maps.
* **Condition Variables (`pthread_cond_t`)**:
  * Used for event signaling in IPC queues (`RT_COND_SIGNAL`, `RT_COND_WAIT`).

### 4.2 POSIX Real-Time Scheduler (`<sched.h>`)
* **Types & Functions**: `struct sched_param`, `sched_get_priority_min()`, `sched_get_priority_max()`.
* **Policy**: `SCHED_FIFO` (first-in, first-out preemptive real-time scheduling).

### 4.3 High-Resolution Monotonic Clocks & Hardware Interval Timers (`<time.h>`, `<signal.h>`)
* **Functions**: `clock_gettime(CLOCK_MONOTONIC, &ts)`, `timer_create()`, `timer_settime()`, `timer_delete()`, `sigwait()`.
* **Purpose & Conformance**:
  * Directly satisfies **Problem Statement Requirement D (Timers)**.
  * Uses QNX/POSIX kernel interval timers (`timer_create(CLOCK_MONOTONIC, ...)` and `timer_settime()`) to deliver hardware-backed periodic ticks with zero drift.
  * Replaces busy-polling and basic sleep loops with kernel-level synchronous signal waits (`sigwait()`).
  * Microsecond timestamping for execution time measurement, deadline monitoring, and IPC latency tracking.

### 4.4 File & Process Control (`<unistd.h>`, `<fcntl.h>`)
* **Functions**: `close()`, `fcntl()`, `sleep()`.
* **Purpose**: Safe closing of socket descriptors and configuring non-blocking descriptor flags.

### 4.5 POSIX Shared Memory (`<sys/mman.h>`, `<sys/stat.h>`, `<fcntl.h>`)
* **Functions**: `shm_open()`, `ftruncate()`, `mmap()`, `munmap()`, `shm_unlink()`.
* **Purpose & Conformance**:
  * Satisfies the **Inter-task Communication requirement**: Uses shared memory (`shm_open` + `mmap`) for high-frequency metrics (CPU%, IPC latency, Task States) to achieve zero-copy, sub-microsecond access.
  * Node 1 creates `/smart_city_metrics` mapped via `mmap(MAP_SHARED)`, mirroring live telemetry (`SharedMetricsBlock`) with zero serialization overhead.
  * Diagnostics and local observers attach via `rt_shm_attach()` (`mmap(PROT_READ)`).
  * Complements cross-node TCP sockets (port 5555) for distributed multi-Pi cluster coordination.

---

## 5. Standard C Runtime (ISO C99 / C11)

| Header | Key Types / Macros Used | Purpose |
| :--- | :--- | :--- |
| `<stdint.h>` | `uint8_t`, `uint16_t`, `uint32_t`, `uint64_t`, `uintptr_t` | Guarantees deterministic variable sizes across 32-bit and 64-bit ARM architectures (`aarch64le`). |
| `<inttypes.h>` | `PRIu64`, `FMT_U64` | Portable cross-platform 64-bit integer printing (maps to `%lu` on 64-bit QNX/ARM, preventing format string compiler warnings). |
| `<stdbool.h>` | `bool`, `true`, `false` | Standardized boolean logic flags for states, fault statuses, and loops. |
| `<stdio.h>` | `printf`, `snprintf`, `sscanf`, `perror` | Formatted output for the UART / interactive diagnostic CLI and JSON payload serialization/deserialization. |
| `<stdlib.h>` | `malloc`, `free`, `atoi`, `strtoul`, `exit` | Dynamic buffer allocation for CLI command parsing and thread parameter contexts. |
| `<string.h>` | `memset`, `memcpy`, `memmove`, `strlen`, `strncpy`, `strchr` | Buffer zeroing, telemetry packet delimitation (`\n`), and safe string copying. |
| `<errno.h>` | `errno`, `strerror` | Diagnostics and logging for network socket errors and system call failures. |

---

## 6. Hardware Target & Toolchain Compatibility

* **Target Architecture**: ARMv8-A 64-bit Little Endian (`aarch64le`)
* **Target Hardware**: Raspberry Pi 4 / Raspberry Pi 5 (Broadcom BCM2711 / BCM2712 SoC)
* **Target OS**: BlackBerry QNX Neutrino RTOS 7.1 / 8.0
* **Compiler**: QNX Software Development Platform (SDP) `qcc`
* **Portability Guarantee**: 100% free of proprietary Windows or vendor-locked APIs; strictly conforms to POSIX and native QNX Neutrino RTOS standards.
