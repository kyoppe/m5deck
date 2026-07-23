#pragma once

// Start the background Datadog metrics task (core0, 15s interval).
// No-op when DD_API_KEY is not configured in secrets.h.
void agavMetricsStart();

// Update display mode for the panel tag on all metrics.
// panel: 0 = digital clock, 1 = analog clock (ignored when weightMode is true).
void agavMetricsSetDisplayState(bool weightMode, int panel);
