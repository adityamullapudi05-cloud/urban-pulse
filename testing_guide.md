# UrbanPulse-QNX: Hardware Testing & Deployment Guide
> **Unified Real-time Bit-level Analytics & Node Performance Under Latency Supervisory Engine**

**Target:** QNX Neutrino 8.0 on Raspberry Pi 4 (aarch64le)  
**Nodes:** 2 x Raspberry Pi connected via Ethernet (genet0)

---

## Project Structure

```
smart_city/
├── Makefile
└── src/
    ├── smart_city_common.h    # Shared definitions, structs, priorities (both nodes)
    ├── hal_gpio.c             # GPIO LEDs + physical button input (both nodes)
    ├── node1_workload.c       # Raspberry Pi 1 - Workload Node
    └── node2_supervisor.c     # Raspberry Pi 2 - Supervisor Node
```

---

## Hardware Wiring

### GPIO LEDs (same pins on BOTH Pi 1 and Pi 2)

| GPIO | RPi Pin | Color  | Meaning         |
|------|---------|--------|-----------------|
| 17   | Pin 11  | Green  | System Healthy  |
| 27   | Pin 13  | Yellow | Warning Active  |
| 22   | Pin 15  | Red    | Critical Fault  |

### GPIO Input Buttons - Pi 1 (Workload Node)

| GPIO | RPi Pin | Touch to GND (hold)    | Release   |
|------|---------|------------------------|-----------|
| 23   | Pin 16  | Service B STARVATION   | Normal    |
| 24   | Pin 18  | CPU OVERLOAD active    | Normal    |
| 25   | Pin 22  | Service A DEADLINE MISS| Normal    |

### GPIO Input Buttons - Pi 2 (Supervisor Node)

| GPIO | RPi Pin | Touch to GND (hold)    | Release     |
|------|---------|------------------------|-------------|
| 23   | Pin 16  | Node 1 DISCONNECTED    | Reconnected |
| 24   | Pin 18  | IPC TIMEOUT            | Normal      |
| 25   | Pin 22  | CPU OVERLOAD warning   | Normal      |

> Wiring: Use any GND pin (e.g. Pin 6, 9, 14, 25) as common ground.
> Buttons are active LOW (internal pull-up). Touch pin to GND = fault active.

---

## Build Instructions

### Option A - QNX Momentics IDE (Recommended)

Create TWO separate QNX C projects in Momentics:

**Project 1: node1_workload**
- Add files: src/node1_workload.c, src/hal_gpio.c, src/smart_city_common.h
- Target: Raspberry Pi 1 IP address
- Linker libs: -lsocket -lm

**Project 2: node2_supervisor**
- Add files: src/node2_supervisor.c, src/hal_gpio.c, src/smart_city_common.h
- Target: Raspberry Pi 2 IP address
- Linker libs: -lsocket -lm

---

### Option B - Makefile (Command Line)

```bash
# Build both binaries (debug, aarch64le)
make all

# Build release variant
make all BUILD_PROFILE=release

# Clean build output
make clean

# Clean and rebuild
make rebuild
```

Build output:
```
build/aarch64le-debug/
├── node1_workload     <- deploy to Pi 1
└── node2_supervisor   <- deploy to Pi 2
```

---

## Deploy to Raspberry Pi

```bash
# Set Pi IP addresses
PI1_IP=192.168.1.1
PI2_IP=192.168.1.2

# Copy binaries to each Pi via SCP
scp build/aarch64le-debug/node1_workload  root@$PI1_IP:/home/root/
scp build/aarch64le-debug/node2_supervisor root@$PI2_IP:/home/root/

# Make executables
ssh root@$PI1_IP "chmod +x /home/root/node1_workload"
ssh root@$PI2_IP "chmod +x /home/root/node2_supervisor"
```

---

## Run Order

> [!IMPORTANT]
> **GPIO Hardware Access Requires ROOT Privileges**
> On QNX, memory-mapped I/O (`ThreadCtl` and physical GPIO registers) requires `root` permissions.
> If uploaded as `qnxuser`, switch to root and prepare the executable before running:
>
> ```bash
> # 1. Switch to root
> su -
>
> # 2. Copy the binary to root's tmp folder
> cp /data/home/qnxuser/tmp/node1_workload /root/tmp/ 2>/dev/null || cp ~qnxuser/tmp/node1_workload ~/tmp/
>
> # 3. Go to root's tmp and check
> cd ~/tmp
> ls -l
> chmod +x node1_workload
> ```
> *(On Pi 2, do the same with `node2_supervisor`)*.

> ALWAYS start Pi 2 (Supervisor) FIRST - it is the TCP server.

### Step 1 - Start Pi 2 (Supervisor)

```bash
# Log in and elevate to root
ssh -m hmac-sha2-256-etm@openssh.com qnxuser@192.168.1.2
su -
cd ~/tmp
./node2_supervisor

# Or with custom port
./node2_supervisor --port=5555
```

### Step 2 - Start Pi 1 (Workload)

```bash
# Log in and elevate to root
ssh -m hmac-sha2-256-etm@openssh.com qnxuser@192.168.1.1
su -
cd ~/tmp
./node1_workload --ip=192.168.1.2

# Or with custom port
./node1_workload --ip=192.168.1.2 --port=5555
```

