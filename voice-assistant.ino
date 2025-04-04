#include <Arduino.h>
#include <driver/i2s.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FS.h>
#include <SPIFFS.h>
#include <WebServer.h>
#include <ArduinoJson.h>

// WiFi credentials
const char* ssid = "Lightyear!";
const char* password = "12345678";

// Google Speech API settings
const char* googleHost = "speech.googleapis.com";
const int httpsPort = 443;
const char* apiKey = "your-google-api-key"; // Replace with your actual API key

// I2S pins and configuration
#define I2S_WS 26    // Word Select pin
#define I2S_SD 27    // Serial Data pin
#define I2S_SCK 25   // Serial Clock pin
#define I2S_PORT I2S_NUM_0
#define SAMPLE_RATE 16000
#define BUFFER_SIZE 512

// Buffer for audio recording
int32_t audioBuffer[BUFFER_SIZE];

// Web server for file access
WebServer server(80);

// Base64 encoding helper class
class Base64Encoder {
  private:
    static const char* encodingTable;
    uint8_t carry;
    int bufferPos;
    
  public:
    Base64Encoder() : carry(0), bufferPos(0) {}
    
    String encode(const uint8_t* data, size_t length) {
      String encoded;
      encoded.reserve((length + 2) / 3 * 4);
      
      int i = 0;
      // Process any leftover bytes from previous call
      if (bufferPos == 1) {
        if (length > 0) {
          encodeBlock(carry, data[0], 0, &encoded);
          i = 1;
          bufferPos = 0;
        }
      } else if (bufferPos == 2) {
        if (length > 0) {
          encodeBlock(carry, data[0], 0, &encoded);
          i = 1;
          bufferPos = 0;
        }
      }
      
      // Process three bytes at a time
      for (; i + 2 < length; i += 3) {
        encodeBlock(data[i], data[i + 1], data[i + 2], &encoded);
      }
      
      // Store any remaining bytes for next call
      if (i < length) {
        carry = data[i];
        bufferPos = 1;
        if (i + 1 < length) {
          carry = (carry << 8) | data[i + 1];
          bufferPos = 2;
        }
      }
      
      return encoded;
    }
    
    String finalize() {
      String encoded;
      // Handle any remaining bytes with padding
      if (bufferPos == 1) {
        encoded += encodingTable[(carry >> 2) & 0x3F];
        encoded += encodingTable[(carry << 4) & 0x3F];
        encoded += "==";  // Add padding
      } else if (bufferPos == 2) {
        encoded += encodingTable[(carry >> 10) & 0x3F];
        encoded += encodingTable[(carry >> 4) & 0x3F];
        encoded += encodingTable[(carry << 2) & 0x3F];
        encoded += "=";   // Add padding
      }
      bufferPos = 0;
      return encoded;
    }
    
  private:
    static void encodeBlock(uint8_t b1, uint8_t b2, uint8_t b3, String* output) {
      uint32_t block = (b1 << 16) | (b2 << 8) | b3;
      output->concat(encodingTable[(block >> 18) & 0x3F]);
      output->concat(encodingTable[(block >> 12) & 0x3F]);
      output->concat(encodingTable[(block >> 6) & 0x3F]);
      output->concat(encodingTable[block & 0x3F]);
    }
};

// Base64 encoding table
const char* Base64Encoder::encodingTable = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Function to connect to WiFi
void connectToWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  
  // Wait until connected
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWiFi connected");
  Serial.println("IP address: " + WiFi.localIP().toString());
}

// Function to configure I2S for microphone input
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

  // Install and configure I2S driver
  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);
  i2s_zero_dma_buffer(I2S_PORT);
}

// Function to write WAV header to file
void writeWavHeader(fs::File file, int totalAudioLen) {
  int32_t sampleRate = SAMPLE_RATE;
  int16_t bitsPerSample = 16;
  int16_t numChannels = 1;
  int32_t byteRate = sampleRate * numChannels * bitsPerSample / 8;
  int16_t blockAlign = numChannels * bitsPerSample / 8;
  int32_t subchunk2Size = totalAudioLen;
  int32_t chunkSize = 36 + subchunk2Size;

  // Go to beginning of file
  file.seek(0);
  
  // RIFF chunk descriptor
  file.write((const uint8_t*)"RIFF", 4);
  file.write((uint8_t*)&chunkSize, 4);
  file.write((const uint8_t*)"WAVE", 4);
  
  // Format sub-chunk
  file.write((const uint8_t*)"fmt ", 4);
  int32_t subchunk1Size = 16;
  int16_t audioFormat = 1; // PCM = 1
  file.write((uint8_t*)&subchunk1Size, 4);
  file.write((uint8_t*)&audioFormat, 2);
  file.write((uint8_t*)&numChannels, 2);
  file.write((uint8_t*)&sampleRate, 4);
  file.write((uint8_t*)&byteRate, 4);
  file.write((uint8_t*)&blockAlign, 2);
  file.write((uint8_t*)&bitsPerSample, 2);
  
  // Data sub-chunk
  file.write((const uint8_t*)"data", 4);
  file.write((uint8_t*)&subchunk2Size, 4);
}

