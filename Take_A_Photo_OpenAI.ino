/*
* Take_A_Photo.ino
* This sketch captures images from an ESP32S3 Eye camera module and displays them on a TFT screen.
* It also allows taking photos by pressing a button and saving them to an SD card with description from OpenAI
* 
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <TFT_eSPI.h>
#include "esp_camera.h"
#include "driver_sdmmc.h"

#include "mbedtls/base64.h"
#include "Audio.h"

#define CAMERA_MODEL_ESP32S3_EYE  // Has PSRAM
#include "camera_pins.h"

#ifdef FNK0102A_1P14_135x240_ST7789
  int screenWidth = 135;
  int screenHeight = 240;
#elif defined FNK0102B_3P5_320x480_ST7796
  int screenWidth = 320;
  int screenHeight = 480;
#endif

#define BUTTON_PIN 19  // Please do not modify it.
#define SD_MMC_CMD 38  // Please do not modify it.
#define SD_MMC_CLK 39  // Please do not modify it.
#define SD_MMC_D0 40   // Please do not modify it.
#define TFT_BL 20

#define AUDIO_OUTPUT_BCLK   42  // Bit clock pin for I2S
#define AUDIO_OUTPUT_LRC    41  // Left/Right clock pin for I2S
#define AUDIO_OUTPUT_DOUT   1   // Data out pin for I2S

// Global variable to track if we're using 3.5 inch screen
bool is35InchScreen = false;

TFT_eSPI tft = TFT_eSPI();
Audio audio;

// -----------------------------
// Wi-Fi & OpenAI configuration
// -----------------------------
const char* WIFI_SSID      = "ssid";
const char* WIFI_PASS      = "pass";
const char* OPENAI_API_KEY = "sk-API-KEY";
// Endpoint (chat.completions with image_url)
static const char* OPENAI_HOST  = "api.openai.com";
static const int   OPENAI_PORT  = 443;
static const char* OPENAI_PATH  = "/v1/chat/completions";
static const char* OPENAI_MODEL = "gpt-4o-mini";

// Timeouts
static const uint32_t CONNECT_TIMEOUT_MS = 20000;
static const uint32_t READ_TIMEOUT_MS    = 35000;
// -----------------------------
// Capture / memory parameters
// -----------------------------
// Keep frames small so base64 + JSON fits in RAM (even with PSRAM).
// QVGA (320x240) JPEG typically ~20–40 KB, base64 grows it by ~33%.
//static const framesize_t CAPTURE_SIZE = FRAMESIZE_QVGA;
//static const int JPEG_QUALITY = 15;   // larger = smaller file
//static const int FB_COUNT     = 1;

void camera_init(int state);
void cameraShow(void);
void cameraPhoto(void);
void tft_rst(void);

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Check if we're using 3.5 inch screen
#ifdef FNK0102B_3P5_320x480_ST7796
  is35InchScreen = true;
#endif
  // init audio - to elimate buzzing sound
  audio.setPinout(AUDIO_OUTPUT_BCLK, AUDIO_OUTPUT_LRC, AUDIO_OUTPUT_DOUT);
  audio.setVolume(0);  // 0...21

  tft_rst();
  tft.init();
  if(is35InchScreen)
    tft.setRotation(1);            // Set the rotation of the TFT display
  else
    tft.setRotation(0);// Set the rotation of the TFT display
  camera_init(0);
  sdmmc_init(SD_MMC_CLK, SD_MMC_CMD, SD_MMC_D0);
  remove_dir("/video");
  create_dir("/video");

  if (WiFi.status() != WL_CONNECTED) 
    connectWiFi();
}

void loop() {
  cameraShow();   // Continuously display the camera feed on the TFT screen
  cameraPhoto();  // Check for button press to take a photo
}

void tft_rst(void) {
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);
  delay(50);
  digitalWrite(TFT_BL, HIGH);
  delay(50);
}

void camera_init(int state) {
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
  config.xclk_freq_hz = 10000000;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 15;
  config.fb_count = 1;
  if (state == 0) {
    if (is35InchScreen) {
      Serial.printf("HVGA");
      config.frame_size = FRAMESIZE_HVGA;
    } else {
      Serial.println("240x240");
      config.frame_size = FRAMESIZE_240X240;
      //Serial.println("128x128");
      //config.frame_size = FRAMESIZE_128X128;
    }
    config.pixel_format = PIXFORMAT_RGB565;
  } else {
    Serial.println("VGA");
    config.frame_size = FRAMESIZE_VGA;
    config.pixel_format = PIXFORMAT_JPEG;
  }
  // Deinitialize and reinitialize the camera with the new configuration
  esp_camera_deinit();
  esp_camera_return_all();
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.println("Camera initialization failed, error code");
    return;
  }
  sensor_t *s = esp_camera_sensor_get();
  // The initial sensor may be vertically flipped and have high color saturation
  s->set_hmirror(s, 0);     // Mirror the image horizontally
  s->set_vflip(s, 1);       // Restore vertical orientation
  s->set_brightness(s, 1);  // Slightly increase brightness
  s->set_saturation(s, 0);  // Reduce saturation
}

void cameraShow(void) {
  // Capture a frame from the camera
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    return;
  }

  // For 3.5 inch screen, display directly without cropping
  if (is35InchScreen) {
    // Direct display when dimensions match
    tft.startWrite();
    tft.pushImage(0, 0, fb->width, fb->height, (uint16_t*)fb->buf);
    tft.endWrite();
  } 
  else {
    // For 1.14 inch screen, use original cropping logic
    int camWidth = fb->width;
    int camHeight = fb->height;

    // Calculate cropping area
    int cropWidth = screenWidth;
    int cropHeight = screenHeight;
    int cropStartX = (camWidth - cropWidth) / 2;
    int cropStartY = (camHeight - cropHeight) / 2;

    // Check if cropping is needed
    if (camWidth > screenWidth || camHeight > screenHeight) {
      // Allocate memory for cropped image
      uint16_t *croppedBuffer = (uint16_t *)malloc(cropWidth * cropHeight * sizeof(uint16_t));
      if (!croppedBuffer) {
        Serial.println("Failed to allocate memory for cropped image");
        esp_camera_fb_return(fb);
        return;
      }

      // Crop the image
      for (int y = 0; y < cropHeight; y++) {
        for (int x = 0; x < cropWidth; x++) {
          croppedBuffer[y * cropWidth + x] = ((uint16_t *)fb->buf)[(cropStartY + y) * camWidth + (cropStartX + x)];
        }
      }

      // Display cropped image on the TFT screen
      tft.startWrite();
      tft.pushImage(0, 0, cropWidth, cropHeight, croppedBuffer);
      tft.endWrite();

      // Free the cropped image buffer
      free(croppedBuffer);
    } else {
      // If camera size is less than or equal to screen size, display the image directly
      tft.startWrite();
      tft.pushImage(0, 0, camWidth, camHeight, fb->buf);
      tft.endWrite();
    }
  }

  // Return the frame buffer to the driver for reuse
  esp_camera_fb_return(fb);
}

void cameraPhoto(void) {
  static int fileCounter = 0;
  int analogValue = analogRead(BUTTON_PIN);
  if (analogValue < 100) {
    camera_init(1);  // Reinitialize camera for photo capture
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      return;
    }
    char filename[32];
    snprintf(filename, sizeof(filename), "/video/photo_%04d.jpg", fileCounter);
    write_jpg(filename, fb->buf, fb->len);  // Save the photo to the SD card

    String description;
    bool ok = describePhotoWithOpenAI(fb->buf, fb->len, description);
    
    
    
    if (ok && description.length() > 0) {
      Serial.print("AI description: ");
      Serial.println(description);
      snprintf(filename, sizeof(filename), "/video/desc_%04d.txt", fileCounter);
      const uint8_t* buf = reinterpret_cast<const uint8_t*>(description.c_str());
      write_file(filename, buf, description.length());
      // Optional: draw on TFT (very small text suggested)
      tft.setRotation(1);
      tft.setCursor(0, 0);
      tft.setTextColor(TFT_WHITE,TFT_BLACK);
      tft.println(description);
      tft.setRotation(0);

      //delay(5000);

    } else {
      Serial.println("OpenAI request failed or empty description.");
    }

    fileCounter++;
    camera_init(0);         // Reinitialize camera for live view
    list_dir("/video", 0);  // List the contents of the /video directory
    while (analogRead(BUTTON_PIN) > 3000);  // Wait for button 
    while (analogRead(BUTTON_PIN) < 3000);  // Wait for button release
  }
}

// ---------- SMALL UTILS ----------
static inline int capInt(size_t len, size_t cap) { return (int)(len < cap ? len : cap); }

void printHeap(const char* tag) {
  Serial.printf("[%s] heap:%u  psram:%u  freeSketch:%u\n",
                tag, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
                (unsigned)ESP.getFreeSketchSpace());
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("Connecting WiFi SSID:%s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < CONNECT_TIMEOUT_MS) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi OK. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connect FAILED.");
  }
}

// Base64 into PSRAM (returns String)
String base64EncodePSRAM(const uint8_t* data, size_t len) {
  if (!data || len == 0) return String();
  size_t outLen = 0;
  mbedtls_base64_encode(NULL, 0, &outLen, data, len);

  uint8_t* outBuf = (uint8_t*) ps_malloc(outLen + 1);
  if (!outBuf) {
    Serial.println("ps_malloc failed for base64 buffer");
    return String();
  }
  size_t written = 0;
  int rc = mbedtls_base64_encode(outBuf, outLen, &written, data, len);
  if (rc != 0) {
    Serial.printf("base64 encode error: %d\n", rc);
    free(outBuf);
    return String();
  }
  outBuf[written] = '\0';
  String s = String((char*)outBuf);
  free(outBuf);
  return s;
}

static String unescapeJsonText(String s) {
  s.replace("\\n", " ");
  s.replace("\\\"", "\"");
  s.replace("\\/", "/");
  return s;
}

// Robustly extract assistant text from chat.completions JSON.
// Handles both:
//   a) choices[0].message.content as a STRING
//   b) choices[0].message.content as an ARRAY with {type:"text", text:"..."}
//   c) legacy "choices":[{"text":"..."}]
// Ignores whitespace around ':' and logs which path was used.
bool parseOpenAIMessageText(const String& body, String& outText) {
  outText = "";

  // Error?
  int errIdx = body.indexOf("\"error\"");
  if (errIdx >= 0) {
    Serial.println("[parser] JSON has an 'error' object:");
    Serial.println(body);
    return false;
  }

  auto skipWS = [&](int i) -> int {
    while (i < (int)body.length() && isspace((unsigned char)body[i])) i++;
    return i;
  };

  auto parseJsonStringFrom = [&](int i, String& dst) -> bool {
    // 'i' points at the opening double-quote
    if (i >= (int)body.length() || body[i] != '\"') return false;
    int start = i + 1;
    int j = start;
    while (j < (int)body.length()) {
      char c = body[j];
      if (c == '\"' && body[j - 1] != '\\') break;
      j++;
    }
    if (j >= (int)body.length()) return false;
    dst = body.substring(start, j);
    dst = unescapeJsonText(dst);
    dst.trim();
    return dst.length() > 0;
  };

  // ---------- Try the modern "choices[0].message.content" path ----------
  // Find "message" then "content" (more precise), but fall back to global "content".
  int msgIdx = body.indexOf("\"message\"");
  int contentIdx = (msgIdx >= 0) ? body.indexOf("\"content\"", msgIdx) : body.indexOf("\"content\"");
  if (contentIdx >= 0) {
    int colon = body.indexOf(':', contentIdx);
    if (colon >= 0) {
      int val = skipWS(colon + 1);
      if (val < (int)body.length()) {
        if (body[val] == '\"') {
          Serial.println("[parser] Found content as STRING.");
          String s;
          if (parseJsonStringFrom(val, s)) { outText = s; return true; }
        } else if (body[val] == '[') {
          Serial.println("[parser] Found content as ARRAY; looking for first {type:\"text\", text:\"...\"}.");
          // Find first "text":"..." inside the content array
          int txtKey = body.indexOf("\"text\"", val);
          if (txtKey >= 0) {
            int c2 = body.indexOf(':', txtKey);
            if (c2 >= 0) {
              int val2 = skipWS(c2 + 1);
              if (val2 < (int)body.length() && body[val2] == '\"') {
                String s;
                if (parseJsonStringFrom(val2, s)) { outText = s; return true; }
              }
            }
          }
        } else {
          Serial.printf("[parser] content value not string/array. First char: %c\n", body[val]);
        }
      }
    }
  }

  // ---------- Legacy "choices":[{"text":"..."}] fallback ----------
  int choicesIdx = body.indexOf("\"choices\"");
  if (choicesIdx >= 0) {
    int textIdx = body.indexOf("\"text\"", choicesIdx);
    if (textIdx >= 0) {
      int colon2 = body.indexOf(':', textIdx);
      if (colon2 >= 0) {
        int val = skipWS(colon2 + 1);
        if (val < (int)body.length() && body[val] == '\"') {
          Serial.println("[parser] Using legacy choices[].text branch.");
          String s;
          if (parseJsonStringFrom(val, s)) { outText = s; return true; }
        }
      }
    }
  }

  Serial.println("[parser] No assistant text found in JSON.");
  return false;
}

// ---------- HTTP helpers ----------
int extractHttpParts(const String& resp, String& headersOut, String& bodyOut) {
  int splitIdx = resp.indexOf("\r\n\r\n");
  if (splitIdx < 0) { headersOut = resp; bodyOut = ""; return -1; }
  headersOut = resp.substring(0, splitIdx);
  bodyOut    = resp.substring(splitIdx + 4);

  // status line
  int lineEnd = headersOut.indexOf("\r\n");
  String statusLine = (lineEnd > 0) ? headersOut.substring(0, lineEnd) : headersOut;
  int sp1 = statusLine.indexOf(' ');
  int sp2 = (sp1 >= 0) ? statusLine.indexOf(' ', sp1 + 1) : -1;
  int code = -1;
  if (sp1 >= 0 && sp2 > sp1) code = statusLine.substring(sp1 + 1, sp2).toInt();
  return code;
}

// Stream body in chunks
bool sendInChunks(WiFiClientSecure& client, const String& s) {
  const size_t total = s.length();
  const size_t CHUNK = 1024;
  const char* p = s.c_str();
  size_t sent = 0;
  while (sent < total) {
    size_t n = (total - sent > CHUNK) ? CHUNK : (total - sent);
    size_t w = client.write((const uint8_t*)(p + sent), n);
    if (w != n) {
      Serial.printf("sendInChunks: short write (%u/%u) at offset %u\n",
                    (unsigned)w, (unsigned)n, (unsigned)sent);
      return false;
    }
    sent += n;
    // small yield
    delay(1);
  }
  return true;
}

// ---------- OpenAI call ----------
bool describePhotoWithOpenAI(const uint8_t* jpg, size_t jpgLen, String& outDescription) {
  if (!jpg || jpgLen == 0) return false;

  printHeap("before b64");
  Serial.printf("JPEG size: %u bytes\n", (unsigned)jpgLen);

  String b64 = base64EncodePSRAM(jpg, jpgLen);
  if (!b64.length()) {
    Serial.println("Base64 conversion failed.");
    return false;
  }
  Serial.printf("Base64 length: %u chars\n", (unsigned)b64.length());
  printHeap("after b64");

  // Build JSON body (as String so we can set Content-Length)
  String body;
  body.reserve(1024 + b64.length());
  body  = F("{\"model\":\"");
  body += OPENAI_MODEL;
  body += F("\",\"temperature\":0,\"max_tokens\":64,\"messages\":[{\"role\":\"user\",\"content\":[");
  body += F("{\"type\":\"text\",\"text\":\"Describe this image in one concise sentence.\"},");
  body += F("{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,");
  body += b64;
  body += F("\"}}]}]}");

  Serial.printf("JSON body length (bytes): %u\n", (unsigned)body.length());

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(READ_TIMEOUT_MS / 1000); // seconds for Stream timeout

  Serial.println("Connecting TLS to api.openai.com ...");
  if (!client.connect(OPENAI_HOST, OPENAI_PORT)) {
    Serial.println("TLS connect FAILED");
    return false;
  }
  Serial.println("TLS connect OK");

  // Headers
  String header;
  header.reserve(512);
  header  = F("POST ");
  header += OPENAI_PATH;
  header += F(" HTTP/1.1\r\nHost: ");
  header += OPENAI_HOST;
  header += F("\r\nUser-Agent: ESP32S3-Freenove/1.1\r\nAccept: application/json\r\nAuthorization: Bearer ");
  header += OPENAI_API_KEY;
  header += F("\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ");
  header += String(body.length());
  header += F("\r\n\r\n");

  Serial.println(">> HTTP request headers (trim):");
  Serial.println(header.substring(0, capInt(header.length(), 200)));
  Serial.println(">> Sending body in chunks...");

  // Send headers, then body in chunks
  if (client.print(header) != (int)header.length()) {
    Serial.println("Header write SHORT/FAIL");
    return false;
  }
  if (!sendInChunks(client, body)) {
    Serial.println("Body write FAILED");
    return false;
  }
  Serial.println("Body write OK. Waiting for response...");

  // Read response
  String resp;
  unsigned long t0 = millis();
  while ((millis() - t0) < READ_TIMEOUT_MS) {
    while (client.available()) {
      resp += (char)client.read();
      t0 = millis(); // extend on activity
    }
    if (!client.connected() && !client.available()) break; // server closed
    delay(5);
  }

  Serial.println("<< HTTP response (first 600 chars):");
  if (resp.length() > 600) Serial.println(resp.substring(0, 600));
  else Serial.println(resp);

  String headers, bodyOnly;
  int status = extractHttpParts(resp, headers, bodyOnly);
  Serial.printf("HTTP status: %d\n", status);
  Serial.println("-- Headers (first 400 chars) --");
  Serial.println(headers.substring(0, capInt(headers.length(), 400)));
  Serial.println("-- Body preview (first 800 chars) --");
  Serial.println(bodyOnly.substring(0, capInt(bodyOnly.length(), 800)));
  Serial.printf("Body length: %u\n", (unsigned)bodyOnly.length());

  if (status != 200) {
    Serial.println("Non-200 status from OpenAI.");
    return false;
  }

  String text;
  if (!parseOpenAIMessageText(bodyOnly, text) || !text.length()) {
    Serial.println("Could not extract assistant text.");
    return false;
  }
  outDescription = text;
  return true;
}