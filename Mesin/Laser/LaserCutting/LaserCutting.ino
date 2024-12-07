/*
 * BAGIAN 1: LIBRARY DAN KONSTANTA
 */
#include <WiFi.h>        // Untuk koneksi WiFi
#include <HTTPClient.h>   // Untuk melakukan HTTP request
#include <ArduinoJson.h>  // Untuk parsing JSON
#include <WiFiClientSecure.h> // Untuk HTTPS connection
#include <NTPClient.h>    // Untuk sinkronisasi waktu
#include <WiFiUdp.h>      // Diperlukan untuk NTPClient
#include <SPI.h>          // Untuk komunikasi dengan SD Card
#include <SD.h>           // Untuk akses SD Card
#include <FS.h>           // File System untuk SD Card

/*
 * BAGIAN 2: KONFIGURASI PIN DAN VARIABEL KONSTAN
 */
// Konfigurasi WiFi
const char* ssid = "your_ssid";
const char* password = "your_password";
const char* serverName = "your_server";

// Pin GPIO Assignment
const int relay = 32;          // Pin untuk relay
const int buzzerPin = 33;      // Pin untuk buzzer
const int currentSensorPin = 34; // Pin untuk sensor arus
const int chipSelect = 5;      // Pin CS untuk SD Card

// Interval waktu untuk polling
const long buttonInterval = 1000;  // Interval cek button (1 detik)
const long currentInterval = 5000; // Interval cek arus (5 detik)
const int MAX_RETRIES = 2;        // Maksimum percobaan retry
const int RETRY_DELAY = 500;      // Delay antar retry (500ms)

/*
 * BAGIAN 3: VARIABEL GLOBAL
 */
// Variabel tracking waktu
unsigned long lastButtonTime = 0;  // Waktu terakhir cek button
unsigned long lastCurrentTime = 0;  // Waktu terakhir cek arus
unsigned long endTime = 0;         // Waktu akhir peminjaman

// Variabel sensor arus
float offsetI = 2048;             // Offset kalibrasi sensor arus
float sensitivity = 0.075;        // Sensitivitas sensor arus

// Status perangkat
bool lastButtonState = false;     // Status terakhir relay
static bool currentBuzzerState = false;  // Status terakhir buzzer

// Variabel untuk logging
String logFileName = "";          // Nama file log
String currentDate = "";          // Tanggal saat ini

// Objek untuk koneksi
WiFiClientSecure client;         // Client untuk HTTPS
WiFiUDP ntpUDP;                 // UDP untuk NTP
NTPClient timeClient(ntpUDP, "pool.ntp.org", 25200); // NTP Client (GMT+7)

/*
 * BAGIAN 4: DEKLARASI PROTOTIPE FUNGSI
 */
// Fungsi SD Card
void initSDCard();                // Inisialisasi SD Card
void writeFile(fs::FS &fs, const char * path, const char * message);  // Menulis file baru
void appendFile(fs::FS &fs, const char * path, const char * message); // Menambah data ke file

// Fungsi waktu dan logging
String getCurrentDate();          // Mendapatkan tanggal saat ini
void updateLogFile();            // Update file log berdasarkan tanggal
void logData(float current, bool buttonState, bool buzzerState); // Log data ke SD

// Fungsi sensor dan pengukuran
float calibrateOffset();         // Kalibrasi offset sensor
float measureCurrent();          // Mengukur arus
float calculateRMS(unsigned long *samples, int num_samples); // Menghitung RMS

// Fungsi komunikasi dan kontrol
bool checkButtonState();         // Cek status dari server
bool sendCurrentData();          // Kirim data arus ke server
void updateEndTime(String timeStr); // Update waktu akhir peminjaman

/*
 * BAGIAN 5: SETUP DAN LOOP UTAMA
 */
void setup() {
  // Inisialisasi Serial untuk debugging
  Serial.begin(115200);
  
  // Konfigurasi pin mode
  pinMode(relay, OUTPUT);
  pinMode(currentSensorPin, INPUT);
  pinMode(buzzerPin, OUTPUT);
  // digitalWrite(buzzerPin, LOW); // Pastikan buzzer mati saat startup
  // digitalWrite(relay, LOW);     // Pastikan relay mati saat startup
  digitalWrite(buzzerPin, HIGH); // Pastikan buzzer mati saat startup
  digitalWrite(relay, LOW);     // Pastikan relay mati saat startup


  // Koneksi WiFi
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected to WiFi");

  // Inisialisasi komponen lain
  client.setInsecure();        // Allow insecure HTTPS
  timeClient.begin();          // Mulai NTP Client
  timeClient.update();         // Update waktu pertama kali
  
  // Kalibrasi sensor arus
  offsetI = calibrateOffset();
  Serial.printf("Kalibrasi Offset: %.2f\n", offsetI);

  // Inisialisasi SD Card
  initSDCard();
  updateLogFile(); // Buat/update file log
}