// Function to record audio and save as WAV file
bool recordToWavFile(const char* filename, int recordingDurationSec) {
  Serial.printf("Recording for %d seconds...\n", recordingDurationSec);
  
  // Open file for writing
  File file = SPIFFS.open(filename, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to create file");
    return false;
  }
  
  // Skip header for now (we'll write it later)
  file.seek(44);
  
  // Calculate total samples to record
  int totalSamples = SAMPLE_RATE * recordingDurationSec;
  size_t bytesRead = 0;
  int samplesRecorded = 0;
  
  // Record audio in chunks
  while (samplesRecorded < totalSamples) {
    // Calculate how many samples to read in this iteration
    int samplesToRead = min(BUFFER_SIZE, totalSamples - samplesRecorded);
    
    // Read audio data from I2S
    esp_err_t result = i2s_read(I2S_PORT, (void*)audioBuffer, samplesToRead * sizeof(int32_t), &bytesRead, portMAX_DELAY);
    
    if (result == ESP_OK && bytesRead > 0) {
      // Convert 32-bit samples to 16-bit and write to file
      int samplesRead = bytesRead / sizeof(int32_t);
      for (int j = 0; j < samplesRead; j++) {
        // Convert 32-bit to 16-bit by right-shifting 16 bits (or 14 for some mics)
        int16_t sample16 = audioBuffer[j] >> 14;
        file.write((uint8_t*)&sample16, sizeof(sample16));
      }
      
      samplesRecorded += samplesRead;
      
      // Show progress
      if (samplesRecorded % (SAMPLE_RATE / 2) == 0) {
        Serial.printf("Recording: %.1f seconds\n", (float)samplesRecorded / SAMPLE_RATE);
      }
    }
    
    // Give system time to perform background tasks
    yield();
  }
  
  // Calculate data size and write header
  int dataSize = file.size() - 44;
  writeWavHeader(file, dataSize);
  file.close();
  
  Serial.printf("Recording saved as %s (%d bytes)\n", filename, dataSize + 44);
  return true;
}

