#ifndef CAMERA_MANAGER_H
#define CAMERA_MANAGER_H

#include "esp_camera.h"

class CameraManager {
public:
    bool begin();
    camera_fb_t* capture();
    void returnFrame(camera_fb_t* fb);
    void end();

    // Capture a frame and JPEG-encode it. Caller must free()
    // *outBuf when done. Returns false on failure.
    bool captureJpeg(uint8_t** outBuf, size_t* outLen, int quality = 70);

private:
    bool initialized = false;
};

extern CameraManager cameraManager;

#endif
