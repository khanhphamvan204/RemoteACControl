#include <Arduino.h>
#include <WiFi.h>
#include <DHT.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>
#define BLYNK_AUTH_TOKEN "kZhv-7PphddZWZXvg2L_QDuuAiCbg1R8"
#define BLYNK_TEMPLATE_ID "TMPL6zXSFYGz3"
#define BLYNK_TEMPLATE_NAME "AC"
#include <BlynkSimpleEsp32.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

// Khai báo prototype cho các hàm
void setupWebServer();
void sendACCommand();

// ============ CẤU HÌNH CHÂN KẾT NỐI ============
#define DHT_PIN 4
#define IR_SEND_PIN 5
#define IR_RECV_PIN 18
#define LED_STATUS 2
#define LED_WIFI 15
#define BTN_MANUAL 19

// ============ CẤU HÌNH WIFI và BLYNK ============
#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASSWORD ""

// ============ CẤU HÌNH DHT22 ============
#define DHT_TYPE DHT22
DHT dht(DHT_PIN, DHT_TYPE);

// ============ CẤU HÌNH IR ============
IRsend irsend(IR_SEND_PIN);
IRrecv irrecv(IR_RECV_PIN);
decode_results results;

// ============ CẤU HÌNH WEBSERVER ============
AsyncWebServer server(80);

// ============ BIẾN TOÀN CỤC ============
float temperature = 0;
float humidity = 0;
bool acStatus = false;
int acTemp = 25;
String acMode = "COOL";
unsigned long lastSensorRead = 0;
const long sensorInterval = 2000;
unsigned long lastBlynkUpdate = 0;
const long blynkUpdateInterval = 5000;

// Biến cho xử lý nút bấm
bool lastButtonState = HIGH;
unsigned long lastButtonPress = 0;
const unsigned long buttonCooldown = 500;

// ============ HÀM KHỞI TẠO ============
void setup()
{
  Serial.begin(115200);
  Serial.println("\n=== HỆ THỐNG ĐIỀU KHIỂN ĐIỀU HÒA TỪ XA ===");

  pinMode(LED_STATUS, OUTPUT);
  pinMode(LED_WIFI, OUTPUT);
  pinMode(BTN_MANUAL, INPUT_PULLUP);

  dht.begin();
  Serial.println("✓ DHT22 đã khởi tạo");
  irsend.begin();
  irrecv.enableIRIn();
  Serial.println("✓ IR đã khởi tạo");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
    digitalWrite(LED_WIFI, !digitalRead(LED_WIFI));
  }
  digitalWrite(LED_WIFI, HIGH);
  Serial.println("\n✓ Đã kết nối WiFi!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  Blynk.begin(BLYNK_AUTH_TOKEN, WIFI_SSID, WIFI_PASSWORD);
  Serial.println("✓ Blynk đã kết nối!");

  // Khởi tạo API endpoints
  setupWebServer();

  Serial.println("✓ Hệ thống sẵn sàng!");
  digitalWrite(LED_STATUS, LOW);
  Serial.println("\n📌 DEBUG: Nhấn nút, gửi IR, hoặc gọi API để test...");
}

// ============ KHỞI TẠO WEBSERVER ============
void setupWebServer()
{
  // Root endpoint
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "application/json", "{\"message\":\"AC Control API is running!\"}"); });

  // Endpoint đọc sensor
  server.on("/sensors", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    StaticJsonDocument<200> doc;
    doc["temperature"] = temperature;
    doc["humidity"] = humidity;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  // Endpoint lấy trạng thái AC
  server.on("/ac/status", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    StaticJsonDocument<200> doc;
    doc["status"] = acStatus;
    doc["temp"] = acTemp;
    doc["mode"] = acMode;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  // Endpoint gửi lệnh AC
  server.on("/ac/command", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (request->hasParam("body", true)) {
      String body = request->getParam("body", true)->value();
      StaticJsonDocument<200> doc;
      DeserializationError error = deserializeJson(doc, body);
      if (error) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      if (doc.containsKey("status")) {
        acStatus = doc["status"];
        if (doc.containsKey("temp")) acTemp = doc["temp"];
        if (doc.containsKey("mode")) acMode = doc["mode"].as<String>();
        sendACCommand();
        StaticJsonDocument<200> responseDoc;
        responseDoc["status"] = acStatus;
        responseDoc["temp"] = acTemp;
        responseDoc["mode"] = acMode;
        String response;
        serializeJson(responseDoc, response);
        request->send(200, "application/json", response);
      } else {
        request->send(400, "application/json", "{\"error\":\"Missing status field\"}");
      }
    } else {
      request->send(400, "application/json", "{\"error\":\"No body provided\"}");
    } });

  // Endpoint mô phỏng nút bấm
  server.on("/manual/button", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    acStatus = !acStatus;
    sendACCommand();
    StaticJsonDocument<200> doc;
    doc["status"] = acStatus;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  // Endpoint nhận IR
  server.on("/ir/receive", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (request->hasParam("body", true)) {
      String body = request->getParam("body", true)->value();
      StaticJsonDocument<200> doc;
      DeserializationError error = deserializeJson(doc, body);
      if (error) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }

      if (doc.containsKey("code")) {
        uint32_t code = doc["code"];
        Serial.printf("📡 Nhận IR qua API: 0x%lX\n", code);
        if (code == 1) {
          acStatus = true;
          sendACCommand();
          request->send(200, "application/json", "{\"message\":\"Turn ON AC\"}");
        } else if (code == 2) {
          acStatus = false;
          sendACCommand();
          request->send(200, "application/json", "{\"message\":\"Turn OFF AC\"}");
        } else {
          request->send(200, "application/json", "{\"message\":\"Unknown IR code\"}");
        }
      } else {
        request->send(400, "application/json", "{\"error\":\"Missing code field\"}");
      }
    } else {
      request->send(400, "application/json", "{\"error\":\"No body provided\"}");
    } });

  server.begin();
  Serial.println("✓ WebServer đã khởi tạo!");
}

