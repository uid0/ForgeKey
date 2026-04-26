#ifndef PERSON_DETECTOR_H
#define PERSON_DETECTOR_H

#include "Arduino.h"

// Forward declarations to avoid pulling TFLite headers into every includer.
namespace tflite {
class MicroInterpreter;
class ErrorReporter;
}
struct TfLiteTensor;

struct DetectionResult {
    int count;
    float confidence;
    bool motionDetected;
    bool tfliteUsed;
    uint32_t inferenceMicros;
};

class PersonDetector {
public:
    bool begin();
    DetectionResult detect(const uint8_t* image, int width, int height);
    void end();

    // Person score above this threshold => person present.
    void setPersonThreshold(float t) { personThreshold = t; }

private:
    bool initialized = false;
    bool tfliteReady = false;

    // TFLite Micro state
    tflite::ErrorReporter* errorReporter = nullptr;
    tflite::MicroInterpreter* interpreter = nullptr;
    TfLiteTensor* inputTensor = nullptr;
    uint8_t* tensorArena = nullptr;
    int8_t* inputBuffer = nullptr;  // 96*96 int8 scratch (PSRAM)

    // Motion-detection fallback state
    uint8_t* previousFrame = nullptr;
    int frameWidth = 0;
    int frameHeight = 0;

    float personThreshold = 0.6f;

    bool beginTflite();
    int detectMotion(const uint8_t* current, const uint8_t* previous,
                     int width, int height, int threshold = 25);
    void preprocessImage(const uint8_t* src, int srcWidth, int srcHeight,
                         uint8_t* dst, int dstWidth, int dstHeight);
    DetectionResult detectFallback(const uint8_t* image, int width, int height);
};

extern PersonDetector personDetector;

#endif
