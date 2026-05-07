#include "camera_manager.h"
#include "camera_config.h"
#include "Arduino.h"
#include "img_converters.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

CameraManager cameraManager;

bool CameraManager::begin() {
    if (initialized) return true;
    
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = XCLK_FREQ_HZ;
    config.frame_size = FRAME_SIZE;
    config.pixel_format = PIXEL_FORMAT;
    config.grab_mode = CAMERA_GRAB_LATEST;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = 4;  // OV3660 native JPEG quality (1=highest quality, 63=lowest)
    config.fb_count = 2;
    
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed with error 0x%x\n", err);
        return false;
    }
    
    initialized = true;
    m_rgb565Mutex = xSemaphoreCreateMutex();
    Serial.println("Camera initialized successfully");
    return true;
}

camera_fb_t* CameraManager::capture() {
    if (!initialized) return nullptr;
    return esp_camera_fb_get();
}

void CameraManager::returnFrame(camera_fb_t* fb) {
    if (fb) esp_camera_fb_return(fb);
}

void CameraManager::end() {
    if (initialized) {
        esp_camera_deinit();
        initialized = false;
    }
}

bool CameraManager::captureJpeg(uint8_t** outBuf, size_t* outLen, int quality) {
    if (!initialized || !outBuf || !outLen) return false;

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("captureJpeg: fb_get failed");
        return false;
    }

    bool ok;
    if (fb->format == PIXFORMAT_JPEG) {
        // Camera is already producing JPEG; copy into a heap buffer the
        // caller can free() after we return the framebuffer.
        *outBuf = (uint8_t*)malloc(fb->len);
        if (!*outBuf) {
            esp_camera_fb_return(fb);
            return false;
        }
        memcpy(*outBuf, fb->buf, fb->len);
        *outLen = fb->len;
        ok = true;
    } else {
        ok = frame2jpg(fb, quality, outBuf, outLen);
    }
    esp_camera_fb_return(fb);
    if (!ok) {
        Serial.println("captureJpeg: encode failed");
    }
    return ok;
}

// OV3660 register addresses
#define OV3660_REG_FORMAT        0x03E
#define OV3660_REG_HSIZE         0x3820
#define OV3660_REG_VSIZE         0x3821

// SXGA (1280x1024) frame size — restored after detection
#define OV3660_SXGA_HSIZE_H      0x05
#define OV3660_SXGA_HSIZE_L      0x00
#define OV3660_SXGA_VSIZE_H      0x04
#define OV3660_SXGA_VSIZE_L      0x00

// QVGA (320x240) frame size — used during detection
#define OV3660_QVGA_HSIZE_H      0x01
#define OV3660_QVGA_HSIZE_L      0x40
#define OV3660_QVGA_VSIZE_H      0x00
#define OV3660_QVGA_VSIZE_L      0xF0

camera_fb_t* CameraManager::captureRgb565() {
    if (!initialized) return nullptr;

    // Wait for mutex to prevent concurrent camera reinit
    if (m_rgb565Mutex) {
        xSemaphoreTake(m_rgb565Mutex, portMAX_DELAY);
    }

    // Capture frame first (camera is still in JPEG mode, but we'll reinit)
    // We need to reinit to RGB565 to get raw data.
    // Save current config
    camera_config_t savedConfig;
    savedConfig.frame_size = FRAME_SIZE;
    savedConfig.pixel_format = PIXEL_FORMAT;
    savedConfig.fb_count = 2;

    // Reinitialize camera to RGB565 + QVGA for detection
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = XCLK_FREQ_HZ;
    config.frame_size = FRAMESIZE_QVGA;
    config.pixel_format = PIXFORMAT_RGB565;
    config.grab_mode = CAMERA_GRAB_LATEST;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = 12;
    config.fb_count = 1;

    // Deinit and reinit with RGB565 config
    esp_camera_deinit();

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.println("captureRgb565: camera reinit failed, restoring original config");
        // Restore original config
        config.frame_size = savedConfig.frame_size;
        config.pixel_format = savedConfig.pixel_format;
        config.fb_count = savedConfig.fb_count;
        err = esp_camera_init(&config);
        if (err == ESP_OK) initialized = true;
        if (m_rgb565Mutex) xSemaphoreGive(m_rgb565Mutex);
        return nullptr;
    }

    // Capture one raw frame
    camera_fb_t* fb = esp_camera_fb_get();

    // Restore original JPEG + SXGA config
    config.frame_size = savedConfig.frame_size;
    config.pixel_format = savedConfig.pixel_format;
    config.fb_count = savedConfig.fb_count;
    esp_camera_deinit();
    err = esp_camera_init(&config);
    if (err == ESP_OK) initialized = true;

    if (m_rgb565Mutex) xSemaphoreGive(m_rgb565Mutex);

    return fb;
}
