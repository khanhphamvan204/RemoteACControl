#include <Arduino.h>
#include <WiFi.h>
#include <DHT.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <IRutils.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

// ============ BẢO MẬT: API KEY ============
#define API_KEY "AC_SECRET_KEY_2024_CHANGE_THIS" // THAY ĐỔI KEY NÀY!

// ============ CẤU HÌNH CHÂN KẾT NỐI ============
#define DHT_PIN 4
#define IR_SEND_PIN 5
#define IR_RECV_PIN 18
#define LED_STATUS 2
#define LED_WIFI 15
#define BTN_MANUAL 19

// ============ CẤU HÌNH WIFI ============
#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASSWORD ""

// ============ CẤU HÌNH DHT22 ============
#define DHT_TYPE DHT22
DHT dht(DHT_PIN, DHT_TYPE);

// ============ CẤU HÌNH IR ============
IRsend irsend(IR_SEND_PIN);
IRrecv irrecv(IR_RECV_PIN);
decode_results results;

// ============ WEBSERVER ============
AsyncWebServer server(80);

// ============ BIẾN TOÀN CỤC ============
float temperature = 0;
float humidity = 0;
bool acStatus = false;
int acTemp = 25;
String acMode = "COOL";
unsigned long lastSensorRead = 0;
const long sensorInterval = 2000;

bool lastButtonState = HIGH;
unsigned long lastButtonPress = 0;
const unsigned long buttonCooldown = 500;

// ============ HÀM XÁC THỰC API KEY ============
bool authenticateRequest(AsyncWebServerRequest *request)
{
  // Kiểm tra header Authorization
  if (request->hasHeader("Authorization"))
  {
    String auth = request->header("Authorization");

    // Hỗ trợ 2 format:
    // 1. Authorization: Bearer YOUR_API_KEY
    // 2. Authorization: YOUR_API_KEY

    if (auth.startsWith("Bearer "))
    {
      auth = auth.substring(7);
    }

    if (auth == API_KEY)
    {
      return true;
    }
  }

  // Kiểm tra query parameter ?api_key=xxx
  if (request->hasParam("api_key"))
  {
    String key = request->getParam("api_key")->value();
    if (key == API_KEY)
    {
      return true;
    }
  }

  return false;
}

// ============ HÀM GỬI LỖI UNAUTHORIZED ============
void sendUnauthorized(AsyncWebServerRequest *request)
{
  request->send(401, "application/json",
                "{\"error\":\"Unauthorized\",\"message\":\"Invalid or missing API key\"}");
  Serial.println("⚠️ Truy cập trái phép!");
}

// ============ PROTOTYPE HÀM ============
void setupWebServer();
void sendACCommand();
void readSensors();
void handleManualButton();
void receiveIR();

// ============ SETUP ============
void setup()
{
  Serial.begin(115200);
  Serial.println("\n=== HỆ THỐNG ĐIỀU HÒA BẢO MẬT ===");

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

  setupWebServer();

  Serial.println("✓ Hệ thống sẵn sàng!");
  Serial.println("🔐 BẢO MẬT: Yêu cầu API Key cho mọi request");
  Serial.printf("📝 API Key: %s\n", API_KEY);
  digitalWrite(LED_STATUS, LOW);
}

