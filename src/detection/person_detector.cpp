#include "person_detector.h"
#include "Arduino.h"

PersonDetector personDetector;

bool PersonDetector::begin() {
    if (initialized) return true;
    
    initialized = true;
    Serial.println("Person detector initialized (motion detection mode)");
    return true;
}

void PersonDetector::preprocessImage(const uint8_t* src, int srcWidth, int srcHeight, 
                                     uint8_t* dst, int dstWidth, int dstHeight) {
    // Simple nearest-neighbor resize
    for (int y = 0; y < dstHeight; y++) {
        for (int x = 0; x < dstWidth; x++) {
            int srcX = (x * srcWidth) / dstWidth;
            int srcY = (y * srcHeight) / dstHeight;
            dst[y * dstWidth + x] = src[srcY * srcWidth + srcX];
        }
    }
}

int PersonDetector::detectMotion(const uint8_t* current, const uint8_t* previous,
                                 int width, int height, int threshold) {
    if (!previous) return 0;
    
    int diffPixels = 0;
    int totalPixels = width * height;
    
    for (int i = 0; i < totalPixels; i++) {
        int diff = abs((int)current[i] - (int)previous[i]);
        if (diff > threshold) {
            diffPixels++;
        }
    }
    
    // Calculate percentage of changed pixels
    float changePercent = (float)diffPixels / totalPixels * 100.0;
    
    // If more than 5% of pixels changed, consider it motion
    return (changePercent > 5.0) ? 1 : 0;
}

DetectionResult PersonDetector::detect(const uint8_t* image, int width, int height) {
    DetectionResult result = {0, 0.0, false};
    
    if (!initialized || !image) return result;
    
    // Resize to smaller image for processing
    const int procWidth = 80;
    const int procHeight = 60;
    uint8_t* processed = new uint8_t[procWidth * procHeight];
    preprocessImage(image, width, height, processed, procWidth, procHeight);
    
    // Detect motion
    int motion = detectMotion(processed, previousFrame, procWidth, procHeight);
    
    // Update previous frame
    if (!previousFrame) {
        previousFrame = new uint8_t[procWidth * procHeight];
    }
    memcpy(previousFrame, processed, procWidth * procHeight);
    frameWidth = procWidth;
    frameHeight = procHeight;
    
    delete[] processed;
    
    result.motionDetected = motion > 0;
    result.count = motion;  // 1 if motion detected, 0 otherwise
    result.confidence = motion ? 0.8 : 0.0;
    
    return result;
}

void PersonDetector::end() {
    if (previousFrame) {
        delete[] previousFrame;
        previousFrame = nullptr;
    }
    initialized = false;
}
