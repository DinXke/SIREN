# Contributing to SIREN Documentation

This document defines the rules and process for keeping the SIREN documentation accurate and up to date.

---

## The Core Rule

**Every feature issue must update the relevant documentation before it can be closed.**

If you merge code that adds, changes, or removes behaviour, you must also update the corresponding docs page(s). Documentation is part of the Definition of Done for every issue.

**Every documentation change must be pushed to GitHub immediately after committing.**

This applies to ALL agents who write or edit documentation (board directive, 2026-07-31). Documentation that only exists locally does not count as done — the GitHub copy at `https://github.com/DinXke/SIREN/tree/multiroom/docs` is the authoritative, always-current version. After any docs commit, run `git push origin multiroom` before closing the issue.

---

## Where Documentation Lives

All documentation lives in `docs/` in the SIREN repository, on the `multiroom` branch. It is pushed to GitHub so it is always publicly accessible at:

```
https://github.com/DinXke/SIREN/tree/multiroom/docs
```

Do not put documentation only in Paperclip issue comments or documents — those are project management artefacts, not the living documentation.

---

## Documentation Structure

SIREN follows the **Diataxis framework** for documentation organisation:

| Type | Purpose | Examples in SIREN |
|---|---|---|
| **Tutorials** | Learning-oriented, step-by-step guides for beginners | [Getting Started](getting-started.md) |
| **How-to guides** | Task-oriented recipes for experienced users | CLI commands in [CLI Reference](cli-reference.md) |
| **Reference** | Information-oriented, accurate technical facts | [Architecture](architecture.md), [Glossary](glossary.md) |
| **Explanation** | Understanding-oriented context and rationale | [Introduction](introduction.md), [Replication Protocol](replication-protocol.md) |

When adding new documentation, ask which type it is and place it accordingly.

---

## Documentation Files and What They Cover

| File | Content |
|---|---|
| `docs/README.md` | Index and learning path — links to all other docs |
| `docs/introduction.md` | What SIREN is, LoRa, MeshCore — for absolute beginners |
| `docs/hardware.md` | Heltec LoRa32 V3 hardware, antenna, board units |
| `docs/getting-started.md` | First flash, OTA, radio defaults, quick-start checklist |
| `docs/architecture.md` | Multiroom model, RoomSlot, post pool, packet dispatch, SPIFFS layout |
| `docs/wifi-webui.md` | AP/STA modes, web management pages, backup/restore, OTA via web |
| `docs/cli-reference.md` | Every CLI command with syntax, examples, and expected output |
| `docs/web-clients.md` | Standalone HTML client and React client — install, use, troubleshoot |
| `docs/replication-protocol.md` | SYNCREQ/SYNCDAT/SYNCEND, version vectors, peer configuration |
| `docs/operations.md` | Boot sequence, radio gotchas, boot loop recovery, factory reset |
| `docs/glossary.md` | Definitions of all technical terms |
| `docs/meshcore-upgrade-runbook.md` | How to upgrade MeshCore to a new upstream release |
| `docs/CONTRIBUTING-DOCS.md` | This file — rules for maintaining docs |

---

## How to Update Documentation

### For a new feature

1. Identify which doc file(s) are affected.
2. Edit the file(s) to describe the new feature accurately.
3. If the feature introduces new terms, add them to `glossary.md`.
4. Update `docs/README.md` if a new doc file was added.
5. Commit the documentation changes alongside the code changes (or in a separate commit on the same branch).
6. Add to the changelog section at the bottom of this file.
7. **Push to GitHub (`git push origin multiroom`) immediately — do not leave docs commits local-only.**

### For a bug fix that changes observable behaviour

If the fix changes what users see or how commands behave, update the relevant doc section.

### For a deprecated or removed feature

Remove or strike through the relevant documentation. Do not leave outdated instructions that will confuse users.

---

## Technical Accuracy Rules

1. **Verify against source code, not memory.** Before writing a doc, read the relevant firmware file to confirm the behaviour.
2. **Verify build flags.** Radio defaults, `MAX_ROOMS`, etc. come from `firmware/variants/heltec_v3/platformio.ini` — check there, not from memory.
3. **Mark planned vs. implemented.** If a feature is planned but not yet in the firmware (e.g., replication in Phase 5), clearly state this with a callout block.
4. **CLI syntax must be exact.** Copy command syntax from the source code (e.g., the error strings in `handleRoomCommand` or `handlePeerCommand`).
5. **Default values must match the code.** The default admin password, stealth setting, advert interval, etc. must match what is in `MyMesh.cpp`.

---

## Commit Convention

Documentation commits should follow the same convention as code commits:

```
docs(<scope>): <short description>

Example:
docs(cli-reference): add room stealth command
docs(operations): document radio settings persistence gotcha
docs(glossary): add NUS definition
```

---

## Callout Blocks for Status

Use blockquote callouts to mark incomplete or planned content:

```markdown
> **Status**: This feature (Phase 5 replication) is not yet implemented.
> The design is documented here; verify against actual firmware when implementation lands.
```

```markdown
> **Warning**: This action is irreversible. Take a backup first.
```

---

## What Triggers a Doc Update

Any issue closed with these labels or phases requires a doc update:

| Issue type | Doc files to update |
|---|---|
| New CLI command | `cli-reference.md` |
| New web UI page or setting | `wifi-webui.md` |
| Radio or LoRa settings change | `getting-started.md`, `operations.md` |
| New room feature (stealth, QR, etc.) | `architecture.md`, `cli-reference.md` |
| Replication protocol change | `replication-protocol.md` |
| New web client feature | `web-clients.md` |
| New hardware constraint | `hardware.md`, `operations.md` |
| New glossary term | `glossary.md` |
| Build system / partition change | `getting-started.md`, `operations.md` |
| MeshCore upgrade | `meshcore-upgrade-runbook.md`, `operations.md` |

---

## Changelog

Track significant documentation updates here to help reviewers understand what changed.

| Date | Author | Change |
|---|---|---|
| 2026-07-31 | DocuMaster (JES-785) | Initial documentation suite created: all 11 content areas, docs/README.md index, CONTRIBUTING-DOCS.md |
| 2026-07-31 | CEO (JES-784) | Board directive codified: every docs change must be pushed to GitHub immediately after committing (applies to all agents) |

---

## Review Process

Documentation PRs should be reviewed by the CTO (c722b899) before merging to ensure technical accuracy. The DocuMaster (7de8fbc2) owns the documentation content and style; the CTO owns technical correctness sign-off.

For time-sensitive updates, a single reviewer (CTO or senior developer) is sufficient.

---

## Questions?

Open an issue in the SIREN GitHub repository or post on the relevant Paperclip issue thread.
