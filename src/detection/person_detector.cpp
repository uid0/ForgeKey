#include "person_detector.h"
#include "tflite_model.h"
#include "Arduino.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>

#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

PersonDetector personDetector;

namespace {
constexpr int kModelCols = 96;
constexpr int kModelRows = 96;
constexpr int kModelChannels = 1;
constexpr int kModelInputSize = kModelCols * kModelRows * kModelChannels;
constexpr int kPersonIndex = 1;
constexpr int kNotPersonIndex = 0;

// Arena sizing follows the Espressif person_detection example for ESP32-S3.
// 81 KB base + 39 KB scratch headroom for the optimized kernels.
constexpr int kTensorArenaSize = (81 + 39) * 1024;
}  // namespace

bool PersonDetector::beginTflite() {
    static tflite::MicroErrorReporter staticErrorReporter;
    errorReporter = &staticErrorReporter;

    const tflite::Model* model = tflite::GetModel(g_person_detect_model_data);
    if (model->version() != TFLITE_SCHEMA_VERSION) {
        Serial.printf("TFLite: model schema %lu != supported %d\n",
                      (unsigned long)model->version(), TFLITE_SCHEMA_VERSION);
        return false;
    }

    // Put the arena and the int8 input scratch in PSRAM. Internal DRAM is
    // contended on the S3 (WiFi/MQTT/camera buffers); PSRAM has 8 MB free.
    tensorArena = (uint8_t*)heap_caps_malloc(
        kTensorArenaSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tensorArena) {
        Serial.printf("TFLite: failed to allocate %d-byte arena in PSRAM\n",
                      kTensorArenaSize);
        return false;
    }

    inputBuffer = (int8_t*)heap_caps_malloc(
        kModelInputSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!inputBuffer) {
        Serial.println("TFLite: failed to allocate int8 input scratch in PSRAM");
        heap_caps_free(tensorArena);
        tensorArena = nullptr;
        return false;
    }

    // The op resolver must list every op in person_detect.tflite.
    static tflite::MicroMutableOpResolver<5> opResolver;
    opResolver.AddAveragePool2D();
    opResolver.AddConv2D();
    opResolver.AddDepthwiseConv2D();
    opResolver.AddReshape();
    opResolver.AddSoftmax();

    static tflite::MicroInterpreter staticInterpreter(
        model, opResolver, tensorArena, kTensorArenaSize, errorReporter);
    interpreter = &staticInterpreter;

    if (interpreter->AllocateTensors() != kTfLiteOk) {
        Serial.println("TFLite: AllocateTensors() failed");
        return false;
    }

    inputTensor = interpreter->input(0);
    if (inputTensor->dims->size != 4 ||
        inputTensor->dims->data[1] != kModelRows ||
        inputTensor->dims->data[2] != kModelCols ||
        inputTensor->dims->data[3] != kModelChannels ||
        inputTensor->type != kTfLiteInt8) {
        Serial.printf("TFLite: unexpected input shape (rank=%d type=%d)\n",
                      inputTensor->dims->size, inputTensor->type);
        return false;
    }

    Serial.printf("TFLite: ready (model=%dB, arena=%dKB)\n",
                  g_person_detect_model_data_len, kTensorArenaSize / 1024);
    return true;
}

bool PersonDetector::begin() {
    if (initialized) return true;

    tfliteReady = beginTflite();
    if (!tfliteReady) {
        Serial.println("Person detector: TFLite init failed, "
                       "falling back to motion detection");
    } else {
        Serial.println("Person detector initialized (TFLite person detection)");
    }

    initialized = true;
    return true;
}

