// People-counter capability: wraps the existing camera + person-detector +
// occupancy-counter + photo-uploader pipeline behind the capability
// framework. The detect() probe is "did the camera init succeed", which
// covers boards without an attached OV3660 (or with PSRAM trouble) - on
// those, no people-counter capability registers and the firmware happily
// runs the rest of its capabilities.
//
// TFLite person-detection inference takes ~4.5s synchronously and used to
// run on the main Arduino loop. While it ran, mqttClient.loop() and any
// in-flight TLS POST were starved long enough for the WiFi MAC to drop
// ACK windows (MISSING_ACKS) and for the photo upload TLS handshake to
// abort (errno 113). To fix that (fo-t7f) inference now runs on its own
// FreeRTOS task pinned to Core 1; the main loop polls for results without
// blocking, and photo upload is gated by a quiet window after each
// inference so the WiFi stack has time to recover.

#ifndef FORGEKEY_DISABLE_PEOPLE_COUNTER

#include "../capability.h"
#include "people_counter.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

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

// Minimum time that must elapse after an inference finishes before we
// will start a photo upload. TLS handshake + multi-KB POST on shared
// WiFi needs the radio to be unstarved; without this gate the upload
// fails with errno 113 (MISSING_ACKS).
constexpr unsigned long PHOTO_QUIET_WINDOW_MS = 1500;

// Pin the inference task to Core 1 (same core as the Arduino loopTask)
// so the WiFi/lwIP tasks that live on Core 0 are not contended. Stack
// size accommodates the TFLite Micro interpreter call chain plus our
// preprocessing scratch.
constexpr UBaseType_t INFERENCE_TASK_PRIORITY = 1;
constexpr BaseType_t  INFERENCE_TASK_CORE     = 1;
constexpr uint32_t    INFERENCE_TASK_STACK    = 16 * 1024;

bool g_active = false;
bool g_setupDone = false;
unsigned long g_lastDetectionRequest = 0;
unsigned long g_lastMqttPublish = 0;
bool g_occupancyChanged = false;
bool g_oneShotCaptureRequested = false;

TaskHandle_t      g_inferenceTask    = nullptr;
SemaphoreHandle_t g_inferenceTrigger = nullptr;
portMUX_TYPE      g_stateMux         = portMUX_INITIALIZER_UNLOCKED;

// Cross-core state. Read/written under g_stateMux only.
bool             g_inferenceBusy        = false;
bool             g_resultReady          = false;
DetectionResult  g_pendingResult        = {0, 0.0f, false, false, 0};
unsigned long    g_lastInferenceDoneMs  = 0;

