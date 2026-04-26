#ifndef CAMERA_MANAGER_H
#define CAMERA_MANAGER_H

#include "esp_camera.h"

class CameraManager {
public:
    bool begin();
    camera_fb_t* capture();
    void returnFrame(camera_fb_t* fb);
    void end();
    
private:
    bool initialized = false;
};

extern CameraManager cameraManager;

#endif
