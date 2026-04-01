# MeshMiner32

A DIY Bitcoin solo miner built on ESP32 DevKit V1 boards using ESP-NOW mesh networking, a SH1106 SPI OLED display, and public-pool.io.

Multiple ESP32 boards work together as a mesh — one **master** connects to WiFi and the Bitcoin mining pool, while **worker** nodes receive job assignments over ESP-NOW and hash their portion of the nonce space independently. The more workers you add, the higher the combined hashrate.

![Mining page](photos/mining.jpg)

---

## Features

- **ESP-NOW mesh** — no router needed between nodes, works peer-to-peer
- **Auto nonce splitting** — master divides the nonce space evenly across all workers
- **SH1106 SPI OLED** — 4 rotating info pages on the master (Mining, Pool, Network, Mesh Workers)
- **Boot screen** — Bitcoin B logo with MeshMiner32 name on startup
- **Mining spinner** — animated indicator shows active hashing
- **Blue LED** — blinks proportional to hash speed on both master and worker
- **Fallback election** — if master goes offline, workers elect a new master automatically
- **Cross-core safe** — miner runs on Core 1, WiFi/ESP-NOW safely handled on Core 0
- **public-pool.io** — 0% fee solo pool, 100% of block reward goes to your address
- **Scalable** — add as many worker ESP32s as you want, each auto-joins and gets a nonce slice

---

## Hardware Required

### Per node (master or worker)
| Part | Notes |
|------|-------|
| ESP32 DevKit V1 | Any ESP32-WROOM-32 based board |
| USB cable | For flashing and power |

### Master only
| Part | Notes |
|------|-------|
| SH1106 128x64 OLED | SPI version, 6-pin (no CS pin) |
| Jumper wires | 5 wires for OLED |

---

## OLED Wiring (Master only)

```
SH1106 OLED    ESP32 DevKit V1
-----------    ---------------
VCC / 3V3  ->  3.3V
GND        ->  GND
SCL        ->  GPIO 18  (HW SPI SCK)
SDA        ->  GPIO 23  (HW SPI MOSI)
RES        ->  GPIO 4   (D4)
DC         ->  GPIO 2   (D2)
CS         ->  GND on module (no wire needed)
```

---

## Software Setup

### Libraries (install via Arduino IDE Library Manager)

**Master** requires:
- `ArduinoJson` >= 7.0 by bblanchon
- `U8g2` >= 2.35 by olikraus

**Worker** requires:
- No extra libraries needed

### Board settings (Arduino IDE)
- Board: **ESP32 Dev Module**
- Upload Speed: 921600
- Partition Scheme: Default

---

## Configuration

### Master — edit the top of `MeshMiner32.ino`

```cpp
#define WIFI_SSID     "YourWiFiName"
#define WIFI_PASSWORD "YourWiFiPassword"
#define POOL_USER     "bc1qYourBitcoinAddress.MeshMiner32"
```

> Your Bitcoin address must be a native SegWit `bc1q...` address.
> Get one free from [BlueWallet](https://bluewallet.io) or [Electrum](https://electrum.org).

### Worker — edit the top of `MeshMiner32_Worker.ino`

```cpp
#define ESPNOW_CHANNEL  11   // Must match your router's WiFi channel
```

> Flash the master first. Check Serial Monitor for `[Main] WiFi channel: N`.
> Set that number here on all worker boards.

---

## Flashing

1. Open `MeshMiner32.ino` in Arduino IDE — flash to the **master** board
2. Open `MeshMiner32_Worker.ino` in Arduino IDE — flash to each **worker** board
3. Power both boards — workers auto-discover the master within seconds

Each `.ino` file must be inside a folder with the **exact same name**.

---

## Display Pages (Master)

The OLED cycles through 4 pages every 3 seconds. Press the **BOOT button** to advance manually.

| Page | Content |
|------|---------|
| **Mining** | Spinner, hashrate, peers, accepted shares, found blocks, job ID |
| **Pool** | Pool host, port, difficulty, accepted shares |
| **Network** | WiFi SSID, IP address, RSSI signal strength, mesh node count |
| **Mesh Workers** | Each worker MAC address and hashrate |

---

## Monitoring on public-pool.io

1. Go to [https://web.public-pool.io](https://web.public-pool.io)
2. Enter your `bc1q...` Bitcoin address
3. Your miner appears as **MeshMiner32** after the first accepted share

---

## How It Works

```
Bitcoin Network
      |
      | Stratum protocol
      v
  MASTER ESP32  ---- WiFi ----  public-pool.io
      |
      | ESP-NOW (2.4GHz, no router needed)
      |
  +---+-------------------+
  |                       |
WORKER 1              WORKER 2
nonce 0x00000000      nonce 0x80000000
  to 0x7FFFFFFF          to 0xFFFFFFFF
```

- Master connects to the pool and receives Bitcoin block templates
- Nonce space is split evenly: master takes chunk 0, workers get the rest
- Each node hashes its range independently using double-SHA256
- If a valid nonce is found, worker sends it to master via ESP-NOW
- Master submits the result to the pool via Stratum

---

## Expected Performance

| Nodes | Combined Hashrate |
|-------|------------------|
| 1 master only | ~25 kH/s |
| 1 master + 1 worker | ~50 kH/s |
| 1 master + 3 workers | ~100 kH/s |

---

## Photos

| Mining page | Pool page |
|-------------|-----------|
| ![Mining](photos/mining.jpg) | ![Pool](photos/pool.jpg) |

| Network page | Mesh Workers page |
|--------------|-------------------|
| ![Network](photos/network.jpg) | ![Mesh](photos/mesh.jpg) |

---

## File Structure

```
MeshMiner32/
├── MeshMiner32.ino          # Master node sketch
├── MeshMiner32_Worker.ino   # Worker node sketch
├── photos/
│   ├── mining.jpg
│   ├── pool.jpg
│   ├── network.jpg
│   └── mesh.jpg
└── README.md
```

---

## Disclaimer

Bitcoin solo mining with an ESP32 is a hobby project. The probability of finding
a block is extremely low. This project is for educational purposes and the joy
of building — not as a financial strategy.

---

## License

MIT License — free to use, modify and share. Credit appreciated but not required.

---

## Acknowledgements

Inspired by the original [NerdMiner v2](https://github.com/BitMaker-hub/NerdMiner_v2) project by BitMaker.
Built with [U8g2](https://github.com/olikraus/u8g2) display library and [ArduinoJson](https://arduinojson.org).
Solo mining via [public-pool.io](https://web.public-pool.io) — 0% fee, open source.
