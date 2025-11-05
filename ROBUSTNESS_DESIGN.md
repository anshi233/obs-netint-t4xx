# Robustness and Error Handling Design

## Overview

This document describes the robustness features and error handling strategies implemented in the NETINT T4XX encoder plugin. The design focuses on making the plugin more reliable, easier to debug, and able to recover from common failure scenarios.

## Error Handling Strategy

### Philosophy

**Fail Fast → Recover → Report**

1. **Fail Fast**: When errors occur, we detect them quickly and fail operations immediately
2. **Recover**: Attempt automatic recovery when possible (up to a limit)
3. **Report**: Provide detailed error information to help debugging

**Decision: When to restart vs. throw error**

- **Restart/Recover**: Transient errors (network glitches, temporary hardware issues)
  - Up to `MAX_RECOVERY_ATTEMPTS` (3) recovery attempts
  - Errors that might be recoverable (encoder hang, temporary communication failure)
  
- **Throw Error**: Persistent errors or recovery exhaustion
  - After `MAX_CONSECUTIVE_ERRORS` (5) consecutive errors
  - After `MAX_RECOVERY_ATTEMPTS` (3) failed recovery attempts
  - Hardware failures that can't be recovered from software

When encoder fails, we return `false` to OBS, which will:
1. Destroy the failed encoder instance
2. Create a new encoder instance (if streaming/recording continues)
3. This allows OBS to handle the lifecycle properly

## Error Tracking

### Error Counters

- **`consecutive_errors`**: Count of consecutive errors (reset on success)
  - Used to detect persistent failures
  - When reaches `MAX_CONSECUTIVE_ERRORS`, encoder marked as FAILED
  
- **`total_errors`**: Total errors since encoder creation
  - Useful for debugging and statistics
  - Never reset (except on encoder recreation)

- **`recovery_attempts`**: Number of recovery attempts made
  - Prevents infinite recovery loops
  - Max attempts: `MAX_RECOVERY_ATTEMPTS`

### Timestamps

- **`last_packet_time`**: When last packet was received
  - Used to detect encoder hangs
  - If no packet for `ENCODER_HANG_TIMEOUT_SEC` (10s), encoder is considered hung

- **`last_frame_time`**: When last frame was sent to encoder
  - Helps diagnose if encoder is receiving frames but not producing packets

- **`last_error_time`**: When last error occurred
  - Helps track error frequency

- **`encoder_start_time`**: When encoder was created
  - Used for uptime statistics

## Encoder States

The encoder can be in one of these states:

1. **NORMAL**: Operating normally, no errors
2. **ERROR**: Encountered errors but still trying (consecutive_errors < MAX)
3. **HUNG**: No packets received for too long (hang detection)
4. **RECOVERING**: Attempting recovery from error/hang
5. **FAILED**: Encoder has failed and should be recreated by OBS

### State Transitions

```
NORMAL → ERROR (on error)
ERROR → NORMAL (on success after error)
ERROR → FAILED (after MAX_CONSECUTIVE_ERRORS)
NORMAL → HUNG (no packets for timeout)
HUNG → RECOVERING (recovery attempt)
RECOVERING → NORMAL (recovery success)
RECOVERING → FAILED (max recovery attempts)
```

## Health Monitoring

### Hang Detection

The encoder monitors packet reception timing:
- If no packets received for `ENCODER_HANG_TIMEOUT_SEC` (10 seconds)
- AND encoder is not in flushing mode
- Encoder is marked as HUNG

**Why 10 seconds?**
- At 30fps, 10 seconds = 300 frames
- Hardware encoders typically have low latency (< 100ms)
- 10 seconds is clearly abnormal and indicates a problem

### Health Checks

Health checks are performed:
- Periodically during encoding (every 10 frames)
- Before critical operations
- When errors occur

## Recovery Strategies

### Current Recovery Strategy

1. **Reset error counters** - Clear consecutive error count
2. **Reset state** - Return encoder to NORMAL state
3. **Reset packet timing** - Give encoder fresh start for hang detection

### Future Recovery Strategies (if API supports)

