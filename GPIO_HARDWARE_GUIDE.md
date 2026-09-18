# GPIO, LED & Push-Button Hardware Demonstration Guide
**Smart City RTOS Fault & Performance Monitoring Platform**
*Target: Dual Raspberry Pi 4/5 running QNX Neutrino RTOS 8.0 / 7.1*

---

## 1. Physical Hardware Pinout & Wiring Table

Both boards (Raspberry Pi 1 and Raspberry Pi 2) share the **exact same physical pin layout** on the 40-pin header.

### A. Tri-Color Health LEDs (Outputs)

| LED Color | BCM GPIO Number | Physical Header Pin | Wiring Instructions |
| :--- | :--- | :--- | :--- |
| **🟢 GREEN** | **GPIO 17** | **Physical Pin 11** | Long leg (+) to Pin 11, Short leg (-) through $220\Omega$ resistor to GND (Pin 9) |
| **🟡 YELLOW** | **GPIO 27** | **Physical Pin 13** | Long leg (+) to Pin 13, Short leg (-) through $220\Omega$ resistor to GND (Pin 9) |
| **🔴 RED** | **GPIO 22** | **Physical Pin 15** | Long leg (+) to Pin 15, Short leg (-) through $220\Omega$ resistor to GND (Pin 9) |

> ⚠️ **Ground Pins available on Raspberry Pi:** Physical Pin 6, 9, 14, 20, 25, 30, 34, 39.

---

### B. Push-Buttons / Jumper Wire Pins (Inputs)

All button pins are configured with **internal pull-up resistors** in `hal_gpio.c`:
* **Default (Floating / Released):** Pin reads `HIGH` ($3.3\text{V}$) $\rightarrow$ System is **NORMAL**.
* **Pressed (Contact with GND):** Pin pulled `LOW` ($0\text{V}$) $\rightarrow$ **FAULT ACTIVE** while contact is held.
* **Released (GND removed):** Pin returns `HIGH` $\rightarrow$ **FAULT AUTO-CLEARS** (Self-healing).

| Button | BCM GPIO | Physical Header Pin | Connect To |
| :--- | :--- | :--- | :--- |
| **Button 1** | **GPIO 23** | **Physical Pin 16** | One leg to Pin 16, other leg to GND (Pin 14) |
| **Button 2** | **GPIO 24** | **Physical Pin 18** | One leg to Pin 18, other leg to GND (Pin 20) |
| **Button 3** | **GPIO 25** | **Physical Pin 22** | One leg to Pin 22, other leg to GND (Pin 25) |

---

## 2. LED Indications: When Do They Turn ON and OFF?

The RTOS health engine evaluates active faults every interval and sets the LED state according to strict priority:

$$\mathbf{\color{red}CRITICAL\ (RED)} \quad>\quad \mathbf{\color{orange}WARNING\ (YELLOW)} \quad>\quad \mathbf{\color{green}HEALTHY\ (GREEN)}$$

### 🟢 GREEN LED (Healthy Baseline)
* **When it turns ON:**
  * At system boot when both nodes start normally.
  * When all active tasks meet their deadlines and heartbeats.
  * When CPU is $< 70\%$ and IPC latency is $< 1\text{ ms}$.
  * When you run `clear` in the CLI or release all held push-buttons.
* **When it turns OFF:**
  * As soon as any Warning (Yellow) or Critical (Red) fault occurs.

### 🟡 YELLOW LED (Warning State)
* **When it turns ON:**
  * **On Node 1:**
    * **Deadline Miss:** Service A, B, or C execution exceeds its deadline (e.g. Service A $> 80\text{ ms}$).
    * **Slow Heartbeat:** A service heartbeat is delayed by $> 2\times$ its period.
    * **Elevated CPU:** CPU load is between $70\%$ and $84.9\%$.
    * **Elevated IPC:** Inter-task transfer latency is between $220\text{ ms}$ and $300\text{ ms}$.
  * **On Supervisor (Node 2):**
    * Automatically turns Yellow within $500\text{ ms}$ when telemetry from Node 1 reports any of the above warning states.
* **When it turns OFF:**
  * When the deadline overrun clears (returns to Green), OR when a Critical fault escalates it to Red.

### 🔴 RED LED (Critical Safety Fault)
* **When it turns ON:**
  * **On Node 1:**
    * **Task Starvation:** Service B (or any task) fails to report a heartbeat for $\ge 3\times$ its period ($600\text{ ms}$ silent).
    * **CPU Overload:** Total CPU utilization $\ge 85\%$.
    * **IPC Latency Critical:** Shared-memory queue latency $\ge 300\text{ ms}$.
  * **On Supervisor (Node 2):**
    * **Node 1 Disconnected:** No telemetry packet received from Node 1 for $> 2\text{ seconds}$ (Ethernet pull or crash).
    * Telemetry reports remote task starvation or CPU overload $\ge 85\%$.
    * Button 1 or 2 is pressed directly on the Supervisor board.