void loop() {
  unsigned long currentMillis = millis();
  timeClient.update(); // Update waktu dari NTP

  // Cek button state setiap interval
  if (currentMillis - lastButtonTime >= buttonInterval) {
    lastButtonTime = currentMillis;
    
    for (int i = 0; i < MAX_RETRIES; i++) {
      if (checkButtonState()) break;
      delay(RETRY_DELAY);
    }
  }

  // Cek dan kirim data arus setiap interval
  if (currentMillis - lastCurrentTime >= currentInterval) {
    lastCurrentTime = currentMillis;
    
    for (int i = 0; i < MAX_RETRIES; i++) {
      if (sendCurrentData()) break;
      delay(RETRY_DELAY);
    }
  }

  // Cek apakah peminjaman sudah berakhir
  if (endTime > 0 && currentMillis >= endTime) {
    // Matikan semua perangkat
    // digitalWrite(relay, HIGH);
    // digitalWrite(buzzerPin, HIGH);
    digitalWrite(relay, LOW);
    digitalWrite(buzzerPin, LOW);
    currentBuzzerState = false;
    lastButtonState = false;
    
    // Log status akhir
    float finalCurrent = measureCurrent();
    logData(finalCurrent, false, false);
    
    Serial.println("Peminjaman berakhir. Semua perangkat dimatikan.");
    endTime = 0;
  }
}

/*
 * BAGIAN 6: IMPLEMENTASI FUNGSI KOMUNIKASI
 */
bool checkButtonState() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String url = String(serverName) + "/:your_type/your_endpoint";
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.GET();
  bool success = false;

  if (httpCode > 0) {
    String payload = http.getString();
    Serial.println("Server response: " + payload);

    StaticJsonDocument<500> doc;
    DeserializationError error = deserializeJson(doc, payload);

    if (!error) {
      // Handle button status
      if (doc["data"].containsKey("button")) {
        bool newButtonState = doc["data"]["button"];
        if (newButtonState != lastButtonState) {
          lastButtonState = newButtonState;
          // digitalWrite(relay, newButtonState ? HIGH : LOW);
          digitalWrite(relay, newButtonState ? HIGH : LOW);
          Serial.printf("Relay state changed to: %s\n", newButtonState ? "ON" : "OFF");
        }

        // Handle buzzer status
        if (doc["data"].containsKey("buzzerStatus")) {
          bool newBuzzerState = doc["data"]["buzzerStatus"];
          if (newBuzzerState != currentBuzzerState) {
            currentBuzzerState = newBuzzerState;
            // digitalWrite(buzzerPin, newBuzzerState ? HIGH : LOW);
            digitalWrite(buzzerPin, newBuzzerState ? LOW : HIGH);
            Serial.printf("Buzzer state changed to: %s\n", newBuzzerState ? "ON" : "OFF");
          }

          // Log current state
          float currentValue = measureCurrent();
          logData(currentValue, lastButtonState, currentBuzzerState);
        }
      }
      success = true;
    } else {
      Serial.printf("JSON parsing failed: %s\n", error.c_str());
    }
  } else {
    Serial.printf("HTTP request failed: %d\n", httpCode);
  }

  http.end();
  return success;
}

/*
 * BAGIAN 7: IMPLEMENTASI FUNGSI SENSOR ARUS
 */
float calibrateOffset() {
  Serial.println("Memulai kalibrasi sensor arus...");
  unsigned long sum = 0;
  const int num_samples = 1000;
  
  // Ambil 1000 sampel untuk kalibrasi
  for (int i = 0; i < num_samples; i++) {
    sum += analogRead(currentSensorPin);
    delay(1);
  }
  
  float offset = sum / num_samples;
  Serial.printf("Kalibrasi selesai. Offset: %.2f\n", offset);
  return offset;
}

float calculateRMS(unsigned long *samples, int num_samples) {
  unsigned long sum = 0;
  
  // Hitung RMS dari sampel
  for (int i = 0; i < num_samples; i++) {
    float value = samples[i] - offsetI;
    sum += value * value;
  }
  
  float mean = sum / num_samples;
  return sqrt(mean);
}

float measureCurrent() {
  const int numSamples = 600;
  unsigned long values[numSamples];
  
  // Ambil sampel
  for (int i = 0; i < numSamples; i++) {
    values[i] = analogRead(currentSensorPin);
    delay(1);
  }

  // Konversi ke nilai arus
  float adcRMS = calculateRMS(values, numSamples);
  float voltageRMS = (adcRMS / 4095.0) * 3.3; // Konversi ke voltage (ESP32 ADC: 12-bit)
  float current = voltageRMS / sensitivity;
  
  // Filter noise
  if (current < 0.02) {
    current = 0;
  }

  Serial.printf("Pengukuran arus: %.2fA\n", current);
  return current;
}

/*
 * BAGIAN 8: IMPLEMENTASI FUNGSI SD CARD
 */
