# SIREN — Claude Agent Instructions

## Dual-build rule (board directive, JES-827, 2026-08-01)

**Every firmware change MUST produce binaries for BOTH Heltec V3 and Heltec V4.**

Whenever you build firmware (any feature, fix, or change), run:

```bash
bash scripts/build-dist.sh
```

This builds both targets and copies artifacts to:

- `dist/heltec_v3/SIREN_v3_room_server.bin` — OTA binary for Heltec V3
- `dist/heltec_v3/SIREN_v3_room_server-full-flash.bin` — initial serial flash for Heltec V3
- `dist/heltec_v4/SIREN_v4_room_server.bin` — OTA binary for Heltec V4
- `dist/heltec_v4/SIREN_v4_room_server-full-flash.bin` — initial serial flash for Heltec V4

Both sets of `dist/` binaries must be committed in the same commit as the firmware change. Never commit a firmware change with only one target's binaries updated.

### PlatformIO environments

| Target | PIO environment |
|---|---|
| Heltec V3 | `SIREN_v3_room_server` |
| Heltec V4 | `SIREN_v4_room_server` |

### Build individually (only when testing one target)

```bash
bash scripts/build-dist.sh v3   # V3 only
bash scripts/build-dist.sh v4   # V4 only
```

Never commit a single-target build to the `multiroom` branch.

## Branch

Active development branch: `multiroom`. Always push to `origin multiroom` after commits.

## Docs rule

Every feature issue must update the relevant `docs/` files before closing. Push docs commits immediately (`git push origin multiroom`). See `docs/CONTRIBUTING-DOCS.md` for the full docs workflow.
