#pragma once

// Start the background Datadog metrics task (core0, 15s interval).
// No-op when DD_API_KEY is not configured in secrets.h.
void agavMetricsStart();
