#ifndef CAMERA_CONFIG_H
#define CAMERA_CONFIG_H

#include "esp_camera.h"

// XIAO ESP32-S3 Sense OV3660 pin configuration
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39

#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15

#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM    13

#define XCLK_FREQ_HZ     20000000

// Camera settings
// PIXFORMAT_JPEG: OV3660 native JPEG encoding — best quality, smallest file.
// FRAMESIZE_SXGA: 1280x1024 — highest resolution JPEG mode on OV3660.
//
// The OV3660 supports only one pixel format at a time. Person detection
// (person_detector) needs raw RGB565 data, so the camera is temporarily
// switched to RGB565 + QVGA for detection via cameraManager.captureRgb565().
// After detection the camera switches back to JPEG + SXGA for photo uploads.
#define FRAME_SIZE FRAMESIZE_SXGA  // 1280x1024
#define PIXEL_FORMAT PIXFORMAT_JPEG

#define CAMERA_MODEL_XIAO_ESP32S3 true

#endif
