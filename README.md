# ForgeKey
Open Source Makerspace Device Operating System


Forgekey is an embedded device firmware that works in step with OpenMakerSpace in order to optimize and manage your Makerspace.  ForgeKey has different types of devices that seamlessly integrate into the Makerspace:

* Traffic Counters for areas
* Tool Enablement and Metering 
* Accessory Enablement (For example, always turn on dust collection when a wood shop tool is enabled, regardless of if it is currently powered on or not, and then run it for 3 minutes after the tool is finished being used)
* Status Lights for Restrooms, tools, and other areas

```
+---------------------+     +--------------------+                          
|                     |     |                    |                          
|                     |     |                    |                          
|  OpenMakerSuite     +-+--->  Postgresql        |<--------+                
|                     | |   |                    |         |                
|                     | |   |                    |         |                
+---------------------+ |   +--------------------+         |                
                        |                                  |                
                        |   +--------------------+    +----+---------------+
                        |   |                    |    |                    |
                        |   |                    |    |                    |
                        +--->  Redis             +--->|  Celery            |
                        |   |                    |    |                    |
                        |   |                    |    |                    |
                        |   +--------------------+    +--------------------+
                        |                                                   
+---------------------+ |   +--------------------+                          
|                     | +-->|                    |                          
|                     |     |                    |                          
|   ForgeKey Devices  +---->|   MQTT             |                          
|                     <-----+                    |                          
|                     |     |                    |                          
+---------------------+     +--------------------+                          
```


ForgeKey is based off of the adaptable ESP32 series of chips.

## Documentation

- [FORGEKEY_DEVICE.md](FORGEKEY_DEVICE.md) — device lifecycle, OTA apply flow, NVS layout, security model.
- [PEOPLE_COUNTER.md](PEOPLE_COUNTER.md) — people-counter variant specifics.
- [docs/OTA_DEPLOYMENT.md](docs/OTA_DEPLOYMENT.md) — operator walkthrough for shipping new firmware (signing-key setup, build, upload via OMS, troubleshooting).
- [DEBUG.md](DEBUG.md) — diagnostic logging reference.

## Predefined Dev Networks (optional)

On boot ForgeKey first tries up to two predefined WiFi networks via
`WiFiMulti`. If neither is reachable it falls back to the captive-portal
AP (`ForgeKey-Setup-XXXX`) so end users can still provision normally.

To skip the portal during development:

```bash
cp src/wifi_setup/secrets_local.example.h src/wifi_setup/secrets_local.h
# edit secrets_local.h and uncomment/fill in FORGEKEY_NETWORK_1_* / _2_*
```

`secrets_local.h` is gitignored — never commit real credentials. With no
`secrets_local.h` present, the firmware compiles and runs identically to
a clean checkout (portal-only).  