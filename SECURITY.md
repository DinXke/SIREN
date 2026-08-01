# Security Policy — SIREN

SIREN (Shared Incident Radio Emergency Network) is an auxiliary LoRa mesh
communication tool built on [MeshCore](https://github.com/ripplebiz/MeshCore).
It is **not** a primary clinical or life-safety system. This document states the
supported versions, how to report vulnerabilities, the security model, and the
known limitations operators must accept before deployment.

Full technical detail lives in the **Security Addendum** (JES-717) and the
`docs/` security material. This file is the entry point.

## Supported versions

| Component | Supported |
|-----------|-----------|
| `multiroom` branch firmware (`SIREN_v3_room_server`) | Yes — current development line |
| Vendored MeshCore base (`base-room-server-v1.16.0`) | Upstream-supported; SIREN patches on top |
| `dist/` OTA/full-flash binaries | Only the latest published build |

## Reporting a vulnerability

Do **not** open a public GitHub issue for a security vulnerability. Report
privately to the SIREN maintainers / hospital ICT security officer. Include:

- Affected component and firmware version / commit.
- Reproduction steps or a proof-of-concept.
- Observed vs. expected behavior and the impact you believe it has.

Because SIREN nodes operate on an ISM-band radio and may be deployed in a
multi-agency emergency context, treat radio-observable findings as sensitive.

## Security model (summary)

- **Identity & authenticity.** Every room slot is an Ed25519 keypair. Posts are
  authenticated by the MeshCore MAC; the room server verifies the MAC before
  accepting a post. Impersonation requires possession of a valid private key.
- **Confidentiality.** Direct messages (login, CLI-over-mesh, join flow) are
  X25519-encrypted and authenticated. Room posts require login. LoRa metadata
  (timing, activity) is observable to any SDR in range.
- **Authorization.** Per-room ACL with roles (admin / read-write / read-only /
  guest). Destructive CLI commands (`room del`, `peer add/del`) are enforced
  **serial-only** in firmware.
- **Replication.** Anti-entropy sync uses version vectors; the receive parsers
  are length-validated and bounded.

## Known limitations — operators MUST accept these

1. **No patient identifiers or sensitive operational data** over this network.
   Contents are encrypted but metadata is observable; brief all users.
2. **Default admin password.** Stock `dist/` binaries currently build with a
   well-known default admin password. **Change the admin password on every node
   before deployment.** (Tracked for remediation — see Security Addendum SEC-001.)
3. **Keys at rest are unencrypted.** ESP32 NVS is not encrypted by default;
   private keys and passwords are cleartext at rest. Physical node compromise
   requires rotating all room/admin passwords and re-enrolling. Enable NVS/flash
   encryption for production (Security Addendum SEC-005).
4. **Availability is not guaranteed.** RF jamming or duty-cycle DoS can deny
   service and cannot be mitigated in firmware. Keep a fallback voice-radio
   channel and record SIREN in the business-continuity plan.

## NIS2

Deploying operators in the health sector are likely **essential entities** under
NIS2 (Annex I). A formal criticality-tier assessment and documented risk
acceptance, signed by the CISO/security officer, must exist **before**
multi-agency or production deployment. See the Security Addendum §4.

## Hardening checklist before production

- [ ] Change the admin password and all room passwords from defaults.
- [ ] Enable ESP32-S3 NVS/flash encryption (eFuse key).
- [ ] Wire `cppcheck` + `clang-tidy` into CI (Security Addendum SEC-008).
- [ ] Complete the NIS2 risk assessment / risk acceptance.
- [ ] Document a physical-compromise credential-rotation runbook.
