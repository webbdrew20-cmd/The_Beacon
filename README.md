# The_Beacon
Low-power, 5V ESP32 off-grid survival appliance featuring 256GB offline Wikipedia, LoRa mesh messaging, and environmental weather sensing.
## 📦 Bill of Materials (BOM)

Project Beacon relies on a modular, multi-controller architecture to handle heavy offline data processing, long-range mesh networking, and environmental monitoring without internet access.

### 🧠 Processing & Controllers
| Component | Model / Variant | Qty | Function in Project Beacon |
| :--- | :--- | :---: | :--- |
| **Main Processing Node** | Waveshare ESP32-P4 Wi-Fi 6 Basic Kit | 1 | Primary server host (High-performance RISC-V compute, web hosting, offline database indexing) |
| **Mesh Gateway / Controller** | ESP32-S3-WROOM-2 | 1 | Dedicated controller managing communication pipelines and local display UI |
| **Satellite / Compact Nodes** | ESP32-S3 Super Mini | 2 | Ultra-compact controllers for remote sensor gathering and auxiliary radio endpoints |

### 💾 Storage & Data
| Component | Model / Variant | Qty | Function in Project Beacon |
| :--- | :--- | :---: | :--- |
| **MicroSD Module** | HW-125 SD Card Reader | 2 | SPI-based storage interface for hosting large offline ZIM databases (Wikipedia/Medical) and system logs |

### 📡 Wireless & Communication
| Component | Model / Variant | Qty | Function in Project Beacon |
| :--- | :--- | :---: | :--- |
| **LoRa Radio Transceiver** | Semtech SX1262 Module | 1 | Sub-GHz long-range off-grid mesh radio communication |

### 🌡️ Sensors & User Interface
| Component | Model / Variant | Qty | Function in Project Beacon |
| :--- | :--- | :---: | :--- |
| **Barometric Pressure Sensor** | BMP280 | 1 | Measures atmospheric pressure and precise ambient temperature over I2C |
| **Humidity & Temp Sensor** | HW-507 (DHT11 Module) | 1 | Digital ambient relative humidity and temperature monitoring |
| **Status Display** | 0.96" OLED (SSD1306 I2C) | 1 | 128x64 display showing live node status, signal strength, and environmental readings |

---

### 🔩 Recommended Hardware & Wiring Accessories
* **Storage:** 2x High-end MicroSD Cards (Class 10 / UHS-I, 32GB to 256GB formatted FAT32/exFAT depending on database size).
* **Power Supply:** 5V / 3A USB-C Power Bank or regulated DC step-down power rail.
* **Interconnects:** DuPont jumper wires (F-F, M-F) and solid copper core wire for custom breadboard/PCB routing.
* **Antenna:** 868MHz / 915MHz SMA antenna matched to your regional LoRa frequency.

## 🔌 Wiring & Assembly Instructions

> ⚠️ **CRITICAL WIRING RULES BEFORE POWERING ON:**
> 1. **Common Ground (GND):** Every microcontroller board and peripheral module **must share a single common ground connection** to ensure logic signals reference the same baseline voltage.
> 2. **UART TX/RX Crossover:** Always connect Transmit to Receive ($TX \rightarrow RX$) and Receive to Transmit ($RX \rightarrow TX$) between communicating microcontrollers.
> 3. **Logic Levels:** The SX1262 radio, BMP280, and hw507 operate on **3.3V logic**. Connecting 5V directly to their data or power pins will permanently damage the hardware.

---

### 1. Main Processing Node (Waveshare ESP32-P4)

The ESP32-P4 manages primary web hosting and routes data between nodes[cite: 2, 3].

#### 💾 MicroSD Card Module #1 (System & ZIM Storage)
Connect the primary HW-125 SD module over SPI[cite: 2]:

| HW-125 Pin | ESP32-P4 Pin | Signal Name | Notes |
| :--- | :--- | :--- | :--- |
| **VCC** | **5V** | Power Rail | Use 5V if using an onboard regulator on the HW-125 module |
| **GND** | **GND** | Common Ground | Required |
| **MISO** | **GPIO 52** | `BEACON_SD_MISO` | SPI Master In / Slave Out[cite: 2] |
| **MOSI** | **GPIO 51** | `BEACON_SD_MOSI` | SPI Master Out / Slave In[cite: 2] |
| **SCK** | **GPIO 31** | `BEACON_SD_SCK` | SPI Serial Clock[cite: 2] |
| **CS** | **GPIO 30** | `BEACON_SD_CS` | SPI Chip Select[cite: 2] |

#### 🔀 Inter-Board UART Serial Links
Connect the P4's hardware serial buses to the auxiliary controller nodes[cite: 2]:

