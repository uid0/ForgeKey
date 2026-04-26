#ifndef PERSON_DETECTOR_H
#define PERSON_DETECTOR_H

#include "Arduino.h"

struct DetectionResult {
    int count;
    float confidence;
    bool motionDetected;
};

class PersonDetector {
public:
    bool begin();
    DetectionResult detect(const uint8_t* image, int width, int height);
    void end();
    
private:
    bool initialized = false;
    uint8_t* previousFrame = nullptr;
    int frameWidth = 0;
    int frameHeight = 0;
    
    int detectMotion(const uint8_t* current, const uint8_t* previous, 
                     int width, int height, int threshold = 25);
    void preprocessImage(const uint8_t* src, int srcWidth, int srcHeight, 
                       uint8_t* dst, int dstWidth, int dstHeight);
};

extern PersonDetector personDetector;

#endif
