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