1. **Close and reopen encoder** - If libxcoder supports hot-reconnect
2. **Reset encoder hardware** - If API provides reset function
3. **Reinitialize encoder parameters** - If parameters can be changed on the fly

### Recovery Limits

- Maximum recovery attempts: `MAX_RECOVERY_ATTEMPTS` (3)
- After max attempts, encoder marked as FAILED
- OBS will recreate encoder (handled automatically)

## Error Logging

### Error Context

Every error is logged with:
- **Operation name**: Which operation failed (e.g., "encode_get_frame")
- **Error message**: What went wrong
- **Error counts**: Consecutive and total errors
- **Encoder state**: Current state of encoder
- **Timestamps**: When error occurred, encoder uptime

### Error Levels

- **LOG_WARNING**: Transient errors, recovery possible
- **LOG_ERROR**: Persistent errors, encoder may fail
- **LOG_INFO**: Recovery attempts, state changes

### Example Error Log

```
[obs-netint-t4xx] Encoder error in 'encode_send': Failed to send frame to hardware encoder (consecutive: 2/5, total: 15)
[obs-netint-t4xx] Encoder appears HUNG: no packets for 12 seconds
[obs-netint-t4xx] Attempting encoder recovery (attempt 1/3)...
[obs-netint-t4xx] Encoder recovered from 2 consecutive errors
[obs-netint-t4xx] Encoder FAILED after 5 consecutive errors in operation 'encode_get_frame': Failed to get frame buffer
[obs-netint-t4xx] Encoder stats: total_errors=25, recovery_attempts=3, uptime=123.4s
```

## Thread Safety

### Mutexes

- **`queue_mutex`**: Protects packet queue
- **`state_mutex`**: Protects encoder state and error counters

### Thread Lifecycle

- **Main thread**: Calls `encode()`, checks health, handles errors
- **Receive thread**: Receives packets, updates timestamps, detects errors

Both threads access shared state (error counters, timestamps) through mutexes.

## Debugging Features

### Error Messages

- Last error message stored in `last_error_msg` (256 bytes)
- Can be accessed for debugging (if needed)

### Statistics

On encoder failure, full statistics are logged:
- Total errors
- Recovery attempts
- Encoder uptime
- Last error operation and message

### Health Monitoring

Health checks provide visibility into:
- Packet reception rate
- Frame sending rate
- Error frequency
- Encoder state

## Configuration

### Tunable Parameters

All timeouts and limits are defined as constants:
- `MAX_CONSECUTIVE_ERRORS`: 5
- `ENCODER_HANG_TIMEOUT_SEC`: 10 seconds
- `MAX_RECOVERY_ATTEMPTS`: 3
- `MAX_PKT_QUEUE_SIZE`: 10 packets

These can be adjusted based on:
- Hardware characteristics
- Network conditions
- Use case requirements

## Best Practices for Users

### When Encoder Fails

1. **Check OBS logs** - Full error context is logged
2. **Check hardware** - Verify NETINT card is functioning
3. **Check drivers** - Ensure libxcoder is up to date
4. **Restart OBS** - If encoder keeps failing, restart OBS Studio

### Debugging Tips

1. **Enable verbose logging** - OBS log level can show more details
2. **Monitor error counts** - Check if errors are transient or persistent
3. **Check timing** - Look at packet/frame timing in logs
4. **Check state transitions** - See how encoder state changes

## Future Improvements

1. **Exponential backoff** - For recovery attempts
2. **Health metrics export** - For monitoring tools
3. **Configurable thresholds** - Via OBS settings
4. **Better recovery** - If libxcoder API supports hot-reconnect
5. **Packet dropping** - When queue is full (instead of just warning)

## Summary

The plugin now has:
- ✅ Comprehensive error tracking
- ✅ Automatic recovery (with limits)
- ✅ Hang detection
- ✅ Detailed error logging
- ✅ Health monitoring
- ✅ Graceful failure handling

When encoder fails, OBS automatically recreates it, providing seamless user experience while maintaining detailed diagnostics for debugging.