// ============ GỬI LỆNH IR ĐẾN ĐIỀU HÒA ============
void sendACCommand()
{
  Serial.println("→ Gửi lệnh IR đến điều hòa");
  uint32_t codeOn = 0x00020906;
  uint32_t codeOff = 0x00029069;

  if (acStatus)
  {
    irsend.sendNEC(codeOn, 32);
    digitalWrite(LED_STATUS, HIGH);
    Serial.println("✓ Đã bật điều hòa");
  }
  else
  {
    irsend.sendNEC(codeOff, 32);
    digitalWrite(LED_STATUS, LOW);
    Serial.println("✓ Đã tắt điều hòa");
  }

  Blynk.virtualWrite(V2, acStatus ? 1 : 0);
}

// ============ ĐỌC CẢM BIẾN ============
void readSensors()
{
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(h) || isnan(t))
  {
    Serial.println("✗ Lỗi đọc DHT22!");
    return;
  }

  temperature = t;
  humidity = h;
  Serial.printf("🌡️ Nhiệt độ: %.1f°C | 💧 Độ ẩm: %.1f%%\n", temperature, humidity);

  if (millis() - lastBlynkUpdate >= blynkUpdateInterval)
  {
    Blynk.virtualWrite(V0, temperature);
    Blynk.virtualWrite(V1, humidity);
    lastBlynkUpdate = millis();
    Serial.println("📤 Đã gửi dữ liệu lên Blynk");
  }
}

// ============ XỬ LÝ NÚT BẤM ============
void handleManualButton()
{
  int currentButtonState = digitalRead(BTN_MANUAL);
  if (currentButtonState != lastButtonState)
  {
    Serial.print("🔍 DEBUG - Trạng thái nút: ");
    Serial.println(currentButtonState == LOW ? "NHẤN (LOW)" : "THẢ (HIGH)");
  }

  if (lastButtonState == HIGH && currentButtonState == LOW)
  {
    if (millis() - lastButtonPress > buttonCooldown)
    {
      Serial.println("✅ Phát hiện nhấn nút hợp lệ!");
      acStatus = !acStatus;
      sendACCommand();
      Serial.printf("🔘 Nút thủ công - Trạng thái mới: %s\n", acStatus ? "BẬT" : "TẮT");
      lastButtonPress = millis();
    }
    else
    {
      Serial.println("⏱️ Nhấn quá nhanh - bỏ qua");
    }
  }

  lastButtonState = currentButtonState;
}

// ============ XỬ LÝ LỆNH TỪ BLYNK ============
BLYNK_WRITE(V2)
{
  acStatus = param.asInt();
  sendACCommand();
  Serial.println("📱 Nhận lệnh từ Blynk");
}

// ============ NHẬN TÍN HIỆU IR ============
void receiveIR()
{
  if (irrecv.decode(&results))
  {
    Serial.print("📡 Nhận IR: ");
    serialPrintUint64(results.value, HEX);
    Serial.println();
    uint32_t code = results.value;

    if (code == 1)
    {
      Serial.println("🟢 Nhận lệnh IR: BẬT điều hòa");
      acStatus = true;
      sendACCommand();
    }
    else if (code == 2)
    {
      Serial.println("🔴 Nhận lệnh IR: TẮT điều hòa");
      acStatus = false;
      sendACCommand();
    }
    else
    {
      Serial.printf("⚙️ Mã IR khác: 0x%lX\n", code);
    }

    irrecv.resume();
  }
}

// ============ LOOP ============
void loop()
{
  Blynk.run();

  if (WiFi.status() != WL_CONNECTED)
  {
    digitalWrite(LED_WIFI, LOW);
    Serial.println("✗ Mất kết nối WiFi!");
    while (WiFi.status() != WL_CONNECTED)
      delay(500);
    digitalWrite(LED_WIFI, HIGH);
    Serial.println("✓ Đã kết nối WiFi lại!");
    Blynk.connect();
  }

  if (millis() - lastSensorRead > sensorInterval)
  {
    lastSensorRead = millis();
    readSensors();
  }

  handleManualButton();
  receiveIR();
}