#ifndef TFLITE_MODEL_H
#define TFLITE_MODEL_H

// Espressif's pre-trained 96x96 grayscale person detection model.
// Source: https://github.com/espressif/esp-tflite-micro/tree/master/examples/person_detection
// Generated via: xxd -i person_detect.tflite > tflite_model.h
//
// Model input:  int8[1, 96, 96, 1] (camera grayscale shifted by -128)
// Model output: int8[1, 2]         ([no_person_score, person_score], dequantize via output->params)

extern const unsigned char g_person_detect_model_data[];
extern const int g_person_detect_model_data_len;

#endif