| Target Node | P4 UART Bus | P4 Pin | Target Node Pin | Line Function |
| :--- | :--- | :--- | :--- | :--- |
| **LoRa Mesh Node** | `UART_LORA`[cite: 2] | **GPIO 22** (TX)[cite: 2] | **GPIO 8** (RX)[cite: 1] | P4 Transmits commands to LoRa |
| **LoRa Mesh Node** | `UART_LORA`[cite: 2] | **GPIO 23** (RX)[cite: 2] | **GPIO 9** (TX)[cite: 1] | P4 Receives mesh inbox data from LoRa |
| **Media Node (WROOM-2)** | `UART_WROOM2`[cite: 2] | **GPIO 21** (TX)[cite: 2] | **GPIO 19** (RX)[cite: 4] | P4 Transmits media requests |
| **Media Node (WROOM-2)** | `UART_WROOM2`[cite: 2] | **GPIO 20** (RX)[cite: 2] | **GPIO 20** (TX)[cite: 4] | P4 Receives media status updates |

---

### 2. LoRa Communication Node (ESP32-S3)

This node executes the `RadioLib` mesh firmware and maintains the wireless message queue[cite: 1].

#### 📡 Semtech SX1262 Transceiver (SPI)
Connect the LoRa module to the dedicated SPI controller[cite: 1]:

| SX1262 Pin | ESP32-S3 Pin | Signal Name | Notes |
| :--- | :--- | :--- | :--- |
| **VCC** | **3.3V** | Power | **DO NOT USE 5V** |
| **GND** | **GND** | Ground | Common Ground |
| **NSS / CS** | **GPIO 10** | `LORA_CS` | SPI Chip Select[cite: 1] |
| **SCK** | **GPIO 11** | `LORA_SCK` | SPI Clock[cite: 1] |
| **MOSI** | **GPIO 12** | `LORA_MOSI` | SPI Master Out / Slave In[cite: 1] |
| **MISO** | **GPIO 13** | `LORA_MISO` | SPI Master In / Slave Out[cite: 1] |
| **NRESET** | **GPIO 7** | `LORA_RST` | Hardware Reset[cite: 1] |
| **BUSY** | **GPIO 6** | `LORA_BUSY` | Radio Busy State Line[cite: 1] |
| **DIO1** | **GPIO 5** | `LORA_DIO1` | Hardware Interrupt Trigger (`onDio1`)[cite: 1] |

#### 🔗 Serial Link to P4 Node
| ESP32-S3 Pin | Direction | Waveshare P4 Pin | Notes |
| :--- | :---: | :--- | :--- |
| **GPIO 8** (`P4_UART_RX`)[cite: 1] | $\leftarrow$ | **GPIO 22** (P4 TX)[cite: 2] | Listens to P4 data requests |
| **GPIO 9** (`P4_UART_TX`)[cite: 1] | $\rightarrow$ | **GPIO 23** (P4 RX)[cite: 2] | Flushes inbox JSON to P4 |

---

### 3. Entertainment Node (ESP32-S3-WROOM-2)

Dedicated controller handling local audio and ROM file streaming[cite: 4, 5].

#### 💾 MicroSD Card Module #2 (Media Storage)
Connect the secondary HW-125 module to the WROOM-2 SPI bus[cite: 4]:

| HW-125 Pin | WROOM-2 Pin | Signal Name | Notes |
| :--- | :--- | :--- | :--- |
| **VCC** | **5V** | Power Rail | Power supply line |
| **GND** | **GND** | Ground | Common Ground |
| **MISO** | **GPIO 12** | `W2_SD_MISO` | SPI Master In / Slave Out[cite: 4] |
| **MOSI** | **GPIO 11** | `W2_SD_MOSI` | SPI Master Out / Slave In[cite: 4] |
| **SCK** | **GPIO 10** | `W2_SD_SCK` | SPI Serial Clock[cite: 4] |
| **CS** | **GPIO 9** | `W2_SD_CS` | SPI Chip Select[cite: 4] |

#### 🔗 Serial Link to P4 Node
| WROOM-2 Pin | Direction | Waveshare P4 Pin | Notes |
| :--- | :---: | :--- | :--- |
| **GPIO 19** (`UART_P4_RX`)[cite: 4] | $\leftarrow$ | **GPIO 21** (P4 TX)[cite: 2] | Listens to P4 HTTP portal |
| **GPIO 20** (`UART_P4_TX`)[cite: 4] | $\rightarrow$ | **GPIO 20** (P4 RX)[cite: 2] | Sends streaming status to P4 |

---

### 4. Weather Station Node (ESP32 Super Mini)

This standalone node uses separate I2C buses to manage its display and barometric pressure sensor, plus a digital input for humidity[cite: 6].

#### 🖥️ 0.96" OLED Display (SSD1306 - I2C Bus 0)
| SSD1306 Pin | ESP32 Super Mini Pin | Signal Name |
| :--- | :--- | :--- |
| **VCC** | **5v** | Power Rail |
| **GND** | **GND** | Common Ground |
| **SDA** | **GPIO 10** | `OLED_SDA`[cite: 6] |
| **SCL** | **GPIO 11** | `OLED_SCL`[cite: 6] |

