# Walkthrough: QNX `/proc/<pid>/as`, `devctl()`, and `ClockCycles()` Integration

We have fully implemented **Core Building Block A (Health/performance sampling)** directly utilizing QNX's native kernel interfaces:
1. **`/proc/<pid>/as` and `devctl()`** for true kernel-level CPU execution accounting and thread state monitoring.
2. **`ClockCycles()`** alongside `clock_gettime(CLOCK_MONOTONIC)` for sub-nanosecond task execution profiling.

---

## 1. Key Implementation Details

### A. QNX `procfs` & `devctl()` Interface ([`src/smart_city_common.h`](file:///c:/Users/adity/ide-8.0-workspace/smart_city/src/smart_city_common.h#L350-L400))
* Added `<devctl.h>` and `<sys/procfs.h>`.
* Implemented `qnx_procfs_sample_cpu()`:
  * Opens `/proc/<pid>/as` via `open(path, O_RDONLY)`.
  * Issues `devctl(fd, DCMD_PROC_INFO, &pinfo, sizeof(pinfo), NULL)` to query thread count, kernel user time (`utime`), system time (`stime`), and process flags.
  * Issues `devctl(fd, DCMD_PROC_STATUS, &pstatus, sizeof(pstatus), NULL)` to sample active thread states (`STATE_RUNNING`, `STATE_RECEIVE`, etc.).
  * Closes the descriptor, guaranteeing zero resource leaks.

### B. Hardware Cycle Timing (`ClockCycles()`) ([`src/smart_city_common.h`](file:///c:/Users/adity/ide-8.0-workspace/smart_city/src/smart_city_common.h#L345-L355))
* Implemented `get_clock_cycles()`:
  * Invokes native QNX `ClockCycles()` to read the ARM/x86 64-bit hardware CPU cycle counter directly.
  * Integrated into `service_a_thread`, `service_b_thread`, and `service_c_thread` for cycle-level jitter and latency profiling.

### C. Watchdog CPU Sampling ([`src/node1_workload.c`](file:///c:/Users/adity/ide-8.0-workspace/smart_city/src/node1_workload.c#L535-L560))
* In `monitoring_task_thread`, invokes `qnx_procfs_sample_cpu(getpid(), &proc_sample)` every 100 ms.
* Combines kernel procfs metrics with the duty-cycle estimate for double-verified CPU overload detection.

---

## 2. Updated Reports & Documentation
* [**`LIBRARIES_README.md`**](file:///c:/Users/adity/ide-8.0-workspace/smart_city/LIBRARIES_README.md#L55-L63): Added `<sys/procfs.h>`, `<devctl.h>`, and `ClockCycles()` to Section 3.
* [**`requirements_compliance_report.html`**](file:///c:/Users/adity/ide-8.0-workspace/smart_city/requirements_compliance_report.html#L187-L194): Updated Requirement 2 (Section A) and Requirement 8 to reflect procfs `devctl()` sampling and `ClockCycles()` hardware timing.