* **When it turns OFF:**
  * When heartbeats resume, Node 1 reconnects, or buttons are released.

---

## 3. Push-Button Functionality on Each Board

### Board 1: Node 1 (Workload Pi)

| Push-Button | Pin | Hardware Action | Result on Node 1 LED | Result on Supervisor LED |
| :--- | :--- | :--- | :--- | :--- |
| **Button 1** | **Pin 16** (GPIO 23) | **Press & Hold to GND** | 🔴 **RED LED ON** *(Service B Starved)* | 🔴 **RED LED ON** *(propagated over TCP)* |
| | | **Release** | 🟢 **GREEN LED restores** | 🟢 **GREEN LED restores** |
| **Button 2** | **Pin 18** (GPIO 24) | **Press & Hold to GND** | 🔴 **RED LED ON** *(CPU Overload $>85\%$)* | 🔴 **RED LED ON** *(propagated over TCP)* |
| | | **Release** | 🟢 **GREEN LED restores** | 🟢 **GREEN LED restores** |
| **Button 3** | **Pin 22** (GPIO 25) | **Press & Hold to GND** | 🟡 **YELLOW LED ON** *(Service A Deadline Miss)* | 🟡 **YELLOW LED ON** *(propagated over TCP)* |
| | | **Release** | 🟢 **GREEN LED restores** | 🟢 **GREEN LED restores** |

---

### Board 2: Supervisor (Supervisor Pi)

| Push-Button | Pin | Hardware Action | Result on Supervisor LED |
| :--- | :--- | :--- | :--- |
| **Button 1** | **Pin 16** (GPIO 23) | **Press & Hold to GND** | 🔴 **RED LED ON** *(Simulates Node 1 Disconnect)* |
| | | **Release** | 🟢 **GREEN LED restores** *(Reconnected)* |
| **Button 2** | **Pin 18** (GPIO 24) | **Press & Hold to GND** | 🔴 **RED LED ON** *(Simulates Remote IPC Timeout)* |
| | | **Release** | 🟢 **GREEN LED restores** |
| **Button 3** | **Pin 22** (GPIO 25) | **Press & Hold to GND** | 🟡 **YELLOW LED ON** *(Simulates Elevated Remote CPU)* |
| | | **Release** | 🟢 **GREEN LED restores** |

---

## 4. How to Test Without Soldered Buttons (Using a Jumper Wire)

If you have jumper wires but no physical buttons:
1. Take a female-to-male or male-to-male jumper wire.
2. Plug one end into a **Ground Pin** (e.g., **Physical Pin 14**).
3. Touch the other end of the wire to:
   * **Pin 22** $\rightarrow$ Watch the **🟡 Yellow LED** turn ON immediately.
   * Pull the wire away $\rightarrow$ Watch the **🟢 Green LED** come back ON.
   * **Pin 16** $\rightarrow$ Watch the **🔴 Red LED** turn ON immediately.
   * Pull the wire away $\rightarrow$ Watch the **🟢 Green LED** come back ON.

---

## 5. Software Simulation via CLI (Zero Hardware Needed)

You can perform the exact same demonstration directly through the terminal CLI:

### Test Yellow LED (Deadline Miss):
```text
node1> inject deadline 1 100
```
*Yellow LED turns on on both Pi 1 and Pi 2.*

### Test Red LED (Task Starvation):
```text
node1> inject starve 2
```
*Red LED turns on on both Pi 1 and Pi 2.*

### Test Red LED (CPU Overload):
```text
node1> inject cpu on
```
*CPU exceeds 85%, Red LED turns on.*

### Clear All & Restore Green LED:
```text
node1> clear
supervisor> clear
```
*Both boards immediately return to Green LED.*

---

## 6. Complete 3-Minute Live Judging Demo Script

1. **Nominal State (Green LED):**
   * Show both Pis with **🟢 GREEN LEDs** lit.
   * Run `status` on both nodes: Explain that all 3 periodic tasks are meeting deadlines.
2. **Warning Injection (Yellow LED):**
   * Touch Pin 22 to GND (or run `inject deadline 1 100`).
   * Show that **🟡 YELLOW LED** illuminates on Pi 1 and syncs to Pi 2 within $500\text{ ms}$.
   * Run `faults` to display the active `FAULT_DEADLINE_MISS`.
3. **Critical Injection (Red LED):**
   * Touch Pin 16 to GND (or run `inject starve 2`).
   * Show that **🔴 RED LED** illuminates on both Pis.
   * Run `faults` to display `TASK_STATE_STARVED`.
4. **Self-Healing Recovery (Green LED):**
   * Release the wire (or run `clear`).
   * Show both LEDs instantly returning to **🟢 GREEN**.
   * Run `status` to confirm zero active faults.
