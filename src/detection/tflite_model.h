#ifndef TFLITE_MODEL_H
#define TFLITE_MODEL_H

// Person detection model for ESP32-S3
// This is a placeholder - replace with actual model from:
// https://github.com/espressif/esp-tflite-micro/tree/main/examples/person_detection
//
// To generate: xxd -i person_detect.tflite > tflite_model.h

// Minimal model data (replace with actual model)
// Model input: 96x96 grayscale image (9216 bytes)
// Model output: [person_score, not_person_score]

extern const unsigned char g_person_detect_model_data[];
extern const unsigned int g_person_detect_model_data_len;

#endif
