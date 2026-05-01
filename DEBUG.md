# Debugging the People Counter / MQTT publish path

Verbose diagnostic logging is **on by default** in this branch. Every key
transition on the boot → register → MQTT-connect → publish path emits a
labelled serial line so you can identify which step is silently failing.

## Flash and capture

```bash
~/.platformio/penv/bin/platformio run --target upload
~/.platformio/penv/bin/platformio device monitor -b 115200 | tee boot.log
```

Power-cycle the device after the monitor is attached so you capture the
full `setup()` trace. If you only need the post-boot state, the publish
lines will tell you the resolved topic regardless.

## Expected serial output (happy path)

A successful boot + register + first-publish cycle looks roughly like
this (timestamps and identifiers will differ):

```
[  1003] [INFO] MAIN: ForgeKey People Counter Starting...
[  1004] [INFO] MAIN: Firmware version: 0.1.0
[  1010] [INFO] WIFI: WiFi connected
[  1011] [INFO] WIFI: IP: 192.168.1.42
[  1014] [INFO] MAIN: MAC Address: aabbcc112233
provisioning: loaded from NVS device_id='dev_4321' (len=8)
provisioning: loaded from NVS mqtt_topic_for_pings='forgekey/aabbcc112233/people_counter/occupancy' (len=51)
provisioning: loaded from NVS mqtt_topic_for_firmware='forgekey/aabbcc112233/people_counter/firmware' (len=49)
provisioning: loaded from NVS jwt_token len=412
[INFO] PROV: Already provisioned
[MQTT] begin: broker=dms.oms-iot.com:1883 jwt_len=412 jwt_prefix=eyJhbGci tls=plain
[MQTT] connect: broker=dms.oms-iot.com:1883 client_id=ForgeKey_a1b2c3d4 username=forgemqtt jwt_len=412
[MQTT] connected: state=0 (CONNECTED)
[MQTT] setTopicPrefix: prefix=forgekey/aabbcc112233/people_counter default_occupancy=forgekey/aabbcc112233/people_counter/occupancy
[MQTT] setOccupancyTopic: override 'forgekey/aabbcc112233/people_counter/occupancy' -> 'forgekey/aabbcc112233/people_counter/occupancy'
[MQTT] setFirmwareTopic: '' -> 'forgekey/aabbcc112233/people_counter/firmware'
[MQTT] setConfigTopic: '' -> 'forgekey/aabbcc112233/config'
...
[MQTT] publish OK: topic=forgekey/aabbcc112233/people_counter/occupancy payload={"count":0,"timestamp":12345}
```

If this is a **first boot**, the `loaded from NVS` lines will show empty
strings, and you'll instead see the registration trace:

```
[INFO] PROV: First boot — registering with OMS
register: POST https://oms.example.com:443/api/forgekey/devices/register/ (...)
register: HTTP 200 (HTTP/1.1 200 OK) body_len=512
register: response body: {"device_id":"dev_4321","mqtt_topic_for_pings":"forgekey/aabbcc112233/people_counter/occupancy",...,"jwt_token":"eyJhbGci...REDACTED(len=412)"}
register: parsed device_id='dev_4321'
register: parsed mqtt_topic_for_pings='forgekey/aabbcc112233/people_counter/occupancy' (present=1)
register: parsed mqtt_topic_for_firmware='forgekey/aabbcc112233/people_counter/firmware' (present=1)
register: parsed jwt_token len=412 (present=1)
```

## Symptom → likely cause

| Symptom | Most likely cause | Where to look |
|---------|-------------------|---------------|
| `[MQTT] publishOccupancy FAILED: occupancyTopic is empty` | `setTopicPrefix` was never called, or MAC was empty when called | `[MQTT] setTopicPrefix:` line missing/with empty MAC |
| `[MQTT] publish FAILED: ... state=4 (CONNECT_BAD_CREDENTIALS)` | JWT is wrong, expired, or server side ACL rejecting it | Compare `jwt_len` in `[MQTT] begin:` vs what `register: parsed jwt_token len=` reported. If there's no register line, the device booted with a stale NVS JWT — clear NVS or wait for re-register on auth failure. |
| `[MQTT] publish FAILED: ... state=-2 (CONNECT_FAILED)` | Broker host/port wrong or unreachable | Check `[MQTT] connect: broker=` line; ping the host from another device on the same WiFi |
| `[MQTT] publish OK: topic=/aabbcc112233/people_counter/occupancy ...` (leading slash, no `forgekey/` prefix) | **Old firmware default** still applied — this is the bug fo-mr7 was filed for. The fix on this branch removes the leading slash and prefixes with `forgekey/`. | If you still see this on a built-from-this-branch firmware, the topic came from `setOccupancyTopic` — i.e. NVS or the register response is delivering the wrong shape |
| `register: HTTP 401` and body mentions provisioning token | `FORGEKEY_PROVISIONING_TOKEN` not set at build time, or rotated server-side | `register: WARNING using placeholder provisioning token` would have appeared earlier; rebuild with `-DFORGEKEY_PROVISIONING_TOKEN=...` |
| `provisioning: loaded from NVS mqtt_topic_for_pings='/...'` then `WARN MAIN: Stored mqtt_topic_for_pings='/...' does not match OMS contract; ignoring and clearing creds` | Stored topic is from a pre-fix firmware. The fix clears creds; next boot will re-register and persist a contract-compliant topic. | Reboot once more and confirm subsequent `loaded from NVS` line shows `forgekey/...` |
| Last `_seen` updates on power cycle but **no occupancy data** ever shows in OMS, despite `[MQTT] publish OK` lines locally | Broker is delivering, but OMS subscriber is on a different topic. Compare the topic in the `publish OK` line against the OMS-side subscription (`forgekey/<mac>/people_counter/occupancy`). | This was the original fo-mr7 root cause — leading-slash default. Verify the published topic starts with `forgekey/`, not `/`. |

## Notes

- `last_seen` updates from the MQTT broker connection itself (LWT/keepalive),
  not from publishes. So a device with the wrong publish topic still looks
  "alive" in OMS but never delivers occupancy.
- Every `setTopicPrefix` / `setOccupancyTopic` / `setFirmwareTopic` /
  `setConfigTopic` call logs both the previous and new value, so reordering
  bugs (override before prefix, or vice versa) are visible in the trace.
- The JWT is logged by length and 8-char prefix only. The HTTP register body
  redacts the JWT before printing.