void inferenceTaskFn(void*) {
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
    Serial.printf("[CAP/people_counter] inference task started on core %d, watermark %u\n",
                  (int)xPortGetCoreID(), (unsigned)hwm);
    for (;;) {
        // Block until tickFn signals work is due.
        xSemaphoreTake(g_inferenceTrigger, portMAX_DELAY);

        DetectionResult result = {0, 0.0f, false, false, 0};
        camera_fb_t* fb = cameraManager.capture();
        if (!fb) {
            Serial.println("[CAP/people_counter] camera capture failed");
        } else {
            Serial.printf("[CAP/people_counter] frame %ux%u format=%d bytes=%u\n",
                          (unsigned)fb->width, (unsigned)fb->height,
                          (int)fb->format, (unsigned)fb->len);
            result = personDetector.detect(fb->buf, fb->width, fb->height);
            cameraManager.returnFrame(fb);
            Serial.printf("[CAP/people_counter] detected=%d conf=%.2f motion=%s "
                          "inf_us=%lu tflite=%s\n",
                          result.count, result.confidence,
                          result.motionDetected ? "yes" : "no",
                          (unsigned long)result.inferenceMicros,
                          result.tfliteUsed ? "yes" : "no(fallback)");
        }

        unsigned long doneMs = millis();
        portENTER_CRITICAL(&g_stateMux);
        g_pendingResult       = result;
        g_resultReady         = true;
        g_inferenceBusy       = false;
        g_lastInferenceDoneMs = doneMs;
        portEXIT_CRITICAL(&g_stateMux);

        // Periodic stack-watermark report so a slow leak in the TFLite call
        // chain is diagnosable from serial without reflashing.
        UBaseType_t loopHwm = uxTaskGetStackHighWaterMark(NULL);
        Serial.printf("[CAP/people_counter] inference task watermark=%u\n",
                      (unsigned)loopHwm);

        // Yield so the IDLE task on Core 1 can clear and so our priority-1
        // peer (Arduino loopTask) gets a clean shot at the CPU even if the
        // scheduler tick boundary just elapsed.
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

}  // namespace

bool isActive() { return g_active; }

bool captureProvisioningPhoto(uint8_t** outBuf, size_t* outLen) {
    if (!g_active || !outBuf || !outLen) return false;
    return cameraManager.captureJpeg(outBuf, outLen);
}

bool requestOneShotCapture() {
    if (!g_active) return false;
    g_oneShotCaptureRequested = true;
    return true;
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

    g_inferenceTrigger = xSemaphoreCreateBinary();
    if (!g_inferenceTrigger) {
        Serial.println("[CAP/people_counter] FATAL: failed to create inference semaphore");
        return;
    }
    BaseType_t taskOk = xTaskCreatePinnedToCore(
        inferenceTaskFn, "infer",
        INFERENCE_TASK_STACK, NULL,
        INFERENCE_TASK_PRIORITY, &g_inferenceTask,
        INFERENCE_TASK_CORE);
    if (taskOk != pdPASS) {
        Serial.println("[CAP/people_counter] FATAL: failed to spawn inference task");
        vSemaphoreDelete(g_inferenceTrigger);
        g_inferenceTrigger = nullptr;
        return;
    }

    g_setupDone = true;
}

void tickFn() {
    if (!g_setupDone) return;
    unsigned long now = millis();

    // Snapshot inference-task state without holding the lock across any
    // expensive call.
    bool             inferenceBusy;
    bool             resultReady;
    DetectionResult  result;
    unsigned long    lastInferenceDoneMs;
    portENTER_CRITICAL(&g_stateMux);
    inferenceBusy       = g_inferenceBusy;
    resultReady         = g_resultReady;
    result              = g_pendingResult;
    lastInferenceDoneMs = g_lastInferenceDoneMs;
    if (resultReady) {
        g_resultReady = false;
    }
    portEXIT_CRITICAL(&g_stateMux);

    if (resultReady) {
        int prevStable = occupancyCounter.getCurrentCount();
        occupancyCounter.updateCount(result.count);
        int newStable = occupancyCounter.getCurrentCount();
        Serial.printf("[CAP/people_counter] stabilization raw=%d prev=%d now=%d changed=%s\n",
                      result.count, prevStable, newStable,
                      (prevStable != newStable) ? "yes" : "no");
        if (result.motionDetected || result.count > 0) {
            photoUploader.markMotion(now);
        }
        OccupancyData data = occupancyCounter.getData();
        if (data.changed) {
            g_occupancyChanged = true;
            Serial.printf("[CAP/people_counter] occupancy changed -> %d\n",
                          data.currentCount);
        }
    }

    // Trigger the next detection cycle once the cadence has elapsed AND
    // the worker is idle. Skipping when busy means a slow inference just
    // back-pressures the cadence rather than queueing up requests.
    if (!inferenceBusy && now - g_lastDetectionRequest >= DETECTION_INTERVAL_MS) {
        g_lastDetectionRequest = now;
        portENTER_CRITICAL(&g_stateMux);
        g_inferenceBusy = true;
        portEXIT_CRITICAL(&g_stateMux);
        xSemaphoreGive(g_inferenceTrigger);
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

    // Photo upload: only when the inference task is idle AND has been idle
    // long enough for the WiFi MAC to recover. Without this gate, TLS
    // handshake aborts mid-flight with errno 113 because the radio is still
    // catching up on the burst of PSRAM/CPU pressure inference creates.
    bool networkQuietEnough =
        !inferenceBusy &&
        (lastInferenceDoneMs == 0 || now - lastInferenceDoneMs > PHOTO_QUIET_WINDOW_MS);

    if (provisioning.isProvisioned() && !otaUpdater.inProgress() && networkQuietEnough &&
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

    // Operator-triggered one-shot capture. Independent of shouldUpload()'s
    // motion/interval gates — the operator wants the photo now. Still gated
    // by provisioning, OTA, and the post-inference quiet window so we don't
    // tear down a TLS session mid-handshake.
    if (g_oneShotCaptureRequested && provisioning.isProvisioned() &&
        !otaUpdater.inProgress() && networkQuietEnough) {
        g_oneShotCaptureRequested = false;
        uint8_t* jpeg = nullptr;
        size_t jpegLen = 0;
        const char* uploadStatus = "failed";
        const char* reason = "capture_failed";
        if (cameraManager.captureJpeg(&jpeg, &jpegLen)) {
            Serial.printf("[CAP/people_counter] one-shot capture: uploading %u bytes\n",
                          (unsigned)jpegLen);
            PhotoUploader::Result r = photoUploader.uploadPhoto(jpeg, jpegLen, now);
            free(jpeg);
            switch (r) {
                case PhotoUploader::Result::Ok:
                    uploadStatus = "ok";
                    reason = "";
                    break;
                case PhotoUploader::Result::AuthExpired:
                    uploadStatus = "failed";
                    reason = "auth_expired";
                    provisioning.clear();
                    break;
                case PhotoUploader::Result::ServerError:
                    uploadStatus = "failed";
                    reason = "server_error";
                    break;
                case PhotoUploader::Result::TransportError:
                    uploadStatus = "failed";
                    reason = "transport_error";
                    break;
                case PhotoUploader::Result::BadResponse:
                    uploadStatus = "failed";
                    reason = "bad_response";
                    break;
                case PhotoUploader::Result::Skipped:
                    uploadStatus = "failed";
                    reason = "skipped";
                    break;
            }
        }
        char buf[128];
        if (reason && *reason) {
            snprintf(buf, sizeof(buf),
                     "{\"cmd_ack\":\"capture\",\"upload_status\":\"%s\",\"reason\":\"%s\"}",
                     uploadStatus, reason);
        } else {
            snprintf(buf, sizeof(buf),
                     "{\"cmd_ack\":\"capture\",\"upload_status\":\"%s\"}",
                     uploadStatus);
        }
        mqttClient.publishStatus(buf);
        Serial.printf("[CAP/people_counter] one-shot capture result: %s\n", buf);
    }
}

}  // namespace PeopleCounter

REGISTER_CAPABILITY(people_counter, "people_counter",
                    PeopleCounter::detectFn,
                    PeopleCounter::setupFn,
                    PeopleCounter::tickFn,
                    "occupancy")

#endif  // FORGEKEY_DISABLE_PEOPLE_COUNTER
