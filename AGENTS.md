# ForgeKey AGENTS Instructions

## Documentation References
- `FORGEKEY_DEVICE.md`: Describes the device lifecycle, OTA apply flow, NVS layout, and security model.
- `PEOPLE_COUNTER.md`: Details specifics for the people-counter variant of ForgeKey devices.
- `docs/OTA_DEPLOYMENT.md`: Walks through the process of shipping new firmware, including signing-key setup, build, upload via OMS, and troubleshooting.

## Development Network Configuration
To skip the captive portal during development:
1. Create a `secrets_local.h` file by copying `src/wifi_setup/secrets_local.example.h` to `src/wifi_setup/secrets_local.h`.
2. Edit `secrets_local.h` and uncomment/fill in the network credentials for `FORGEKEY_NETWORK_1_*` and `FORGEKEY_NETWORK_2_*`.

## Build System
To run MQTT component locally during development or testing, use the following command:
```bash
pio run -t build
```

### Build Verification Before Feature Complete
A pre-commit hook at `.git/hooks/pre-commit` runs `pio run` automatically when firmware source files (`.cpp`, `.h`, `platformio.ini`, etc.) are staged. This prevents committing code that fails to build.

To manually verify all environments before committing:
```bash
pio run
```

This builds both `seeed_xiao_esp32s3` and `seeed_xiao_esp32s3_temperature` targets.

To run Celery task runner locally during development or testing, use the following command:
```bash
# TODO: Replace with actual command to start Celery task runner
```

## Testing Framework
To run tests locally during development or testing, ensure the following services are running: Postgresql, Redis, and MQTT. Then use the following command:
```bash
# TODO: Replace with actual test command
```