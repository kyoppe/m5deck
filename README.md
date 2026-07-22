# m5deck

Desk dashboard firmware for **M5Stack Core2**. Switch between clock panels, weigh plants with a Mini Scales Unit, sync readings to [Agavydration](https://github.com/kyouhei-ohno/agavydration), and push device health metrics to Datadog.

Detailed guides (Japanese): [docs/README.md](docs/README.md)

## Current features

| Area | Status | Notes |
|------|--------|-------|
| Digital clock (DSEG7) | Done | NTP + RTC fallback |
| Analog clock (Muji station style) | Done | Button C toggles panel |
| Battery overlay | Done | Top-right on both panels |
| Display Dim / Sleep on battery | Done | USB: dim after 5min only (no sleep); see [docs/display-power.md](docs/display-power.md) |
| Weight mode + Mini Scales Unit | Done | I2C `0x26`, PORT-A G32/G33 |
| Agavydration device API | Done | See [docs/agavydration.md](docs/agavydration.md) |
| Datadog custom metrics (heartbeat) | Done | See [docs/datadog-metrics.md](docs/datadog-metrics.md) |
| Datadog alert siren (Worker relay) | In tree, **off** | `ENABLE_LEGACY_EXTRAS=0` in `main.cpp` |
| Google Calendar reminders | In tree, **off** | Same flag |
| IMU "pick me up" reaction | In tree, **off** | Same flag |
| SwitchBot temperature/humidity | Planned | |
| Datadog Query Value on screen | Planned | |

## Hardware

- M5Stack Core2 (ESP32, 320x240 touch LCD)
- Optional: [Mini Scales Unit](https://docs.m5stack.com/en/unit/miniscales) on PORT-A (SDA=32, SCL=33)

## Quick start

```bash
export PATH="$HOME/.local/bin:$PATH"

cp include/secrets.h.example include/secrets.h
# Edit secrets.h (WiFi, Agavydration, Datadog)

pio run
pio run -t upload
pio device monitor    # 115200 baud
```

## Configuration (`include/secrets.h`)

| Define | Purpose |
|--------|---------|
| `WIFI_SSID` / `WIFI_PASS` | Station WiFi |
| `AGAV_API_URL` | Agavydration base URL (e.g. `https://agavydration.pages.dev`) |
| `AGAV_DEVICE_TOKEN` | Bearer token for `/api/device/*` |
| `AGAV_CF_ACCESS_*` | Cloudflare Access service token (production HTTPS) |
| `DD_API_KEY` / `DD_SITE` | Datadog Metrics API v2 (optional; metrics disabled if empty) |
| `ALERT_WORKER_URL` / `ALERT_DEVICE_TOKEN` | Legacy alert relay (only if `ENABLE_LEGACY_EXTRAS=1`) |

`secrets.h` is gitignored. Use `secrets.h.example` as a template.

## Build flags (`platformio.ini`)

| Flag | Default | Meaning |
|------|---------|---------|
| `AGAV_METRIC_PREFIX` | `kyouhei.iot` | Datadog metric namespace |
| `M5DECK_VERSION` | git short SHA | Injected by `scripts/git_version.py` at build time |

## Repository layout

```
m5deck/
├── src/
│   ├── main.cpp           # Clock panels, weight mode, UI loop
│   ├── agavydration.*     # Plant list, stable-weight detect, POST readings
│   ├── agav_thumb.*       # Thumbnail download/cache for plant picker
│   ├── agav_metrics.*     # Background Datadog metrics (core0, 15s)
│   ├── agav_network.*     # HTTP mutex across tasks
│   └── agav_ui.*          # Weight / Agavydration screens
├── include/
│   ├── secrets.h.example
│   └── agav_tls.h         # TLS CA bundles (Cloudflare + Datadog API)
├── cloud/                 # Datadog alert relay Worker (legacy extras)
├── docs/                  # Integration and operations guides
├── scripts/git_version.py
└── platformio.ini
```

## Runtime architecture

- **core1 (Arduino `loop`)**: Display, touch, buttons, weight sampling
- **core0 (FreeRTOS tasks)**: HTTP (plants fetch, reading POST, thumbnails, metrics)
- **`AgavNetworkGuard`**: Serializes HTTP so tasks do not overlap

WiFi connects once at boot (`syncTime()`). NTP syncs system time and RTC; clock works from RTC if WiFi fails.

## Cloud components

| Path | Role |
|------|------|
| [cloud/README.md](cloud/README.md) | Cloudflare Worker: Datadog monitor webhooks to M5 alert polling (legacy) |
| [docs/agavydration.md](docs/agavydration.md) | Device-side Agavydration integration |
| [docs/display-power.md](docs/display-power.md) | Dim/Sleep on battery, USB bypass |
| [docs/battery-runtime-test.md](docs/battery-runtime-test.md) | Battery runtime test via Datadog metrics |

## Roadmap

- [x] PlatformIO + M5Unified setup
- [x] Clock panels (digital + analog, NTP)
- [x] Weight mode + Agavydration Phase 2 device API
- [x] Datadog device metrics (heartbeat, CPU, memory, battery)
- [ ] SwitchBot indoor temp/humidity panel
- [ ] On-device Datadog Query Value display
- [ ] Panel picker UI (touch)
