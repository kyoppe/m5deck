# Battery runtime test (Datadog metrics)

How to measure how long m5deck runs on battery using existing Datadog custom metrics. Use this when evaluating Dim/Sleep and overall power draw on the scale setup.

## What you are measuring

**End of test**: device stops sending metrics (WiFi down, brownout, or shutdown).

**Primary signal**:

```
kyouhei.iot.device.running{device:m5deck}
```

While the device is alive and posting every 15s, this gauge stays at `1`. When the series stops, runtime ended.

Metric prefix: `kyouhei.iot` (`platformio.ini` `AGAV_METRIC_PREFIX`).

## Supporting metrics

| Metric | Use in analysis |
|--------|-----------------|
| `*.battery.pct` | State of charge over time (`charging:true/false` tag) |
| `*.cpu.user` | Load spikes (TLS POST every 15s, weight mode) |
| `*.memory.pct_usable` | Leaks or instability before death |
| `*.device.running` | Liveness / total runtime |

All carry `version:<git-sha>` for firmware revision.

## Recommended test procedure

1. **Flash** firmware with Dim/Sleep enabled and metrics configured (`secrets.h` `DD_API_KEY`).
2. **Full charge** or note starting `battery.pct` in Datadog.
3. **Unplug USB** (VBUS gone). Confirm `usb_connected:false` on `device.running` / `battery.pct`.
4. **Do not touch** the screen (let Dim at 45s, Sleep at ~3m45s unless you change constants).
5. **Wait** until `device.running` stops (or note time when battery hits 0% if device still runs).
6. **Record**:
   - Start time (first `device.running` after unplug)
   - End time (last point)
   - Duration
   - Start / end `battery.pct`
   - Firmware `version` tag

## Datadog queries

**Liveness (last hour)**:

```
avg:kyouhei.iot.device.running{device:m5deck}
```

**Battery trend**:

```
avg:kyouhei.iot.battery.pct{device:m5deck} by {usb_connected,charging}
```

**Unplug moment** (running series tag flips):

```
avg:kyouhei.iot.device.running{device:m5deck} by {usb_connected}
```

**Runtime window**: use Metrics Explorer or Notebooks, zoom from unplug to last heartbeat.

**Monitor (optional, after baseline)**:

```
avg(last_5m):kyouhei.iot.device.running{device:m5deck} < 1
```

## Interpreting results

| Pattern | Likely meaning |
|---------|----------------|
| `battery.pct` falls smoothly, then series stops | Normal battery exhaustion |
| Series stops with battery still > 0% | WiFi drop, crash, or brownout (check serial if possible) |
| Flat `battery.pct` at 100% with `charging:false` | Estimation quirk on USB unplug; trust trend after a few minutes |
| CPU spikes every 15s | Expected (Datadog POST on core0) |

Dim/Sleep reduces backlight power; expect longer runtime than always-on brightness 110. Compare runs with the same starting charge and network conditions.

## Results log (fill in after test)

| Field | Value |
|-------|-------|
| Date | 2026-07-22 / 2026-07-23 |
| Firmware `version` tag | `2c913f0` (metric prefix `test.kyouhei.iot`; no `usb_connected` tag) |
| Start `battery.pct` | 100% |
| Unplug time (JST) | ~2026-07-22 20:37 (first `battery.pct` drop to 99%; held 100% while on USB) |
| Last `device.running` time (JST) | ~2026-07-23 00:30:40 (20s rollup; next point absent) |
| **Runtime** | **~3h 54m** (20:37 unplug to last heartbeat) |
| End `battery.pct` (if seen) | 0% from ~00:27; device still posted until ~00:30 |
| Display reached Sleep? (Y/N) | N (pre Dim/Sleep firmware) |
| Notes | Baseline always-on display. `pup metrics query` on `test.kyouhei.iot.*{device:m5deck}`. Metrics span 20:18-00:30 JST if counted from first post (~4h 12m). |

### Run 1 detail (2026-07-22 baseline)

- **Unplug to last heartbeat**: 3h 53m 30s (~3.9 h)
- **Unplug to `battery.pct` 0%**: 3h 49m 50s
- **cpu.user**: ramped 34% -> 68% in first minutes, then flat ~92.5% for hours (measurement artifact; see [datadog-metrics.md](datadog-metrics.md))

## Related

- [display-power.md](display-power.md): Dim/Sleep timing and USB bypass
- [datadog-metrics.md](datadog-metrics.md): full metric catalog