// Function to send WAV file to Google's Speech-to-Text API in chunks
bool sendWavToGoogle(fs::FS &fs, const char* path) {
  Serial.printf("Sending %s to Google Speech API...\n", path);
  
  // Open the WAV file
  File audioFile = fs.open(path, "r");
  if (!audioFile) {
    Serial.println("Failed to open WAV file");
    return false;
  }
  
  // Connect to Google servers
  WiFiClientSecure client;
  client.setInsecure(); // Skip certificate verification
  
  Serial.println("Connecting to Google...");
  if (!client.connect(googleHost, httpsPort)) {
    Serial.println("Connection failed!");
    audioFile.close();
    return false;
  }
  
  // Calculate file size (excluding WAV header)
  size_t audioSize = audioFile.size() - 44;
  audioFile.seek(44); // Skip WAV header
  
  // Start of JSON payload
  String jsonStart = "{\"config\":{\"encoding\":\"LINEAR16\",\"sampleRateHertz\":16000,\"languageCode\":\"en-US\"},\"audio\":{\"content\":\"";
  // End of JSON payload
  String jsonEnd = "\"}}";
  
  // Calculate approximate total length after base64 encoding
  size_t base64Size = (audioSize * 4 / 3) + 4; // Base64 expansion + padding
  size_t totalLength = jsonStart.length() + base64Size + jsonEnd.length();
  
  // Send HTTP headers
  client.print("POST /v1/speech:recognize?key=");
  client.print(apiKey);
  client.println(" HTTP/1.1");
  client.print("Host: ");
  client.println(googleHost);
  client.println("Content-Type: application/json");
  client.print("Content-Length: ");
  client.println(totalLength);
  client.println("Connection: close");
  client.println();
  
  // Send JSON start
  client.print(jsonStart);
  
  // Process audio in chunks
  const size_t CHUNK_SIZE = 1024; // 1KB chunks
  uint8_t audioBuffer[CHUNK_SIZE];
  size_t bytesRead = 0;
  size_t totalBytesRead = 0;
  
  // Create base64 encoder that can handle streaming
  Base64Encoder encoder;
  
  // Read and encode the file in chunks
  while ((bytesRead = audioFile.read(audioBuffer, CHUNK_SIZE)) > 0) {
    // Convert this chunk to base64 and send
    String base64Chunk = encoder.encode(audioBuffer, bytesRead);
    client.print(base64Chunk);
    
    totalBytesRead += bytesRead;
    
    // Show progress periodically
    if (totalBytesRead % (CHUNK_SIZE * 10) == 0) {
      Serial.printf("Sent %u bytes (%.1f%%)\n", totalBytesRead, 
                   (totalBytesRead * 100.0) / audioSize);
    }
    
    // Give system time for background tasks
    yield();
  }
  
  // Finalize base64 encoding
  String remaining = encoder.finalize();
  if (remaining.length() > 0) {
    client.print(remaining);
  }
  
  // Send JSON end
  client.print(jsonEnd);
  
  audioFile.close();
  
  // Wait for and parse response
  Serial.println("Waiting for Google response...");
  String responseStatus = "";
  bool headersDone = false;
  
  // Read HTTP response headers
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line.startsWith("HTTP/")) {
      responseStatus = line;
    }
    // Empty line marks end of headers
    if (line == "\r") {
      headersDone = true;
      break;
    }
  }
  
  // Read and parse JSON response
  DynamicJsonDocument jsonResponse(4096);
  String response = "";
  
  if (headersDone) {
    response = client.readString();
    Serial.println("Google Response Status: " + responseStatus);
    Serial.println("Response Body:");
    Serial.println(response);
    
    // Parse JSON response
    DeserializationError error = deserializeJson(jsonResponse, response);
    if (!error) {
      // Check if we have results
      if (jsonResponse.containsKey("results")) {
        JsonArray results = jsonResponse["results"];
        for (JsonObject result : results) {
          JsonArray alternatives = result["alternatives"];
          for (JsonObject alternative : alternatives) {
            if (alternative.containsKey("transcript")) {
              Serial.println("\nTranscript: " + alternative["transcript"].as<String>());
              if (alternative.containsKey("confidence")) {
                Serial.printf("Confidence: %.2f\n", alternative["confidence"].as<float>());
              }
            }
          }
        }
      } else if (jsonResponse.containsKey("error")) {
        // Handle error response
        Serial.println("Error: " + jsonResponse["error"]["message"].as<String>());
        Serial.println("Code: " + jsonResponse["error"]["code"].as<String>());
      }
    } else {
      Serial.println("Failed to parse JSON response");
    }
  } else {
    Serial.println("Failed to get proper response from server");
  }
  
  return true;
}

// Web server routes setup
void setupWebServer() {
  // Serve WAV file
  server.on("/recording.wav", HTTP_GET, []() {
    File file = SPIFFS.open("/recording.wav", "r");
    if (!file) {
      server.send(404, "text/plain", "File not found");
      return;
    }
    server.streamFile(file, "audio/wav");
    file.close();
  });

  // Simple HTML interface
  server.on("/", HTTP_GET, []() {
    String html = "<html><body>";
    html += "<h2>ESP32 Audio Recorder</h2>";
    html += "<p>Current IP: " + WiFi.localIP().toString() + "</p>";
    html += "<p><a href='/recording.wav'>Download WAV File</a></p>";
    html += "<p><a href='/record'>Start New Recording (10 seconds)</a></p>";
    html += "<p><a href='/transcribe'>Transcribe Recording</a></p>";
    html += "</body></html>";
    server.send(200, "text/html", html);
  });
  
  // Start recording
  server.on("/record", HTTP_GET, []() {
    server.send(200, "text/plain", "Recording started (10 seconds)...");
    recordToWavFile("/recording.wav", 10);
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
  
  // Transcribe recording
  server.on("/transcribe", HTTP_GET, []() {
    server.send(200, "text/plain", "Transcribing... check serial output for results");
    sendWavToGoogle(SPIFFS, "/recording.wav");
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });
  
  server.begin();
  Serial.println("Web server started");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\nESP32 Google Speech Recognition");
  
  // Connect to WiFi
  connectToWiFi();
  
  // Initialize file system
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS Mount Failed");
    return;
  }
  Serial.println("SPIFFS mounted successfully");
  
  // Configure I2S for audio input
  configureI2S();
  Serial.println("I2S configured");
  
  // Setup web server
  setupWebServer();
  
  // Initial recording (optional)
  Serial.println("Starting initial recording (10 seconds)...");
  if (recordToWavFile("/recording.wav", 10)) {
    Serial.println("Initial recording completed");
    
    // Send to Google (optional for initial startup)
    Serial.println("Sending initial recording to Google...");
    sendWavToGoogle(SPIFFS, "/recording.wav");
  }
  
  Serial.println("Setup complete. Access the web interface at http://" + WiFi.localIP().toString());
}

void loop() {
  // Handle web server clients
  server.handleClient();
  
  // Add a small delay to prevent watchdog timer issues
  delay(10);
}
