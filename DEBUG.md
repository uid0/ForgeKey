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

## Triage decision tree

Walk these questions top-down. The first one that fails tells you where to look.

### 1. Is the device on WiFi at all?

Look for `[INFO] WIFI:` lines:

```
[INFO] WIFI: SSID: <ssid>
[INFO] WIFI: IP: <addr>
[INFO] WIFI: Gateway: <addr>
[INFO] WIFI: DNS: <addr> / <addr>
```

| Symptom | Cause |
|---------|-------|
| No `WiFi connected` line; only `WIFI: Connecting to WiFi...` then `Portal failed` | Device couldn't auto-connect and the captive portal also timed out. Re-flash near a known WiFi or wait for a person to provision. |
| `IP: 0.0.0.0` or `Gateway: 0.0.0.0` | DHCP failed or partial — the device joined an AP but didn't get a lease. Suspect captive AP misconfig or an isolation segment. |
| WiFi up but no `register:` and no `MQTT begin:` later | DNS likely failed silently — confirm `DNS:` is non-zero and that `dms.oms-iot.com` resolves from the same subnet. |

### 2. Did registration succeed?

Look for `register: HTTP <code>`. The device only registers on first boot
(or after `provisioning.clear()`). On subsequent boots you'll instead see
`provisioning: loaded from NVS` lines and skip directly to MQTT.

| Symptom | Cause |
|---------|-------|
| `register: TLS connect to <host>:443 failed` | Either DNS, network egress, or the OMS CA pin is stale. Confirm `Gateway:`/`DNS:` and that the device can reach the host. |
| `register: WARNING using placeholder provisioning token` then `HTTP 401` | Build was missing `-DFORGEKEY_PROVISIONING_TOKEN=...`. Rebuild with the staff-handed token. |
| `register: HTTP 401` body mentions `invalid_token` | Token rotated server-side. A `config:` rotation message hasn't been delivered yet, or NVS rotation slot is stale. Re-flash with the current token, or wait for OMS to push a rotation. |
| `register: HTTP 200` but `parsed jwt_token len=0 (present=0)` | OMS-side regression — registration accepted but didn't issue a JWT. Escalate; firmware can't recover without a JWT. |
| `register: NVS persist failed` | NVS partition full/corrupt. Erase flash and re-flash. |

### 3. Did the JWT survive into MQTT?

Compare the `register: NVS persisted jwt_token len=N prefix=XXXXXXXX` line
against the `[MQTT] begin: ... jwt_len=N jwt_prefix=XXXXXXXX` line on the
*same* boot (or against `provisioning: loaded from NVS jwt_token len=N`
on a later boot). They must match byte-for-byte (length and 8-char prefix).

| Symptom | Cause |
|---------|-------|
| `[MQTT] begin: ... jwt_len=0` | Provisioning didn't run or was cleared mid-boot. Look for an earlier `provisioning.clear()` trigger (auth-expired photo upload, malformed pings topic). |
| `jwt_len` differs between `register:` and `[MQTT] begin:` | Persist roundtrip wrote something different from what the server sent. NVS bug — file an issue with both lines. |
| `prefix` differs between two consecutive boots' `loaded from NVS` lines | Something cleared and re-registered between boots. Look upstream in the previous boot's log for `provisioning: clear()` callers. |

### 4. Did the MQTT CONNECT succeed?

Look for `[MQTT] connected: state=0 (CONNECTED)` or `[MQTT] connect FAILED`.

| `rc=` | Meaning | Most likely cause |
|-------|---------|-------------------|
| `-4 CONNECTION_TIMEOUT` | TCP open succeeded but no CONNACK in time | Broker overloaded, or wrong protocol version on the wire (TLS port spoken as plain). |
| `-3 CONNECTION_LOST` | Was connected, dropped | Network blip; firmware will auto-reconnect within 5s. |
| `-2 CONNECT_FAILED` | TCP connect refused | Broker host/port wrong, or firewall. Check `[MQTT] begin: broker=...` and try `nc -zv <host> 1883` from a peer. |
| `1 CONNECT_BAD_PROTOCOL` | Server doesn't speak our MQTT version | EMQX listener misconfigured (e.g. WebSocket-only on this port). |
| `2 CONNECT_BAD_CLIENT_ID` | Server rejected our client id | Rare. EMQX ACL might forbid the `ForgeKey_*` pattern. |
| `3 CONNECT_UNAVAILABLE` | Broker can't accept | EMQX is degraded — escalate. |
| `4 CONNECT_BAD_CREDENTIALS` | JWT rejected before ACL eval | EMQX JWT authenticator (JWKS) is missing or misconfigured. **This was the symptom that motivated this logging build.** Confirm `JWT authenticator already present id=jwt` on the EMQX side. |
| `5 CONNECT_UNAUTHORIZED` | JWT accepted but ACL denied | The JWT's `client_id`/`username` claim doesn't match the broker's ACL. Cross-check the JWT subject against EMQX's ACL rules. |