void initSDCard() {
  Serial.println("Inisialisasi SD Card...");
  
  if (!SD.begin(chipSelect)) {
    Serial.println("Inisialisasi SD Card gagal!");
    return;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("Tidak ada SD Card terpasang");
    return;
  }

  // Tampilkan informasi SD Card
  Serial.print("Tipe SD Card: ");
  switch (cardType) {
    case CARD_MMC: Serial.println("MMC"); break;
    case CARD_SD: Serial.println("SDSC"); break;
    case CARD_SDHC: Serial.println("SDHC"); break;
    default: Serial.println("UNKNOWN"); break;
  }

  uint64_t cardSize = SD.cardSize() / (1024 * 1024);
  Serial.printf("Ukuran SD Card: %lluMB\n", cardSize);
}

void writeFile(fs::FS &fs, const char* path, const char* message) {
  Serial.printf("Menulis file: %s\n", path);

  File file = fs.open(path, FILE_WRITE);
  if (!file) {
    Serial.println("Gagal membuka file untuk menulis");
    return;
  }

  if (file.print(message)) {
    Serial.println("File berhasil ditulis");
  } else {
    Serial.println("Gagal menulis file");
  }
  
  file.close();
}

void appendFile(fs::FS &fs, const char* path, const char* message) {
  Serial.printf("Menambah ke file: %s\n", path);

  File file = fs.open(path, FILE_APPEND);
  if (!file) {
    Serial.println("Gagal membuka file untuk menambah");
    return;
  }

  if (file.print(message)) {
    Serial.println("Pesan berhasil ditambahkan");
  } else {
    Serial.println("Gagal menambah pesan");
  }
  
  file.close();
}

/*
 * BAGIAN 9: IMPLEMENTASI FUNGSI WAKTU DAN LOGGING
 */
String getCurrentDate() {
    time_t epochTime = timeClient.getEpochTime();
    struct tm *timeinfo = localtime(&epochTime);
    
    char dateStr[11];
    snprintf(dateStr, sizeof(dateStr), "%02d-%02d-%04d", 
             timeinfo->tm_mday, timeinfo->tm_mon + 1, timeinfo->tm_year + 1900);
    
    return String(dateStr);
}

void updateLogFile() {
    String newDate = getCurrentDate();
    
    // Buat file log baru jika tanggal berubah
    if (newDate != currentDate) {
        currentDate = newDate;
        logFileName = "/logDataLaser_" + currentDate + ".csv";
        
        if (!SD.exists(logFileName)) {
            const char* header = "Time,Current (A),Kondisi Mesin,Notifikasi Peringatan\n";
            writeFile(SD, logFileName.c_str(), header);
            Serial.println("File log baru dibuat: " + logFileName);
        } else {
            Serial.println("File log sudah ada: " + logFileName);
        }
    }
}

void logData(float current, bool buttonState, bool buzzerState) {
    if (!timeClient.isTimeSet()) {
        Serial.println("Peringatan: Waktu belum tersinkronisasi!");
        timeClient.forceUpdate();
    }

    updateLogFile();

    String timeStamp = timeClient.getFormattedTime();
    String currentStr = String(current, 2) + "A";
    String stateStr = buttonState ? "Nyala" : "Mati";
    String buzzerStr = buzzerState ? "Aktif" : "Nonaktif";
    
    String dataMessage = timeStamp + "," + 
                        currentStr + "," + 
                        stateStr + "," + 
                        buzzerStr + "\n";
    
    appendFile(SD, logFileName.c_str(), dataMessage.c_str());
    Serial.println("Data dilog: " + dataMessage);
}

/*
 * BAGIAN 10: IMPLEMENTASI FUNGSI PENGIRIMAN DATA
 */
bool sendCurrentData() {
  if (WiFi.status() != WL_CONNECTED) return false;

  float current = measureCurrent();
  String url = String(serverName) + "/:your_type/your_endpoint";
  
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  // Persiapkan data JSON
  StaticJsonDocument<200> doc;
  doc["current"] = current;
  String httpRequestData;
  serializeJson(doc, httpRequestData);

  int httpResponseCode = http.POST(httpRequestData);
  bool success = false;

  if (httpResponseCode > 0) {
    Serial.printf("Data arus terkirim. Response: %d\n", httpResponseCode);
    logData(current, lastButtonState, currentBuzzerState);
    success = true;
  } else {
    Serial.printf("Gagal mengirim data arus. Error: %s\n", 
                 http.errorToString(httpResponseCode).c_str());
  }

  http.end();
  return success;
}

/*
 * BAGIAN 11: IMPLEMENTASI FUNGSI UPDATE WAKTU AKHIR
 */
void updateEndTime(String timeStr) {
  // Parsing waktu dari string ISO8601
  struct tm tm;
  strptime(timeStr.c_str(), "%Y-%m-%dT%H:%M:%S", &tm);
  time_t newEndTime = mktime(&tm);
  
  unsigned long currentTime = timeClient.getEpochTime();
  
  if (newEndTime > currentTime) {
    // Konversi ke millis untuk tracking internal
    endTime = millis() + ((newEndTime - currentTime) * 1000);
    Serial.printf("Waktu akhir diperbarui: %s\n", timeStr.c_str());
  } else {
    Serial.println("Waktu akhir tidak valid");
  }
}