#### 🌡️ BMP280 Barometric Pressure Sensor (I2C Bus 1)
| BMP280 Pin | ESP32 Super Mini Pin | Signal Name | Notes |
| :--- | :--- | :--- | :--- |
| **VCC** | **3.3V** | Power Rail | **3.3V only** |
| **GND** | **GND** | Common Ground | Common Ground |
| **SDA** | **GPIO 1** | `BMP_SDA`[cite: 6] | Secondary Wire instance (`I2CBMP`)[cite: 6] |
| **SCL** | **GPIO 2** | `BMP_SCL`[cite: 6] | Secondary Wire instance (`I2CBMP`)[cite: 6] |

#### 💧 HW-507 / DHT11 Temp & Humidity Sensor
| DHT11 Pin | ESP32 Super Mini Pin | Signal Name | Notes |
| :--- | :--- | :--- | :--- |
| **VCC** | **3.3V** | Power Rail | Power supply |
| **GND** | **GND** | Common Ground | Common Ground |
| **DATA** | **GPIO 3** | `DHTPIN`[cite: 6] | 1-Wire Digital Data Line[cite: 6] |


# 📡 Project Beacon: Off-Grid Hardware Stack

Project Beacon is a modular, multi-node offline server and mesh communication appliance. It combines a high-performance **ESP32-P4** primary server node with auxiliary **ESP32-S3** controllers for long-range LoRa messaging, local media delivery, and micro-climate sensing.

---

## 🔍 Step 1: Locate Your Board's COM Port (Windows Device Manager)

Before flashing firmware or opening a serial console, you need to identify which **COM Port** Windows assigned to your plugged-in ESP32 board.

1. Connect your ESP32 board to your computer using a **data-capable USB-C cable**.
2. Open **Device Manager**:
   * Press `Win + X` on your keyboard and select **Device Manager**.
   * *Alternative:* Press `Win + R`, type `devmgmt.msc`, and hit **Enter**.
3. Expand the **Ports (COM & LPT)** section.
4. Look for an entry such as:
   * `Silicon Labs CP210x USB to UART Bridge (COMx)`
   * `USB-SERIAL CH340 (COMx)`
   * `USB Serial Device (COMx)` *(Standard ESP32 native USB-CDC driver)*
5. **How to verify your port:** Unplug your USB cable and plug it back in. The entry that disappears and reappears is your target port (e.g., `COM3`, `COM5`, `COM12`).

---

## 🛠️ Step 2: Build, Flash & Monitor with ESP-IDF v5.5

The main processing node and entertainment node run on **ESP-IDF v5.5** to support multi-threading, custom partition tables, and high-performance PSRAM access[cite: 3, 5].

### Environment Prerequisites
* Open the **ESP-IDF 5.5 CMD** terminal (or **ESP-IDF 5.5 PowerShell**) installed on your machine.
* Ensure `idf.py` is available in your PATH environment.

---

### Node 1: Waveshare ESP32-P4 (Main Server Node)
* **Directory:** `/p4_beacon_project`
* **Entry Point:** `main/beacon_main.c`[cite: 3]

1. **Navigate to the project directory:**
   ```cmd
   cd path\to\Project-Beacon\p4_beacon_project

## 🖨️ 3D Printing & Post-Processing Guide

Pre-configured printable model files for the Project Beacon enclosure, internal brackets, and mounting plates are provided in the repository as **`.3mf`** files.

---

### 🧵 Material & Support Recommendations

* **Primary Chassis Filament:** **PA6 (Nylon 6)** is strongly recommended for its high heat deflection temperature, extreme impact strength, and rugged outdoor durability.
* **Support Interface Filament:** **ABS** (or ABS-GF/CF).

> 💡 **Pro-Tip (Dual-Material / AMS / MMU Printing):** 
> When printing the main body in **PA6**, set **ABS as your Support Interface material**. PA6 and ABS do not bond chemically during extrusion. Using ABS for interface layers allows solid support structures to snap off effortlessly, leaving a clean, glass-smooth surface without scarring your print.

---

### 🛠️ Hardware Post-Processing & Hole Preparation

Due to normal FDM thermal contraction and inner-wall extrusion squish, printed holes will be slightly tight straight off the build plate.

1. **Drill Before Fastening:** Always chase/clean out every screw hole with a drill bit sized for your chosen thread diameter before driving fasteners or heat-set inserts.
2. **Proper Thread Engagement:** Pre-drilling cleans up layer line ridges, prevents the printed walls from delaminating or splitting along layer boundaries, and ensures screw threads bite cleanly into solid plastic for a rigid, secure hold.
   
