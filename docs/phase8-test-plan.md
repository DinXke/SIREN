# Phase 8: Capacity Test Plan

## Overview
Phase 8 validates that the multiroom firmware meets capacity requirements on the Heltec LoRa32 V3:
- RAM headroom > 20% under peak load (all 4 rooms, all clients)
- LoRa duty cycle < 0.8% under realistic message volume
- Stable operation for 30+ minutes with no crashes
- Graceful handling of post queue overflow

## Acceptance Criteria

| Metric | Target | Pass Condition |
|---|---|---|
| Free heap at peak | >20% | Measured via `free()` |
| LoRa duty cycle | <0.8% | Calculated: TX/RX ms per 60s window |
| Stability soak | 30 min | No crashes, resets, or stack overflow |
| Advert spacing | 1 per burst | No consecutive adverts |

## Test Hardware

- **Nodes**: Minimum 2 Heltec LoRa32 V3 devices (MAIN + BACKUP)
- **USB connections**: Both nodes connected to laptop for serial monitoring
- **LoRa antennas**: ~2-5 meters apart (office environment)
- **Power**: USB powered (stable 5V supply)

## Test Sequence

### 1. Pre-Test Setup (30 min)

```bash
# On SIREN repo, multiroom branch, latest Phase 7 merge
git checkout multiroom
git pull origin multiroom

# Build Phase 8 firmware
cd firmware
pio run -e SIREN_v3_room_server

# Flash both nodes
pio run -e SIREN_v3_room_server -t upload  # Node A (MAIN)
# [Swap USB to other device]
pio run -e SIREN_v3_room_server -t upload  # Node B (BACKUP)

# Configure rooms via serial CLI (Phase 3 prerequisite)
# Node A:
#   room create D1-FIRE MAIN <key_A>
#   room create D2-MED MAIN <key_A>
#   room create D3-POL MAIN <key_A>
#   room create CP-OPS MAIN <key_A>
#
# Node B:
#   room create D1-FIRE BACKUP <key_A>
#   room create D2-MED BACKUP <key_A>
#   room create D3-POL BACKUP <key_A>
#   room create CP-OPS BACKUP <key_A>
```

### 2. Baseline RAM Measurement (5 min)

**All 4 rooms online, no clients connected**

```bash
# On each node, serial console:
> free
  [Record heap stats]

# On laptop, capture output:
python3 measure_ram.py <PORT_A> <PORT_B>
```

Expected: ~50% RAM used (all room metadata, no post queue).

### 3. Peak RAM Load (10 min)

**All 4 rooms fully loaded: MAX_CLIENTS per room, all post queues full**

1. Connect 3-4 clients to each room (via standard MeshCore app)
2. Each room generates MAX_UNSYNCED_POSTS posts
3. Trigger replication sync (node A initiates SYNCREQ)

```bash
# Measure continuously:
python3 measure_ram.py <PORT_A> <PORT_B>

# Record timestamps and heap values
# Expected: >20% free heap at all times (PASS)
# If <20%: FAIL, document which room/client count causes overflow
```

### 4. LoRa Duty Cycle - Idle (5 min)

**All rooms online, no clients, adverts only**

```bash
# Measure TX/RX time on SX1262 radio
python3 measure_duty_cycle.py <PORT_A> 60  # 60-second window

# Record: [on-air ms] / 60000 = duty cycle %
# Expected: <0.1% (adverts only, ~32 bytes every 30s per room)
```

### 5. LoRa Duty Cycle - Normal Load (10 min)

**4 posts per minute total (1 per room), clients connected**

1. Start post generation script:
   ```bash
   python3 post_generator.py <PORT_A> --rooms 4 --rate 1 --duration 600
   ```
2. Measure duty cycle continuously:
   ```bash
   python3 measure_duty_cycle.py <PORT_A> 60 --continuous
   ```

Expected: <0.5% duty cycle (posts + adverts + ACKs)

### 6. LoRa Duty Cycle - Peak Load (10 min)

**16 posts per minute (4 per room), replication sync active**

```bash
# Trigger node A -> node B sync every 10 seconds
python3 sync_trigger.py <PORT_A> --interval 10 --duration 600

# Generate posts at high rate
python3 post_generator.py <PORT_A> --rooms 4 --rate 4 --duration 600

# Measure duty cycle
python3 measure_duty_cycle.py <PORT_A> 60 --continuous
```

