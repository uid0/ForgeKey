// People-counter capability: wraps the existing camera + person-detector +
// occupancy-counter + photo-uploader pipeline behind the capability
// framework. The detect() probe is "did the camera init succeed", which
// covers boards without an attached OV3660 (or with PSRAM trouble) - on
// those, no people-counter capability registers and the firmware happily
// runs the rest of its capabilities.

#ifndef FORGEKEY_DISABLE_PEOPLE_COUNTER

#include "../capability.h"
#include "people_counter.h"

#include "../../camera/camera_manager.h"
#include "../../detection/person_detector.h"
#include "../../counting/occupancy_counter.h"
#include "../../photo_upload/uploader.h"
#include "../../mqtt/mqtt_client.h"
#include "../../ota/ota_updater.h"
#include "../../provisioning/register.h"
#include "../../provisioning/device_config.h"

namespace PeopleCounter {

bool detectFn();
void setupFn();
void tickFn();

namespace {

constexpr unsigned long DETECTION_INTERVAL_MS = 2000;
constexpr unsigned long MQTT_PUBLISH_INTERVAL_MS = 10000;

bool g_active = false;
bool g_setupDone = false;
unsigned long g_lastDetection = 0;
unsigned long g_lastMqttPublish = 0;
bool g_occupancyChanged = false;

}  // namespace

bool isActive() { return g_active; }

bool captureProvisioningPhoto(uint8_t** outBuf, size_t* outLen) {
    if (!g_active || !outBuf || !outLen) return false;
    return cameraManager.captureJpeg(outBuf, outLen);
}

bool detectFn() {
    // cameraManager.begin() probes I2C, configures pins, and starts the
    // sensor. If the camera isn't physically present (or PSRAM is bad), it
    // returns false and we skip the rest of the pipeline.
    bool ok = cameraManager.begin();
    if (ok) {
        g_active = true;
    }
    return ok;
}

void setupFn() {
    if (!personDetector.begin()) {
        Serial.println("[CAP/people_counter] WARN: person detector init failed; "
                       "tick() will run motion-only fallback");
    }
    occupancyCounter.begin(0);
    g_setupDone = true;
}

void tickFn() {
    if (!g_setupDone) return;
    unsigned long now = millis();

    if (now - g_lastDetection >= DETECTION_INTERVAL_MS) {
        g_lastDetection = now;

        camera_fb_t* fb = cameraManager.capture();
        if (!fb) {
            Serial.println("[CAP/people_counter] camera capture failed");
        } else {
            Serial.printf("[CAP/people_counter] frame %ux%u format=%d bytes=%u\n",
                          (unsigned)fb->width, (unsigned)fb->height,
                          (int)fb->format, (unsigned)fb->len);

            DetectionResult result = personDetector.detect(fb->buf, fb->width, fb->height);
            int prevStable = occupancyCounter.getCurrentCount();
            occupancyCounter.updateCount(result.count);
            int newStable = occupancyCounter.getCurrentCount();

            Serial.printf("[CAP/people_counter] detected=%d conf=%.2f motion=%s "
                          "inf_us=%lu tflite=%s\n",
                          result.count, result.confidence,
                          result.motionDetected ? "yes" : "no",
                          (unsigned long)result.inferenceMicros,
                          result.tfliteUsed ? "yes" : "no(fallback)");
            Serial.printf("[CAP/people_counter] stabilization raw=%d prev=%d now=%d changed=%s\n",
                          result.count, prevStable, newStable,
                          (prevStable != newStable) ? "yes" : "no");

            if (result.motionDetected || result.count > 0) {
                photoUploader.markMotion(now);
            }
            cameraManager.returnFrame(fb);

            OccupancyData data = occupancyCounter.getData();
            if (data.changed) {
                g_occupancyChanged = true;
                Serial.printf("[CAP/people_counter] occupancy changed -> %d\n",
                              data.currentCount);
            }
        }
    }

    bool mqttConnected = mqttClient.isConnected();
    if (mqttConnected &&
        (g_occupancyChanged ||
         now - g_lastMqttPublish >= MQTT_PUBLISH_INTERVAL_MS)) {
        int count = occupancyCounter.getCurrentCount();
        if (mqttClient.publishOccupancy(count)) {
            g_lastMqttPublish = now;
            g_occupancyChanged = false;
            Serial.printf("[CAP/people_counter] published occupancy=%d\n", count);
            // First successful publish post-OTA = green light to bless the
            // partition.
            otaUpdater.markStableIfPending();
        } else {
            Serial.println("[CAP/people_counter] failed to publish occupancy");
        }
    }

    if (provisioning.isProvisioned() && !otaUpdater.inProgress() &&
        photoUploader.shouldUpload(now, PHOTO_UPLOAD_INTERVAL_MS,
                                   PHOTO_UPLOAD_MOTION_WINDOW_MS)) {
        uint8_t* jpeg = nullptr;
        size_t jpegLen = 0;
        if (cameraManager.captureJpeg(&jpeg, &jpegLen)) {
            Serial.printf("[CAP/people_counter] uploading photo (%u bytes)\n",
                          (unsigned)jpegLen);
            PhotoUploader::Result r = photoUploader.uploadPhoto(jpeg, jpegLen, now);
            free(jpeg);
            if (r == PhotoUploader::Result::AuthExpired) {
                Serial.println("[CAP/people_counter] auth expired; clearing creds for re-register");
                provisioning.clear();
            } else if (r == PhotoUploader::Result::Ok) {
                Serial.println("[CAP/people_counter] photo upload OK");
            } else {
                Serial.println("[CAP/people_counter] photo upload failed");
            }
        }
    }
}

}  // namespace PeopleCounter

REGISTER_CAPABILITY(people_counter, "people_counter",
                    PeopleCounter::detectFn,
                    PeopleCounter::setupFn,
                    PeopleCounter::tickFn,
                    "occupancy")

#endif  // FORGEKEY_DISABLE_PEOPLE_COUNTER