void PersonDetector::preprocessImage(const uint8_t* src, int srcWidth, int srcHeight,
                                     uint8_t* dst, int dstWidth, int dstHeight) {
    // Nearest-neighbor resize. The model is robust enough to a coarse resample,
    // and this avoids float math on the per-frame hot path.
    for (int y = 0; y < dstHeight; y++) {
        int srcY = (y * srcHeight) / dstHeight;
        const uint8_t* srcRow = &src[srcY * srcWidth];
        uint8_t* dstRow = &dst[y * dstWidth];
        for (int x = 0; x < dstWidth; x++) {
            int srcX = (x * srcWidth) / dstWidth;
            dstRow[x] = srcRow[srcX];
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

    float changePercent = (float)diffPixels / totalPixels * 100.0f;
    return (changePercent > 5.0f) ? 1 : 0;
}

DetectionResult PersonDetector::detect(const uint8_t* image, int width, int height) {
    DetectionResult result = {0, 0.0f, false, false, 0};
    if (!initialized || !image) return result;

    if (!tfliteReady) {
        return detectFallback(image, width, height);
    }

    // Resize directly into the int8 input buffer, then shift uint8 -> int8.
    // We reuse a uint8 staging buffer on the model grid to keep preprocess
    // logic shared with the fallback path.
    static uint8_t staging[kModelInputSize];
    preprocessImage(image, width, height, staging, kModelCols, kModelRows);
    for (int i = 0; i < kModelInputSize; i++) {
        inputBuffer[i] = (int8_t)((int)staging[i] - 128);
    }
    memcpy(inputTensor->data.int8, inputBuffer, kModelInputSize);

    // Cheap motion pre-filter: if nothing changed since the last frame, the
    // previous classification is still valid and we skip ~hundreds of ms of
    // inference. Motion buffer lives at model resolution.
    bool motion = false;
    if (previousFrame) {
        motion = detectMotion(staging, previousFrame,
                              kModelCols, kModelRows) > 0;
    } else {
        previousFrame = (uint8_t*)heap_caps_malloc(
            kModelInputSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        motion = true;  // first frame, force inference
    }
    if (previousFrame) {
        memcpy(previousFrame, staging, kModelInputSize);
    }
    frameWidth = kModelCols;
    frameHeight = kModelRows;

    int64_t t0 = esp_timer_get_time();
    if (interpreter->Invoke() != kTfLiteOk) {
        Serial.println("TFLite: Invoke() failed");
        return detectFallback(image, width, height);
    }
    int64_t t1 = esp_timer_get_time();

    TfLiteTensor* output = interpreter->output(0);
    int8_t personRaw = output->data.int8[kPersonIndex];
    int8_t notPersonRaw = output->data.int8[kNotPersonIndex];
    float personScore = (personRaw - output->params.zero_point) * output->params.scale;
    float notPersonScore = (notPersonRaw - output->params.zero_point) * output->params.scale;

    result.tfliteUsed = true;
    result.motionDetected = motion;
    result.inferenceMicros = (uint32_t)(t1 - t0);
    result.confidence = personScore;
    result.count = (personScore > personThreshold &&
                    personScore > notPersonScore) ? 1 : 0;

    Serial.printf("TFLite: person=%.2f no_person=%.2f count=%d (%lu us)\n",
                  personScore, notPersonScore, result.count,
                  (unsigned long)result.inferenceMicros);
    return result;
}

DetectionResult PersonDetector::detectFallback(const uint8_t* image, int width, int height) {
    DetectionResult result = {0, 0.0f, false, false, 0};

    const int procWidth = 80;
    const int procHeight = 60;
    uint8_t* processed = new uint8_t[procWidth * procHeight];
    preprocessImage(image, width, height, processed, procWidth, procHeight);

    int motion = 0;
    if (previousFrame && frameWidth == procWidth && frameHeight == procHeight) {
        motion = detectMotion(processed, previousFrame, procWidth, procHeight);
    }

    if (!previousFrame || frameWidth != procWidth || frameHeight != procHeight) {
        if (previousFrame) {
            heap_caps_free(previousFrame);
        }
        previousFrame = (uint8_t*)heap_caps_malloc(
            procWidth * procHeight, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (previousFrame) {
        memcpy(previousFrame, processed, procWidth * procHeight);
    }
    frameWidth = procWidth;
    frameHeight = procHeight;

    delete[] processed;

    result.motionDetected = motion > 0;
    result.count = motion;
    result.confidence = motion ? 0.8f : 0.0f;
    return result;
}

void PersonDetector::end() {
    if (previousFrame) {
        heap_caps_free(previousFrame);
        previousFrame = nullptr;
    }
    if (inputBuffer) {
        heap_caps_free(inputBuffer);
        inputBuffer = nullptr;
    }
    if (tensorArena) {
        heap_caps_free(tensorArena);
        tensorArena = nullptr;
    }
    interpreter = nullptr;
    inputTensor = nullptr;
    initialized = false;
    tfliteReady = false;
}