---

## CLI Commands - Pi 1 (node1>)

| Command                      | Description                              |
|------------------------------|------------------------------------------|
| help                         | Show all available commands              |
| status                       | Global health, LED state, CPU, uptime    |
| tasks                        | All 3 task metrics                       |
| cpu                          | CPU utilization breakdown per task       |
| ipc                          | IPC latency Min/Max/Avg/P95              |
| faults                       | Active fault map                         |
| trace                        | Gantt execution timeline (last 20 events)|
| inject starve <1-3>          | Freeze heartbeat - task starvation       |
| inject deadline <1-3> <ms>   | Add execution delay - deadline miss      |
| inject cpu <on|off>          | Toggle CPU overload burner               |
| inject ipc <delay_ms>        | Add IPC channel latency                  |
| clear                        | Clear all faults and injections          |
| exit                         | Shutdown node 1                          |

---

## CLI Commands - Pi 2 (supervisor>)

| Command   | Description                                      |
|-----------|--------------------------------------------------|
| help      | Show all available commands                      |
| status    | Supervisor health, LED, Node 1 link              |
| nodes     | Node connectivity overview                       |
| tasks     | Remote mirror of Node 1 task states              |
| cpu       | CPU analytics Min/Max/Avg/P95                    |
| ipc       | IPC latency received from Node 1                 |
| faults    | Active fault map (all nodes and tasks)           |
| faultmap  | Full fault hierarchy (node/task tree)            |
| clear     | Clear all active faults                          |
| exit      | Shutdown supervisor                              |

---

## Fault Injection Scenarios (CLI)

```bash
# 1. Task Starvation on Service B
node1> inject starve 2
# Wait ~400ms -> STARVED fault + Red LED appears on both nodes

# 2. Deadline Miss on Service A
node1> inject deadline 1 100
# +100ms delay -> exceeds 80ms deadline -> DEADLINE_MISS + Yellow LED

# 3. CPU Overload
node1> inject cpu on
# Burner runs -> exceeds 85% -> CPU_OVERLOAD fault + Red LED

# 4. IPC Latency
node1> inject ipc 60
# 60ms delay -> exceeds 50ms threshold -> IPC_TIMEOUT + Red LED

# 5. Clear all
node1> clear
# All injections removed -> faults clear -> Green LED restored
```

---

## Physical GPIO Button Behavior

| Button | Action | Effect |
|--------|--------|--------|
| Pi1 Pin 16 held to GND | Service B starvation active | Red LED on |
| Pi1 Pin 16 released    | Starvation cleared          | LED restores |
| Pi1 Pin 18 held to GND | CPU overload active         | Red LED on |
| Pi1 Pin 22 held to GND | Deadline miss active        | Yellow LED on |
| Pi2 Pin 16 held to GND | Simulate node disconnect    | Red LED on |
| Pi2 Pin 18 held to GND | Simulate IPC timeout        | Red LED on |
| Pi2 Pin 22 held to GND | Simulate CPU overload       | Yellow LED on |

---

## Network Setup (QNX terminal on each Pi)

```bash
# On Pi 1
ifconfig genet0 192.168.1.1 netmask 255.255.255.0

# On Pi 2
ifconfig genet0 192.168.1.2 netmask 255.255.255.0

# Verify
ping 192.168.1.2     # from Pi 1
ping 192.168.1.1     # from Pi 2
```

---

## Thread Overview

### Pi 1 - node1_workload (8 threads)

| Thread                  | Priority | Description                        |
|-------------------------|----------|------------------------------------|
| hal_gpio_poll_thread    | 25       | Physical button handler (highest)  |
| monitoring_task_thread  | 20       | Heartbeat, deadline, CPU sampling  |
| service_a_thread        | 15       | Smart Traffic (100ms/80ms)         |
| service_b_thread        | 14       | Smart Grid / IPC (200ms/150ms)     |
| service_c_thread        | 13       | Environment sensor (500ms/400ms)   |
| telemetry_sender_thread | 10       | TCP JSON sender to Pi 2 (500ms)    |
| cpu_burner_thread       | 5        | CPU overload injector              |
| cli_thread              | 5        | UART diagnostic terminal (lowest)  |

### Pi 2 - node2_supervisor (3 threads)

| Thread                  | Priority | Description                        |
|-------------------------|----------|------------------------------------|
| hal_gpio_poll_thread    | 25       | Physical button handler (highest)  |
| telemetry_server_thread | 10       | TCP server, receives Pi 1 data     |
| cli_thread              | 5        | UART diagnostic terminal (lowest)  |

---

## Troubleshooting

| Problem                        | Solution                                          |
|--------------------------------|---------------------------------------------------|
| Pi 1 says "Connecting..." loop | Start Pi 2 first, check Ethernet, verify IP       |
| undefined reference: socket    | Add -lsocket to linker flags                      |
| undefined reference: sin       | Add -lm to linker flags                           |
| GPIO buttons not responding    | Run as root (ThreadCtl needs I/O privilege)       |
| multiple definition of main    | Create two separate Momentics projects            |
| LED not lighting               | Check GPIO output pin config in hal_gpio_init()   |
| Format warning on aarch64      | Fixed - uses PRIu64 from inttypes.h               |
