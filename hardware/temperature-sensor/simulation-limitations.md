# Temperature-sensor Simulation Limitations

Wokwi supports DHT22, not a separately named DHT21/AM2301 part. The included
project therefore verifies the same single-wire read pattern using DHT22 as a
stand-in, but it cannot prove DHT21 package mechanics, harness noise margin, or
long-cable timing.

Bench validation is still required for the production DHT21 harness, pull-up
value, enclosure airflow, and calibration expectations.
