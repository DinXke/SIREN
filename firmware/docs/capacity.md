# Phase 8: Capacity Test Results

**Date**: [TBD]
**Firmware Base**: base-room-server-v1.16.0 + Phase 1-7 implementations
**Branch**: multiroom
**Hardware**: Heltec LoRa32 V3 (ESP32-S3FN8, SX1262, no PSRAM)

## Test Environment

### Configuration
- **Rooms**: 4 rooms across all nodes (D1-FIRE, D2-MED, D3-POL, CP-OPS)
- **Nodes**: [Number of devices used - minimum 2 for replication proof]
- **MAX_CLIENTS per room**: [Value from build]
- **MAX_UNSYNCED_POSTS per room**: [Value from build]
- **LoRa duty cycle budget**: 1.0% (test target: < 0.8%)

### Baseline (Phase 0)
- RAM usage: 40.5% (single identity, stock firmware)
- Flash usage: 34.7%

## Measurements

### 1. RAM Headroom

| Metric | Value | Pass? |
|---|---|---|
| Free heap at idle (all 4 rooms loaded) | [XX] bytes | ✓/✗ |
| Free heap at peak (all clients connected) | [XX] bytes | ✓/✗ |
| Percentage free at peak | [XX]% | >20% ✓/✗ |
| Largest consecutive free block | [XX] bytes | ✓/✗ |

**Measurement method**: Serial console `free()` call at idle and under load; LoRa-tool heap dump at peak load.

**Load scenario**:
- All 4 rooms online
- MAX_CLIENTS per room connecting
- All 4 posts per room queued
- Replication sync in progress

### 2. LoRa Duty Cycle

| Scenario | On-Air Time (ms/min) | Duty Cycle % | Pass? |
|---|---|---|---|
| Idle (adverts only) | [XX] | [X.X]% | ✓/✗ |
| Normal operation (4 posts/min per room) | [XX] | [X.X]% | <0.8% ✓/✗ |
| Peak load (16 posts/min all rooms) | [XX] | [X.X]% | <0.8% ✓/✗ |
| Replication sync (2-node) | [XX] | [X.X]% | ✓/✗ |

**Measurement method**: LoRa radio on-air time tracking (SX1262 TX/RX state register polling); wall-clock 60-second measurement windows.

**Normal operation baseline**: 1 post per room per minute (4 posts/min total).

### 3. SYNCTRUNC (Eviction Handling)

| Condition | Behavior | Pass? |
|---|---|---|
| Post queue full (>32 posts) | Oldest post evicted, clients notified gracefully | ✓/✗ |
| Replication with full queue | SYNCDAT frames not lost; version vector updates correctly | ✓/✗ |
| Evicted post recovery | Client can re-request post if not yet aged out | ✓/✗ |

**Measurement method**: Artificially flood one room to >32 posts; observe serial console and peer device reconciliation.

### 4. Advert Spacing

| Metric | Value | Pass? |
|---|---|---|
| Adverts per minute (idle) | [XX] | ✓/✗ |
| Burst count (consecutive adverts) | [XX] | 1 ✓/✗ |
| Spacing variance | [XX] ms | <10% ✓/✗ |
| Replication advert overhead | [XX] adverts/min | ✓/✗ |

**Measurement method**: Serial monitor regex match on `[ADVERT]` messages; timestamp analysis.

### 5. Soak Test (30 Minutes)

| Duration | Stability | Pass? |
|---|---|---|
| 30 minutes, 4 rooms, realistic load | No crashes, no resets, no stack overflow | ✓/✗ |
| Replication sync over 30 min | All 4 rooms on all nodes identical at end | ✓/✗ |
| Memory fragmentation | No heap exhaustion, malloc() always succeeds | ✓/✗ |

**Load**: 1-2 posts per room per minute, clients randomly connecting/disconnecting.

**Acceptance**: No crashes, no resets, all rooms functional at end.

## Results & Analysis

### Summary
- [PASS/FAIL] all criteria
- [Percentage] headroom available
- [Percentage] duty cycle under load
- Limiting factor (if any): [RAM / duty cycle / other]

### Findings
[Space for observations about bottlenecks, unexpected behaviors, tuning opportunities]

### Tuning Decisions

If acceptance criteria not met, document changes:

| Parameter | Current | Proposed | Justification |
|---|---|---|---|
| MAX_UNSYNCED_POSTS | [N] | [N] | [Reason: RAM / duty cycle constraint] |
| MAX_CLIENTS | [N] | [N] | [Reason: RAM / duty cycle constraint] |
| ADVERT_INTERVAL | [ms] | [ms] | [Reason: duty cycle / responsiveness] |

**Impact**: [Describe any user-facing changes or degradation]

## Sign-Off

- **Measured by**: [Agent name + date]
- **Reviewed by**: CTO (required before declaring complete)
- **Capacity proven**: ✓/✗

---

## References

- Spec: [JES-712] MESHCORE MULTIROOM HANDOFF (1), sections 4 & 2.4
- Phase 0 baseline: [JES-713] commitment record
- Phase 7 prerequisite: [JES-725] Join via DM + tokens (must be merged first)
