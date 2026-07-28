# MeshCore Upgrade Runbook

This document describes how to upgrade the vendored MeshCore firmware in the SIREN repository
to a new upstream release (e.g. v1.17, v1.18, ...).

## TL;DR

```bash
# One command to upgrade
./scripts/vendor-meshcore.sh room-server-v1.17.0

# Verify the build
cd firmware && pio run -e SIREN_v3_room_server

# Push
git push origin multiroom --follow-tags
```

---

## Background

`firmware/` is a vendored copy of [ripplebiz/MeshCore](https://github.com/ripplebiz/MeshCore).
It is tracked in git as regular files (not a submodule), so PlatformIO offline builds work
without any git connectivity.

The `meshcore-upstream` remote points to the upstream repo. Upgrade tags follow the pattern:

| Firmware type | Tag pattern          | Example                  |
|---------------|----------------------|--------------------------|
| Room server   | `room-server-vX.Y.Z` | `room-server-v1.16.0`    |
| Companion     | `companion-vX.Y.Z`   | `companion-v1.16.0`      |
| Repeater      | `repeater-vX.Y.Z`    | `repeater-v1.16.0`       |

SIREN targets the **room-server** tags.

After each upgrade, an annotated tag `base-<upstream-tag>` marks the upgrade point in SIREN history.

---

## SIREN-Specific Files

The following files in `firmware/` are SIREN additions that do **not** exist in upstream MeshCore.
The upgrade script preserves them automatically.

### New directories
| Path | What it is |
|------|-----------|
| `firmware/examples/siren_room_server/` | Multi-room server implementation (MyMesh.cpp, MyMesh.h, main.cpp) |

### Modified files
| Path | What was changed | Conflict risk |
|------|-----------------|---------------|
| `firmware/variants/heltec_v3/platformio.ini` | Appended `[env:SIREN_v3_room_server]` section at the bottom | Low — section is append-only |

The upgrade script handles `platformio.ini` automatically: it saves everything from the
`; ===  SIREN` marker onward, replaces the file with the upstream version, and re-appends the saved section.

---

## Step-by-Step Upgrade Procedure

### 1. Identify the new tag

```bash
git fetch meshcore-upstream --tags
git tag | grep room-server | sort -V | tail -5
```

The latest `room-server-vX.Y.Z` tag is the target.

### 2. Check what changed in the new release

Review the wire-format and behavioral changes before upgrading:

```bash
git diff room-server-v1.16.0 room-server-v1.17.0 -- src/Mesh.h src/Mesh.cpp
git diff room-server-v1.16.0 room-server-v1.17.0 -- src/ | grep "^[+-]" | grep -v "^[+-][+-][+-]" | head -50
```

Look specifically for:
- Changes to `createAck` / `createMultiAck` signatures → ACK format change
- Changes to `Packet::calculatePacketHash` → hash size change
- New prefs fields in `CommonCLI.cpp::loadPrefsInt/savePrefs` → NVS layout change
- `self_id` or `LocalIdentity` signature changes → multi-identity refactor impact

### 3. Run the upgrade script

```bash
./scripts/vendor-meshcore.sh room-server-v1.17.0
```

The script will:
1. Verify the working tree is clean
2. Fetch the tag
3. Replace `firmware/` with the upstream tree
4. Restore `firmware/examples/siren_room_server/`
5. Re-append the SIREN platformio.ini section
6. Commit and tag `base-room-server-v1.17.0`

### 4. Resolve conflicts (if any)

The script replaces `firmware/` wholesale, so there are no git merge conflicts.
Any "conflicts" are compile errors caused by API changes in the new upstream.

Common conflict points:

#### A. ACK hash size changed (multi-byte addresses)
**Symptom:** compile error on `createAck` / `createMultiAck` call in `siren_room_server/`

**Fix:** Update calls in `MyMesh.cpp` to use the new signature. As of v1.16:
```cpp
// Old (pre-v1.16):
createAck(uint32_t ack_crc)

// New (v1.16+):
createAck(const uint8_t* ack, uint8_t len)
// Legacy overload still available: createAck(uint32_t ack_crc) -> calls new form with len=4
```

#### B. New prefs field in NVS (scoping)
**Symptom:** NVS state from old firmware is shifted; device behaves oddly after upgrade

**Fix:** Add the new prefs field to `siren_room_server`'s prefs load/save if it extends
`CommonCLI`. As of v1.16, offsets 291-292 are `flood_max_unscoped` / `flood_max_advert`.

#### C. `self_id` moved or renamed
**Symptom:** `self_id` references in `MyMesh.cpp` fail to compile

**Fix:** Check the new location of `self_id` in `Mesh.h` and update the reference.
As of v1.16: `Mesh.h:179  LocalIdentity self_id;`

#### D. New `onBootComplete()` / `getIRQGpio()` on board
**Symptom:** link warning about missing override

**Fix:** These are no-op virtual methods in v1.16. Safe to ignore or add empty overrides.

### 5. Verify the build (Phase 0 gate)

```bash
cd firmware
pio run -e SIREN_v3_room_server 2>&1 | tail -5
```

Pass criteria:
- Flash usage ≤ 95% of target
- RAM usage ≤ 95% of target
- No linker errors
- No `-Werror` failures

### 6. Push

```bash
git push origin multiroom --follow-tags
```

---

## Tracking Spec Anchors

After each upgrade, update these file:line references in MEMORY.md and phase issues:

| Symbol | File | v1.16 line | Notes |
|--------|------|------------|-------|
| `self_id` | `src/Mesh.h` | 179 | Multi-identity replacement target |
| TX path (advert) | `src/Mesh.cpp` | 335 | `self_id.copyHashTo(...)` |
| TX path (datagram) | `src/Mesh.cpp` | 448 | `self_id.copyHashTo(...)` |
| TX path (anon datagram) | `src/Mesh.cpp` | 489 | `self_id.copyHashTo(...)` |
| ADMIN_PASSWORD | `firmware/variants/heltec_v3/platformio.ini` | 114 | Build flag |
| ROOM_PASSWORD | `firmware/variants/heltec_v3/platformio.ini` | 115 | Build flag (upstream env) |

Run this after upgrading to re-verify:

```bash
grep -n "self_id;" firmware/src/Mesh.h
grep -n "self_id.copyHashTo" firmware/src/Mesh.cpp
grep -n "ADMIN_PASSWORD\|ROOM_PASSWORD" firmware/variants/heltec_v3/platformio.ini | head -5
```

---

## v1.15 → v1.16 Changelog (Wire-Format Impact)

This section documents what changed in v1.16 that SIREN must be compatible with.

### 1. ACK hash expanded to 6 bytes (critical wire-format change)

`BaseChatMesh` now sends 6-byte ACK hashes instead of 4-byte:

```
ack_hash[0..3] = SHA-256 truncated (4 bytes, as before)
ack_hash[4]    = "extended attempt byte" from end of message
ack_hash[5]    = random byte (makes packet hash unique)
```

**Impact on SIREN:** `MyMesh.cpp` must handle 6-byte ACK hashes in `sendAckTo(...)` calls.
The `createAck`/`createMultiAck` API is now variable-length. Legacy `uint32_t` overloads
remain for backward compat.

**Wire compatibility:** A v1.16 node sending 6-byte ACKs to a pre-v1.16 node will still
deliver the ACK (extra bytes are benign in payload), but the old node will not de-dup
on them correctly. Mixed-version mesh operation is a degraded but not broken state.

### 2. Path offset overflow fix

`Mesh.cpp` line ~58: `uint8_t offset` → `uint16_t offset` for path resolution.

**Impact:** Bug fix. Corrects a misrouting issue on long paths with large hash sizes.
No wire-format change; purely behavioral.

### 3. ACK de-dup table unified

`SimpleMeshTables` removed the separate `_acks[64]` array. ACK packets are now de-duped
via the same `_hashes[160]` table as other packet types.

**Impact:** NVS-saved `SimpleMeshTables` state from v1.15 is incompatible (old format
included `_acks` + `_next_ack_idx`). A device upgrading from v1.15 that restores persisted
tables will de-dup incorrectly until the table is cleared/expired naturally.

**SIREN action:** Phase 3 (NVS persistence) should save the `SimpleMeshTables` version
field and clear on version mismatch.

### 4. Scoping prefs (advert flood control)

Two new prefs added to NVS (CommonCLI):
- Offset 291: `flood_max_unscoped` (uint8_t)
- Offset 292: `flood_max_advert` (uint8_t)

These control the advert/flood scoping the board directive mentioned.

**Impact on SIREN:** Phase 3 (CLI + NVS) must include these fields in prefs load/save.

### 5. Board API additions (MeshCore.h)

New virtual methods on the `Board` base class:
- `onBootComplete()` — called by example `setup()` when boot is done
- `getIRQGpio()` — returns DIO1 (SX1262) pin; default returns -1

**Impact on SIREN:** No action required. These are no-op defaults; SIREN board code
does not need to override them unless desired.

---

## Version History

| Base tag | Upstream tag | Date | Notes |
|----------|-------------|------|-------|
| `base-room-server-v1.16.0` | `room-server-v1.16.0` | 2026-07-28 | Initial stable baseline |