// ============ SETUP WEBSERVER VỚI BẢO MẬT ============
void setupWebServer()
{

  // ========== ENDPOINT CÔNG KHAI (không cần auth) ==========

  // Root - Health check
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "application/json",
                            "{\"message\":\"AC Control API\",\"version\":\"2.0\",\"security\":\"API Key Required\"}"); });

  // ========== ENDPOINT CẦN XÁC THỰC ==========

  // Đọc cảm biến - YÊU CẦU AUTH
  server.on("/sensors", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      sendUnauthorized(request);
      return;
    }
    
    StaticJsonDocument<200> doc;
    doc["temperature"] = temperature;
    doc["humidity"] = humidity;
    doc["timestamp"] = millis();
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  // Lấy trạng thái AC - YÊU CẦU AUTH
  server.on("/ac/status", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      sendUnauthorized(request);
      return;
    }
    
    StaticJsonDocument<200> doc;
    doc["status"] = acStatus;
    doc["temp"] = acTemp;
    doc["mode"] = acMode;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  // Điều khiển AC - YÊU CẦU AUTH
  server.on("/ac/command", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
            {
      if (!authenticateRequest(request)) {
        sendUnauthorized(request);
        return;
      }
      
      StaticJsonDocument<200> doc;
      DeserializationError error = deserializeJson(doc, (const char*)data);
      
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
        responseDoc["success"] = true;
        responseDoc["status"] = acStatus;
        responseDoc["temp"] = acTemp;
        responseDoc["mode"] = acMode;
        String response;
        serializeJson(responseDoc, response);
        request->send(200, "application/json", response);
      } else {
        request->send(400, "application/json", "{\"error\":\"Missing status field\"}");
      } });

  // Bật AC - YÊU CẦU AUTH
  server.on("/ac/on", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
            {
      if (!authenticateRequest(request)) {
        sendUnauthorized(request);
        return;
      }
      
      acStatus = true;
      
      // Parse optional temp/mode
      if (len > 0) {
        StaticJsonDocument<200> doc;
        deserializeJson(doc, (const char*)data);
        if (doc.containsKey("temp")) acTemp = doc["temp"];
        if (doc.containsKey("mode")) acMode = doc["mode"].as<String>();
      }
      
      sendACCommand();
      
      StaticJsonDocument<200> responseDoc;
      responseDoc["success"] = true;
      responseDoc["message"] = "AC turned ON";
      responseDoc["temp"] = acTemp;
      responseDoc["mode"] = acMode;
      String response;
      serializeJson(responseDoc, response);
      request->send(200, "application/json", response); });

  // Tắt AC - YÊU CẦU AUTH
  server.on("/ac/off", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      sendUnauthorized(request);
      return;
    }
    
    acStatus = false;
    sendACCommand();
    
    request->send(200, "application/json", 
      "{\"success\":true,\"message\":\"AC turned OFF\"}"); });

  // Nút thủ công - YÊU CẦU AUTH
  server.on("/manual/button", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      sendUnauthorized(request);
      return;
    }
    
    acStatus = !acStatus;
    sendACCommand();
    
    StaticJsonDocument<200> doc;
    doc["success"] = true;
    doc["status"] = acStatus;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  // Gửi IR - YÊU CẦU AUTH
  server.on("/ir/send", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
            {
      if (!authenticateRequest(request)) {
        sendUnauthorized(request);
        return;
      }
      
      StaticJsonDocument<200> doc;
      DeserializationError error = deserializeJson(doc, (const char*)data);
      
      if (error || !doc.containsKey("code")) {
        request->send(400, "application/json", "{\"error\":\"Invalid request\"}");
        return;
      }

      uint32_t code = doc["code"];
      
      if (code == 1) {
        acStatus = true;
        sendACCommand();
        request->send(200, "application/json", "{\"success\":true,\"message\":\"AC ON\"}");
      } else if (code == 2) {
        acStatus = false;
        sendACCommand();
        request->send(200, "application/json", "{\"success\":true,\"message\":\"AC OFF\"}");
      } else {
        request->send(200, "application/json", "{\"success\":true,\"message\":\"IR sent\"}");
      } });

  // XỬ LÝ CORS PREFLIGHT (OPTIONS request)
  server.on("/sensors", HTTP_OPTIONS, [](AsyncWebServerRequest *request)
            { request->send(200); });

  server.on("/ac/status", HTTP_OPTIONS, [](AsyncWebServerRequest *request)
            { request->send(200); });

  server.on("/ac/command", HTTP_OPTIONS, [](AsyncWebServerRequest *request)
            { request->send(200); });

  server.on("/ac/on", HTTP_OPTIONS, [](AsyncWebServerRequest *request)
            { request->send(200); });

  server.on("/ac/off", HTTP_OPTIONS, [](AsyncWebServerRequest *request)
            { request->send(200); });

  server.on("/manual/button", HTTP_OPTIONS, [](AsyncWebServerRequest *request)
            { request->send(200); });

  server.on("/ir/send", HTTP_OPTIONS, [](AsyncWebServerRequest *request)
            { request->send(200); });

  // CORS headers (nếu cần gọi từ web app)
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");

  server.begin();
  Serial.println("✓ Secure WebServer đã khởi tạo!");
}

// ============ GỬI LỆNH IR ============
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
}

// ============ XỬ LÝ NÚT BẤM ============
void handleManualButton()
{
  int currentButtonState = digitalRead(BTN_MANUAL);

  if (lastButtonState == HIGH && currentButtonState == LOW)
  {
    if (millis() - lastButtonPress > buttonCooldown)
    {
      Serial.println("✅ Nhấn nút thủ công!");
      acStatus = !acStatus;
      sendACCommand();
      lastButtonPress = millis();
    }
  }

  lastButtonState = currentButtonState;
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
      acStatus = true;
      sendACCommand();
    }
    else if (code == 2)
    {
      acStatus = false;
      sendACCommand();
    }

    irrecv.resume();
  }
}

// ============ LOOP ============
void loop()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    digitalWrite(LED_WIFI, LOW);
    Serial.println("✗ Mất kết nối WiFi!");
    while (WiFi.status() != WL_CONNECTED)
      delay(500);
    digitalWrite(LED_WIFI, HIGH);
    Serial.println("✓ Đã kết nối WiFi lại!");
  }

  if (millis() - lastSensorRead > sensorInterval)
  {
    lastSensorRead = millis();
    readSensors();
  }

  handleManualButton();
  receiveIR();
}