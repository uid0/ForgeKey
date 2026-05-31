# ForgeKey Fleet Management TODO

This is the working backlog for turning ForgeKey into a complete ESP32 fleet
management experience. It is intentionally organized into commit-sized phases
so we can build, review, and test one layer at a time before moving on.

## Phase 1 — Contracts and observability foundations

- [ ] Define the versioned device-twin contract for desired and reported state.
- [ ] Define the versioned health telemetry contract shared by all device classes.
- [ ] Create JSON Schema files for command envelopes, command acknowledgements,
      health reports, desired state, reported state, OTA status, and capability
      telemetry.
- [ ] Add `schema_version` and `command_id` requirements to every command and
      acknowledgement payload.
- [ ] Document topic ownership, QoS expectations, retained-message behavior, and
      offline buffering expectations for each MQTT topic.
- [ ] Add build metadata fields to the contracts: firmware version, build ID,
      git SHA, target environment, hardware target, release channel, and signing
      key ID.

## Phase 2 — Security and provisioning hardening

- [ ] Replace shared provisioning-token enrollment with per-device bootstrap
      identity, claim codes, and QR-labelled install flow.
- [ ] Require signed, replay-protected command envelopes for every operator and
      device-control command, not just safety-sensitive lock commands.
- [ ] Add command expiry, nonce or command-id replay detection, actor identity,
      and negative acknowledgements for failed authorization.
- [ ] Document and implement production secure boot, flash encryption, encrypted
      NVS, JTAG/UART policy, key rotation, and device-retirement procedures.
- [ ] Add compromised-device response workflows for revoking MQTT credentials,
      provisioning identities, command keys, and firmware signing keys.

## Phase 3 — Reliable fleet control plane

- [ ] Add reliable command acknowledgement behavior with retry/backoff for
      critical acks and OTA status events.
- [ ] Add an outbound queue for important telemetry while MQTT is disconnected.
- [ ] Publish retained birth/death state consistently across Arduino and ESP-IDF
      firmware targets.
- [ ] Add dynamic log-level controls, remote diagnostics commands, self-tests,
      and crash or reset-reason reporting.
- [ ] Add watchdog and subsystem self-healing policies for WiFi, MQTT, camera,
      BLE, lock state machine, OTA, and sensors.

## Phase 4 — OTA completion and rollout management

- [ ] Support canary, cohort, percentage, mandatory, minimum-version, and
      deadline-based OTA policies.
- [ ] Add anti-rollback and compatibility checks where the hardware supports it.
- [ ] Include OTA slot, pending verification state, previous version, and last
      OTA error in reported state and health telemetry.
- [ ] Bring the ePaper display target into the remote OTA system.
- [ ] Align Arduino and ESP-IDF OTA payload schemas and status events.

## Phase 5 — Hardware manifests, wiring, and simulation

- [ ] Add board manifests for XIAO ESP32-S3, XIAO ESP32-C3 ePaper, and ESP32-C6
      lock builds.
- [ ] Add per-device pin ownership, boot-strap constraints, pull-up/down
      expectations, bus mappings, ADC availability, and unsafe pins.
- [ ] Add boot-time pin conflict validation before capabilities are activated.
- [ ] Add wiring diagrams, harness notes, BOMs, and install photos or SVGs for
      people counter, temperature sensor, cabinet lock, ePaper display, and BLE
      equipment modules.
- [ ] Add Wokwi simulations where supported; for unsupported hardware, add
      generated wiring diagrams and explicit simulation-limit notes.

## Phase 6 — Device-class feature completion

- [ ] Finish lockout, clear-lockout, commissioning, tamper, manual-key, and
      emergency workflows for the ESP32-C6 cabinet lock.
- [ ] Add people-counter calibration, privacy masking, threshold management,
      model metadata, and redacted calibration-frame capture.
- [ ] Add BLE privacy controls, hashed identifiers, allow/deny lists, scan-duty
      policy, RSSI calibration, and retention guidance.
- [ ] Add OMS UI/API plumbing around the completed ePaper firmware lifecycle:
      desired-state publishing, command status visibility, and health dashboards.
- [ ] Normalize temperature and environmental sensor schemas for future sensor
      variants.

## Phase 7 — Verification ladder

- [ ] Add host-side unit tests for JSON parsing, command validation, OTA payload
      validation, schema examples, NVS config migration, and telemetry builders.
- [ ] Add simulator smoke tests for supported device variants.
- [ ] Add hardware-in-the-loop smoke tests for WiFi provisioning, MQTT, OTA,
      DHT21, camera, ePaper, lock sensors, and solenoid actuation.
- [ ] Add CI jobs for PlatformIO builds, ESP-IDF lock builds, schema validation,
      documentation link checks, and formatting checks.
- [ ] Replace placeholder testing commands in project instructions with the real
      command set once the test ladder exists.

## Working rule for each phase

Each phase should end with:

1. Documentation updated.
2. Firmware or tooling changes committed.
3. At least one lightweight automated check run locally.
4. A manual test plan written down for the next hardware session.
5. A small pull request that can be reviewed before continuing.
