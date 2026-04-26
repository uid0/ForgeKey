#include <Arduino.h>
#include <WiFi.h>
#include "camera/camera_manager.h"
#include "detection/person_detector.h"
#include "counting/occupancy_counter.h"
#include "mqtt/mqtt_client.h"

// ============= CONFIGURATION =============
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* MQTT_BROKER = "mqtt.yourdomain.com";
const int MQTT_PORT = 1883;
const char* MQTT_JWT = "YOUR_JWT_TOKEN";

const int DETECTION_INTERVAL = 2000;  // ms between detections
const int MQTT_PUBLISH_INTERVAL = 10000;  // ms between MQTT updates
// =========================================

unsigned long lastDetection = 0;
unsigned long lastMqttPublish = 0;
bool occupancyChanged = false;

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\nForgeKey People Counter Starting...");
    
    // Connect to WiFi
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected");
    Serial.println("IP: " + WiFi.localIP().toString());
    
    // Get MAC address for MQTT topic
    String macAddress = WiFi.macAddress();
    macAddress.replace(":", "");
    macAddress.toLowerCase();
    
    // Initialize camera
    if (!cameraManager.begin()) {
        Serial.println("Camera init failed!");
        return;
    }
    
    // Initialize person detector
    if (!personDetector.begin()) {
        Serial.println("Person detector init failed!");
        return;
    }
    
    // Initialize occupancy counter
    occupancyCounter.begin(0);
    
    // Initialize MQTT
    mqttClient.begin(MQTT_BROKER, MQTT_PORT, MQTT_JWT);
    mqttClient.setTopicPrefix(macAddress.c_str());
    
    Serial.println("Setup complete. Starting detection loop...");
}

void loop() {
    unsigned long now = millis();
    
    // Run detection at specified interval
    if (now - lastDetection >= DETECTION_INTERVAL) {
        lastDetection = now;
        
        // Capture frame
        camera_fb_t* fb = cameraManager.capture();
        if (!fb) {
            Serial.println("Camera capture failed");
            return;
        }
        
        // Run person detection
        DetectionResult result = personDetector.detect(fb->buf, fb->width, fb->height);
        
        // Update occupancy counter
        occupancyCounter.updateCount(result.count);
        
        // Log detection
        Serial.printf("Detected: %d person(s), Confidence: %.2f\n", 
                     result.count, result.confidence);
        
        // Free frame buffer
        cameraManager.returnFrame(fb);
        
        // Check if occupancy changed
        OccupancyData data = occupancyCounter.getData();
        if (data.changed) {
            occupancyChanged = true;
            Serial.printf("Occupancy changed to: %d\n", data.currentCount);
        }
    }
    
    // Publish to MQTT (on change or at interval)
    if (mqttClient.isConnected() && 
        (occupancyChanged || now - lastMqttPublish >= MQTT_PUBLISH_INTERVAL)) {
        int count = occupancyCounter.getCurrentCount();
        if (mqttClient.publishOccupancy(count)) {
            lastMqttPublish = now;
            occupancyChanged = false;
        }
    }
    
    // MQTT loop
    mqttClient.loop();
    
    // Small delay to prevent watchdog issues
    delay(10);
}