Expected: <0.8% duty cycle (limit with margin)

### 7. SYNCTRUNC Test (5 min)

**Post queue overflow and eviction**

1. Fill one room's queue beyond MAX_UNSYNCED_POSTS:
   ```bash
   python3 post_generator.py <PORT_A> room=D1-FIRE --count 50 --burst
   ```
2. Observe serial console for eviction messages
3. Verify clients on node B don't crash when receiving SYNC with older seq numbers

Expected:
- Oldest posts evicted gracefully
- Version vector advanced correctly
- No duplicate posts on peers

### 8. Advert Spacing Check (5 min)

**Verify adverts are not bursted**

```bash
# Capture serial monitor, filter for ADVERT messages
python3 measure_adverts.py <PORT_A> 300  # 5-minute window

# Parse timestamps, check:
#   - No two consecutive adverts <100ms apart
#   - Average spacing ~30s per room (30s * 4 rooms = 7.5 adverts/min)
```

Expected: 7-10 adverts per minute, never bursted.

### 9. Soak Test (30+ min)

**Realistic operation: 1-2 posts/room/min, clients randomly connecting**

```bash
# Run simultaneously on both nodes:
python3 soak_test.py <PORT_A> <PORT_B> \
  --duration 1800 \
  --rooms 4 \
  --post-rate 1 \
  --client-connect-rate 0.5 \
  --replication-interval 60
```

Monitor for:
- No crashes or watchdog resets
- No stack overflows
- RAM does not fragment (malloc always succeeds)
- All 4 rooms remain responsive
- Posts replicate correctly across nodes

Record at 5-min intervals:
- Free heap on each node
- Duty cycle measurements
- Post sync lag (time for post to reach peer)

Expected: PASS all 30 minutes without intervention

## Measurement Scripts (stubs for implementation)

### measure_ram.py
```python
#!/usr/bin/env python3
# Reads 'free' command from serial, logs heap stats
# Usage: measure_ram.py <PORT_A> <PORT_B>
```

### measure_duty_cycle.py
```python
#!/usr/bin/env python3
# Polls SX1262 state register, calculates TX/RX time ratio
# Usage: measure_duty_cycle.py <PORT> [--continuous]
```

### measure_adverts.py
```python
#!/usr/bin/env python3
# Captures ADVERT messages from serial, analyzes spacing
# Usage: measure_adverts.py <PORT> <DURATION_SECONDS>
```

### post_generator.py
```python
#!/usr/bin/env python3
# Injects posts via serial CLI
# Usage: post_generator.py <PORT> --rooms 4 --rate 1 --duration 600
```

### soak_test.py
```python
#!/usr/bin/env python3
# Full soak test harness: generates load, monitors both nodes
# Usage: soak_test.py <PORT_A> <PORT_B> --duration 1800 --rooms 4
```

## Troubleshooting

### If RAM test fails (<20% free):
- Document which room/client count causes overflow
- Check: MAX_CLIENTS, MAX_UNSYNCED_POSTS, post struct size
- Propose tuning in capacity.md
- May need Phase 1-3 refactoring (e.g., reduce PostInfo size)

### If duty cycle exceeds 0.8%:
- Identify which operation dominates (adverts, replication, ACKs)
- Check: ADVERT_INTERVAL, SYNCDAT packet size, ACK frequency
- Propose: longer advert interval, smaller SYNCDAT, ACK batching
- Document impact on user experience

### If soak test crashes:
- Capture full serial log from crash point
- Check: stack overflow, malloc failure, buffer overrun
- May need architectural changes in post queue or replication

## Deliverables

1. **firmware/docs/capacity.md** — Filled with all measurements, results, tuning decisions
2. **SIREN repo** — Commit measurement scripts to tools/ directory (optional)
3. **Comment on JES-726** — Executive summary of test results, pass/fail status, any recommendations

## Approval Flow

1. Phase 8 testing complete
2. Results posted to JES-726
3. CTO review (5 day SLA)
4. If tuning needed: create follow-up Phase 8b issue, repeat test
5. If PASS: mark JES-726 done, unblock Phase 8 on SIREN roadmap

---

**Owner**: JuniorNetworkEngineer (066d746d)
**Prerequisites**: JES-725 (Phase 7) merged to multiroom
**Status**: [BLOCKED] waiting for Phase 7
