# Display power (Dim / Sleep)

Clock screen backlight management. Weight mode always stays fully bright.

Implementation: `src/main.cpp` (`displayPower*` helpers)

## Goals

- Save battery when the device sits idle on a scale
- Keep the two-tap UX: wake display first, then enter weight mode
- On USB power: Dim after idle, but no Sleep (backlight never fully off)

## States

| State | Brightness | Battery (unplugged) | USB connected |
|-------|------------|---------------------|---------------|
| **Awake** | 110 | Default after activity | Default after activity |
| **Dim** | 12 | After 45s idle | After **5min** idle |
| **Sleep** | 0 (backoff) | After 45s + 3min idle | **Never** |

Constants in `main.cpp`:

- `DISPLAY_IDLE_DIM_MS` = 45s (battery)
- `DISPLAY_IDLE_SLEEP_MS` = 3min after dim threshold (battery only)
- `DISPLAY_USB_IDLE_DIM_MS` = 5min (USB, dim only)

## USB detection

Core2 (AXP192): USB cable = **ACIN** (`M5.Power.Axp192.isACIN()`). Do not use `getVBUSVoltage()`; M-Bus 5V stays up on battery.

Shared helper: `include/agav_power.h` (`agavUsbCableConnected()`), used by display power and Datadog metrics.

When USB is connected:

- After 5 minutes without touch or button C, backlight dims (not off)
- Sleep is not used
- Touch still wakes to Awake first; second touch enters weight mode
- Plugging in does not reset the idle timer; unplugging applies battery rules on the next tick

`isCharging()` is not used for this gate (full battery can show as not charging while USB is still connected).

## Touch flow (clock screen)

1. **Dim or Sleep** + touch: wake to Awake only (does not enter weight mode)
2. **Awake** + touch: enter weight mode

## Other behavior

| Situation | Behavior |
|-----------|----------|
| Weight mode | Always Awake; Dim/Sleep disabled |
| Button C (panel toggle) | Counts as activity, resets idle timer |
| Sleep (battery only) | Skips clock redraw; wakes on touch with `forceDraw` |
| WiFi / Datadog metrics | Keep running in all display states |

## Serial logs

```
display power -> 1 (usb=0)   # Dim
display power -> 2 (usb=0)   # Sleep
display power -> 0 (usb=1) # Awake
```

## Related

- [battery-runtime-test.md](battery-runtime-test.md): measure runtime on battery using Datadog metrics
- [datadog-metrics.md](datadog-metrics.md): metric names and tags
