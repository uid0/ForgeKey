#ifndef CAMERA_MANAGER_H
#define CAMERA_MANAGER_H

#include "esp_camera.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class CameraManager {
public:
    bool begin();
    camera_fb_t* capture();
    void returnFrame(camera_fb_t* fb);
    void end();

    // Capture a frame and return a JPEG-encoded buffer. Caller must free()
    // *outBuf when done. Returns false on failure.
    // When PIXEL_FORMAT is PIXFORMAT_JPEG (OV3660 native), the frame is
    // already JPEG-encoded by the sensor and *quality is ignored.
    bool captureJpeg(uint8_t** outBuf, size_t* outLen, int quality = 70);

    // Capture a single raw RGB565 frame for person detection. Temporarily
    // reinitializes the camera to RGB565 + QVGA, captures one frame, then
    // restores JPEG + SXGA. Caller must return the frame via returnFrame().
    // Thread-safe: uses a mutex to prevent concurrent camera reinit.
    camera_fb_t* captureRgb565();

private:
    bool initialized = false;
    SemaphoreHandle_t m_rgb565Mutex = nullptr;
};

extern CameraManager cameraManager;

#endif