### 5. Did SUBSCRIBE go out?

```
[MQTT] subscribe firmware topic=forgekey/<mac>/people_counter/firmware ok=1 ...
[MQTT] subscribe config topic=forgekey/<mac>/config ok=1 ...
```

`ok=1` only means we wrote the SUBSCRIBE packet to the socket — PubSubClient
does NOT surface SUBACK. If commands (OTA dispatch, `forget_wifi`) never
arrive even though `ok=1`:

- ACL on the broker is denying SUBSCRIBE on this topic for this JWT.
- The OMS publisher is sending to a different topic shape (compare topic
  string in the firmware's subscribe log against the OMS publish log).
- `[MQTT] subscribe ... SKIPPED: topic_len=0` means the firmware never
  resolved a topic — a registration-response field was empty.

### 6. Are publishes actually going out?

Each successful publish prints:

```
[MQTT] publish OK: topic=<topic> qos=0 payload_len=<n> payload={...}
```

QoS 0 publishes are fire-and-forget — `ok=1` does NOT mean the broker
received it, only that we wrote bytes to the socket. To confirm broker
delivery, subscribe from a peer (`mosquitto_sub -h dms.oms-iot.com -t
'forgekey/+/+/occupancy'`) and verify messages arrive.

| Symptom | Cause |
|---------|-------|
| `publish FAILED: ... state=-3 (CONNECTION_LOST)` | Socket closed between connect and publish. The next loop iteration will reconnect. If it loops forever, look at the connect rc. |
| `publish FAILED: ... payload_len > 1024` | Payload exceeds `setBufferSize(1024)`. Bump the buffer or shorten the payload. |
| `MQTT disconnected: last_state=4 since_last_publish_ms=<n>` | Broker actively kicked us — almost always credential/ACL related. Look up the `<n>`: if it's small (~tens of ms), the broker dropped us at CONNECT; if it's large (minutes), the JWT expired mid-session. |
| `MQTT disconnected: ... since_last_publish_ms=never` | Connected at least once but never published before being dropped. Look at SUBSCRIBE outcomes — broker may be cycling us due to ACL deny on a SUBSCRIBE. |

### 7. Is the detection pipeline producing counts to publish?

(People-counter build only.)

```
[INFO] CAM: Frame: 320x240 format=4 bytes=15234
[INFO] DET: Detected: 1 person(s), Confidence: 0.83, Motion: yes, Inference: 145000 us, TFLite: yes
[INFO] CNT: Stabilization: raw=1 stable_prev=0 stable_now=0 changed=no
```

| Symptom | Cause |
|---------|-------|
| `CAM: Frame:` lines missing entirely | Camera init failed (look for `Camera init failed with error 0x..`) or the build is the temperature variant (no camera). |
| `bytes=0` or `bytes < 1000` | Camera returned a degenerate frame — sensor flake or PSRAM exhaustion. |
| `TFLite: no(fallback)` always | `TFLite init failed` — see PERSON_DETECTOR init log. The fallback motion detector still runs but can't classify. |
| `Inference:` consistently `> 1000000 us` (1s) | Arena too small or running on internal RAM; check person_detector init log for arena placement. |
| `Stabilization: raw=N stable_prev=N stable_now=N changed=no` for many cycles, but `publish OK count=0` repeatedly | Stabilizer's mode filter has converged on 0 (last 5 frames mostly 0). Move in front of the camera and watch `raw=` change before `stable_now=` follows — confirms filter is working as designed. |

## Privacy / secrets contract

Every log line in this build is shoulder-surfable. The contract is:

- JWTs and provisioning tokens are logged by **length + first 8 chars** only.
- The HTTP register response body is logged with the JWT field redacted in-place.
- WiFi passwords are never logged (WiFiManager owns them and we don't print).
- MAC address, IP, device id, topic strings, and detection counts are NOT
  considered secret and are logged in full.

If you need to log a new credential-bearing field, follow the same pattern:
length + 8-char prefix only. Do not extend logging to print full secrets even
in error paths.
