#include <Arduino.h>
#include <driver/i2s.h>
#include <WiFi.h>
#include <FS.h>
#include <SPIFFS.h>
#include <WebServer.h>

// WiFi credentials
const char* ssid = "Lightyear!";
const char* password = "12345678";

// I2S pins and configuration
#define I2S_WS 26
#define I2S_SD 27
#define I2S_SCK 25
#define I2S_PORT I2S_NUM_0
#define SAMPLE_RATE 16000
#define BUFFER_SIZE 512

int32_t audioBuffer[BUFFER_SIZE];

WebServer server(80);

void connectToWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
  Serial.println(WiFi.localIP());
}

void configureI2S() {
  const i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = BUFFER_SIZE,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  const i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);
  i2s_zero_dma_buffer(I2S_PORT);
}

#include "base64.h"
#include <ArduinoJson.h>
#include "FS.h"
#include "SPIFFS.h"

const char* apiKey = "api-key"; // Replace with your actual API key

#include <WiFiClientSecure.h>

const char* googleHost = "speech.googleapis.com";
const int httpsPort = 443;

bool sendWavToGoogle(fs::FS &fs, const char* path) {
  File audioFile = fs.open(path, "r");
  if (!audioFile) {
    Serial.println("Failed to open WAV file");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  Serial.println("Connecting to Google...");
  if (!client.connect(googleHost, httpsPort)) {
    Serial.println("Connection failed!");
    audioFile.close();
    return false;
  }

  // Prepare request
  String boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
  String startRequest = "--" + boundary + "\r\n";
  startRequest += "Content-Disposition: form-data; name=\"config\"\r\n";
  startRequest += "Content-Type: application/json; charset=UTF-8\r\n\r\n";
  startRequest += "{\"config\":{\"encoding\":\"LINEAR16\",\"sampleRateHertz\":16000,\"languageCode\":\"en-US\"}}\r\n";
  startRequest += "--" + boundary + "\r\n";
  startRequest += "Content-Disposition: form-data; name=\"audio\"; filename=\"recording.wav\"\r\n";
  startRequest += "Content-Type: audio/wav\r\n\r\n";

  String endRequest = "\r\n--" + boundary + "--\r\n";

  client.printf("POST /v1/speech:recognize?key=%s HTTP/1.1\r\n", apiKey);
  client.printf("Host: %s\r\n", googleHost);
  client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary.c_str());
  client.printf("Transfer-Encoding: chunked\r\n");
  client.printf("Connection: close\r\n\r\n");

  client.printf("%X\r\n", startRequest.length());
  client.print(startRequest);
  client.print("\r\n");

  // Send audio content
  uint8_t buffer[1024];
  size_t bytesRead;
  while ((bytesRead = audioFile.read(buffer, sizeof(buffer))) > 0) {
    client.printf("%X\r\n", bytesRead);
    client.write(buffer, bytesRead);
    client.print("\r\n");
  }

  client.printf("%X\r\n", endRequest.length());
  client.print(endRequest);
  client.print("\r\n");

  client.print("0\r\n\r\n");

  audioFile.close();

  Serial.println("Waiting for Google response...");
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
  }
  String response = client.readString();
  Serial.println("Google Response:");
  Serial.println(response);

  return true;
}

void writeWavHeader(fs::File file, int totalAudioLen) {
  int32_t sampleRate = SAMPLE_RATE;
  int16_t bitsPerSample = 16;
  int16_t numChannels = 1;
  int32_t byteRate = sampleRate * numChannels * bitsPerSample / 8;
  int16_t blockAlign = numChannels * bitsPerSample / 8;
  int32_t subchunk2Size = totalAudioLen;
  int32_t chunkSize = 36 + subchunk2Size;

  file.seek(0);
  file.write((const uint8_t*)"RIFF", 4);
  file.write((uint8_t*)&chunkSize, 4);
  file.write((const uint8_t*)"WAVE", 4);
  file.write((const uint8_t*)"fmt ", 4);

  int32_t subchunk1Size = 16;
  int16_t audioFormat = 1;
  file.write((uint8_t*)&subchunk1Size, 4);
  file.write((uint8_t*)&audioFormat, 2);
  file.write((uint8_t*)&numChannels, 2);
  file.write((uint8_t*)&sampleRate, 4);
  file.write((uint8_t*)&byteRate, 4);
  file.write((uint8_t*)&blockAlign, 2);
  file.write((uint8_t*)&bitsPerSample, 2);

  file.write((const uint8_t*)"data", 4);
  file.write((uint8_t*)&subchunk2Size, 4);
}

void recordToWavFile() {
  File file = SPIFFS.open("/recording.wav", FILE_WRITE);
  if (!file) {
    Serial.println("Failed to create file");
    return;
  }

  file.seek(44); // leave space for WAV header

  int totalSamples = SAMPLE_RATE * 5; // 5 seconds
  size_t bytesRead = 0;

  for (int i = 0; i < totalSamples / BUFFER_SIZE; i++) {
    esp_err_t result = i2s_read(I2S_PORT, (void*)audioBuffer, sizeof(audioBuffer), &bytesRead, portMAX_DELAY);
    if (result == ESP_OK && bytesRead > 0) {
      for (int j = 0; j < BUFFER_SIZE; j++) {
        int16_t sample16 = audioBuffer[j] >> 14;
        file.write((uint8_t*)&sample16, sizeof(sample16));
      }
    }
  }

  int dataSize = file.size() - 44;
  writeWavHeader(file, dataSize);
  file.close();
  Serial.println("Recording saved as /recording.wav");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  connectToWiFi();

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed");
    return;
  }

  configureI2S();
  recordToWavFile();

  server.on("/recording.wav", HTTP_GET, []() {
    File file = SPIFFS.open("/recording.wav", "r");
    if (!file) {
      server.send(404, "text/plain", "File not found");
      return;
    }
    server.streamFile(file, "audio/wav");
    file.close();
  });

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", "<h2>ESP32 Audio</h2><a href='/recording.wav'>Download WAV File</a>");
  });

  server.begin();
  Serial.println(WiFi.localIP());
  Serial.println("Web server started. Access /recording.wav to listen.");

  sendWavToGoogle(SPIFFS, "/recording.wav");
}

void loop() {
  vTaskDelay(10 / portTICK_PERIOD_MS);
  server.handleClient();
}
