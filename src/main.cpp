#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRrecv.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// ============ CẤU HÌNH CHÂN ============
#define DHT_PIN 4
#define IR_SEND_PIN 5
#define IR_RECV_PIN 18
#define PIR_PIN 25
#define LDR_PIN 34
#define LED_STATUS 2
#define LED_WIFI 15
#define LED_ERROR 13
#define LED_LLM 14
#define BTN_MANUAL 19
#define BTN_LLM 27
#define RELAY_PIN 23
#define BUZZER_PIN 12

// ============ CẤU HÌNH WIFI ============
#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASSWORD ""

// ============ CẤU HÌNH LLM API ============
#define LLM_API_URL "http://172.16.0.2:5000/llm/query"
#define API_KEY "AC_SECRET_KEY_2024_LLM"

// ============ KHỞI TẠO THIẾT BỊ ============
#define DHT_TYPE DHT22
DHT dht(DHT_PIN, DHT_TYPE);
IRsend irsend(IR_SEND_PIN);
IRrecv irrecv(IR_RECV_PIN);
decode_results results;
LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS1307 rtc;
AsyncWebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600);

// ============ BIẾN TOÀN CỤC ============
float temperature = 0;
float humidity = 0;
int lightLevel = 0;
bool motionDetected = false;
unsigned long lastMotionTime = 0;
DateTime now;

bool acStatus = false;
int acTemp = 25;
String acMode = "COOL";
int acFan = 2;
bool relayStatus = false;

bool llmEnabled = true;
bool llmProcessing = false;
String lastLLMResponse = "";
unsigned long lastLLMRequest = 0;
// ĐÃ BỎ COOLDOWN - set về 0
const unsigned long llmCooldown = 0; // ← BỎ COOLDOWN
bool useMockLLM = false;

unsigned long totalRequests = 0;
unsigned long llmRequests = 0;
unsigned long lastSensorRead = 0;
const long sensorInterval = 2000;

// ============ LOG SYSTEM ============
struct LogEntry
{
  unsigned long timestamp;
  String level;
  String message;
};

#define MAX_LOGS 50
LogEntry logBuffer[MAX_LOGS];
int logIndex = 0;
int logCount = 0;

void addLog(String level, String message)
{
  logBuffer[logIndex] = {millis(), level, message};
  logIndex = (logIndex + 1) % MAX_LOGS;
  if (logCount < MAX_LOGS)
    logCount++;
  Serial.printf("[%s] %s\n", level.c_str(), message.c_str());
}

// ============ KHAI BÁO PROTOTYPE ============
void updateLCD();
void processLLMDecision(String llmResponse);

// ============ HÀM TIỆN ÍCH ============
void beep(int duration = 100, int times = 1)
{
  for (int i = 0; i < times; i++)
  {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(duration);
    digitalWrite(BUZZER_PIN, LOW);
    if (times > 1)
      delay(100);
  }
}

void blinkLED(int pin, int times = 1)
{
  for (int i = 0; i < times; i++)
  {
    digitalWrite(pin, HIGH);
    delay(100);
    digitalWrite(pin, LOW);
    delay(100);
  }
}

void reportError(String errorMsg, int blinkCount = 3)
{
  addLog("ERROR", errorMsg);
  blinkLED(LED_ERROR, blinkCount);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("ERROR!");
  lcd.setCursor(0, 1);
  lcd.print(errorMsg.substring(0, 16));
  delay(2000);
  updateLCD();
}

// ============ ĐỌC CẢM BIẾN ============
void readSensors()
{
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(h) || isnan(t))
  {
    reportError("DHT22 read fail", 4);
    return;
  }

  temperature = t;
  humidity = h;
  lightLevel = analogRead(LDR_PIN);

  if (digitalRead(PIR_PIN) == HIGH)
  {
    motionDetected = true;
    lastMotionTime = millis();
  }
  else if (millis() - lastMotionTime > 5000)
  {
    motionDetected = false;
  }

  now = rtc.now();
  addLog("INFO", "T=" + String(temperature, 1) + "C H=" + String(humidity, 0) + "%");
}

// ============ CẬP NHẬT LCD ============
void updateLCD()
{
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("T:");
  lcd.print(temperature, 1);
  lcd.print("C H:");
  lcd.print(humidity, 0);
  lcd.print("%");

  if (motionDetected)
  {
    lcd.setCursor(15, 0);
    lcd.write(0xFF);
  }

  lcd.setCursor(0, 1);
  if (acStatus)
  {
    lcd.print("AC:");
    lcd.print(acTemp);
    lcd.print("C ");
    lcd.print(acMode.substring(0, 3));
    if (llmEnabled)
    {
      lcd.setCursor(13, 1);
      lcd.print("AI");
    }
  }
  else
  {
    lcd.print("OFF ");
    lcd.printf("%02d:%02d", now.hour(), now.minute());
  }
}

// ============ GỬI LỆNH IR & RELAY ============
void sendACCommand()
{
  addLog("INFO", "AC: " + String(acStatus ? "ON" : "OFF") + " " + String(acTemp) + "C");

  uint32_t codeOn = 0x00020906;
  uint32_t codeOff = 0x00029069;

  if (acStatus)
  {
    irsend.sendNEC(codeOn, 32);
    digitalWrite(LED_STATUS, HIGH);
    digitalWrite(RELAY_PIN, HIGH);
    relayStatus = true;
    beep(100, 1);
  }
  else
  {
    irsend.sendNEC(codeOff, 32);
    digitalWrite(LED_STATUS, LOW);
    digitalWrite(RELAY_PIN, LOW);
    relayStatus = false;
    beep(50, 2);
  }

  updateLCD();
}

// ============ MOCK LLM ============
String mockLLM(String userMessage)
{
  addLog("INFO", "Mock LLM");

  String action = "maintain";
  int targetTemp = 25;
  String reason = "";

  if (temperature > 28 && !acStatus)
  {
    action = "turn_on";
    targetTemp = 24;
    reason = "Nhiệt độ cao";
  }
  else if (temperature < 23 && acStatus)
  {
    action = "turn_off";
    reason = "Đủ mát";
  }
  else if (temperature > 26 && motionDetected)
  {
    action = "turn_on";
    targetTemp = 25;
    reason = "Có người";
  }
  else if (!motionDetected && acStatus && (millis() - lastMotionTime > 300000))
  {
    action = "turn_off";
    reason = "Không người";
  }

  DynamicJsonDocument doc(512);
  doc["action"] = action;
  doc["temperature"] = targetTemp;
  doc["fan_speed"] = (temperature > 29) ? 3 : 2;
  doc["mode"] = (humidity > 70) ? "DRY" : "COOL";
  doc["reason"] = reason.length() > 0 ? reason : "Maintain";

  String response;
  serializeJson(doc, response);
  addLog("SUCCESS", "Mock: " + action);
  return response;
}

// ============ GỌI LLM API ============
String callLLM(String userMessage)
{
  if (!llmEnabled)
  {
    addLog("WARN", "LLM disabled");
    return "{\"error\":\"disabled\"}";
  }

  if (llmProcessing)
  {
    addLog("WARN", "LLM busy");
    return "{\"error\":\"busy\"}";
  }

  // ĐÃ BỎ CHECK COOLDOWN - cho phép gọi liên tục

  llmProcessing = true;
  digitalWrite(LED_LLM, HIGH);

  String response;

  if (useMockLLM)
  {
    delay(500);
    response = mockLLM(userMessage);
    llmRequests++;
  }
  else
  {
    if (WiFi.status() != WL_CONNECTED)
    {
      addLog("ERROR", "No WiFi");
      llmProcessing = false;
      digitalWrite(LED_LLM, LOW);
      return "{\"error\":\"wifi\"}";
    }

    addLog("INFO", "Call LLM API");

    HTTPClient http;
    String url = String(LLM_API_URL) + "?api_key=" + API_KEY;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(20000);

    // Context ngắn gọn
    String context = "T:" + String(temperature, 1) +
                     "C H:" + String(humidity, 0) +
                     "% L:" + String(lightLevel) +
                     " M:" + String(motionDetected ? "Y" : "N") +
                     " AC:" + String(acStatus ? "ON" : "OFF") +
                     String(acTemp) + "C";

    // Body đơn giản
    DynamicJsonDocument requestDoc(512);
    requestDoc["query"] = context + ". " + userMessage;

    String requestBody;
    serializeJson(requestDoc, requestBody);

    addLog("DEBUG", "Body: " + String(requestBody.length()) + "B");

    int httpCode = http.POST(requestBody);
    addLog("INFO", "HTTP: " + String(httpCode));

    if (httpCode == 200)
    {
      response = http.getString();
      addLog("INFO", "Got " + String(response.length()) + "B");

      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, response);

      if (error)
      {
        addLog("ERROR", "Parse: " + String(error.c_str()));
        response = "{\"error\":\"parse\"}";
      }
      else if (doc.containsKey("action"))
      {
        llmRequests++;
        addLog("SUCCESS", "Action: " + doc["action"].as<String>());
      }
      else if (doc.containsKey("raw_text"))
      {
        addLog("WARN", "Text only");
        response = doc["raw_text"].as<String>();
      }
      else if (doc.containsKey("error"))
      {
        addLog("ERROR", "API: " + doc["error"].as<String>());
      }
    }
    else if (httpCode > 0)
    {
      addLog("ERROR", "HTTP " + String(httpCode));
      response = "{\"error\":\"http\"}";
    }
    else
    {
      addLog("ERROR", "Connect fail");
      response = "{\"error\":\"connect\"}";
    }

    http.end();
  }

  llmProcessing = false;
  digitalWrite(LED_LLM, LOW);
  lastLLMRequest = millis();
  return response;
}

// ============ XỬ LÝ QUYẾT ĐỊNH LLM ============
void processLLMDecision(String llmResponse)
{
  addLog("INFO", "Process LLM...");

  // Kiểm tra error trước
  if (llmResponse.indexOf("\"error\"") != -1)
  {
    addLog("ERROR", "LLM returned error");
    return;
  }

  int jsonStart = llmResponse.indexOf('{');
  int jsonEnd = llmResponse.lastIndexOf('}');

  if (jsonStart == -1 || jsonEnd == -1)
  {
    addLog("ERROR", "No JSON");
    return;
  }

  String jsonStr = llmResponse.substring(jsonStart, jsonEnd + 1);
  addLog("DEBUG", "JSON: " + jsonStr.substring(0, 50) + "...");

  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, jsonStr);

  if (error)
  {
    addLog("ERROR", "Parse: " + String(error.c_str()));
    return;
  }

  String action = doc["action"] | "maintain";
  addLog("INFO", "Action: " + action);

  if (action == "turn_on")
  {
    acStatus = true;
    acTemp = constrain(doc["temperature"] | 25, 16, 30);
    acFan = constrain(doc["fan_speed"] | 2, 1, 3);
    acMode = doc["mode"] | "COOL";
    sendACCommand();
    addLog("SUCCESS", "AC ON " + String(acTemp) + "C");
  }
  else if (action == "turn_off")
  {
    acStatus = false;
    sendACCommand();
    addLog("SUCCESS", "AC OFF");
  }
  else if (action == "adjust")
  {
    if (acStatus)
    {
      acTemp = constrain(doc["temperature"] | acTemp, 16, 30);
      acFan = constrain(doc["fan_speed"] | acFan, 1, 3);
      acMode = doc["mode"] | acMode;
      sendACCommand();
      addLog("SUCCESS", "AC adj " + String(acTemp) + "C");
    }
    else
    {
      addLog("WARN", "Cannot adjust - AC OFF");
    }
  }
  else
  {
    addLog("INFO", "Maintain - no change");
  }

  String reason = doc["reason"] | "No reason";
  addLog("INFO", "Why: " + reason.substring(0, 30));
}

// ============ TỰ ĐỘNG TỐI ƯU ============
void autoLLMOptimize()
{
  static unsigned long lastCheck = 0;
  static float lastTemp = 0;
  static bool lastMotion = false;

  // Giảm thời gian check xuống 60s thay vì 300s
  if (millis() - lastCheck < 60000)
    return;

  bool trigger = false;

  // Trigger nếu nhiệt độ thay đổi > 1.5°C
  if (abs(temperature - lastTemp) > 1.5)
  {
    addLog("INFO", "Temp changed: " + String(lastTemp) + " -> " + String(temperature));
    trigger = true;
  }

  // Trigger nếu phát hiện/mất người
  if (motionDetected != lastMotion)
  {
    addLog("INFO", "Motion changed: " + String(lastMotion) + " -> " + String(motionDetected));
    trigger = true;
  }

  if (trigger && llmEnabled)
  {
    addLog("INFO", "Auto LLM triggered");
    String response = callLLM("Analyze and optimize AC based on current conditions");

    // ĐẢM BẢO XỬ LÝ RESPONSE
    if (response.length() > 0 && response.indexOf("error") == -1)
    {
      processLLMDecision(response);
    }
    else
    {
      addLog("ERROR", "Auto LLM failed");
    }

    lastTemp = temperature;
    lastMotion = motionDetected;
  }

  lastCheck = millis();
}

// ============ XỬ LÝ NÚT BẤM ============
void handleButtons()
{
  static bool lastManual = HIGH;
  static bool lastLLM = HIGH;
  static unsigned long lastPress = 0;

  if (millis() - lastPress < 500)
    return;

  bool currentManual = digitalRead(BTN_MANUAL);
  bool currentLLM = digitalRead(BTN_LLM);

  if (lastManual == HIGH && currentManual == LOW)
  {
    acStatus = !acStatus;
    sendACCommand();
    lastPress = millis();
    addLog("INFO", "Manual btn");
  }

  if (lastLLM == HIGH && currentLLM == LOW)
  {
    addLog("INFO", "LLM btn pressed");
    String response = callLLM("User pressed button - please optimize AC");

    // ĐẢM BẢO XỬ LÝ RESPONSE
    if (response.length() > 0 && response.indexOf("error") == -1)
    {
      processLLMDecision(response);
    }
    else
    {
      addLog("ERROR", "LLM button failed");
    }

    lastPress = millis();
  }

  lastManual = currentManual;
  lastLLM = currentLLM;
}

// ============ NHẬN IR ============
void receiveIR()
{
  if (irrecv.decode(&results))
  {
    addLog("INFO", "IR: 0x" + String(results.value, HEX));

    if (results.value == 0x00020906)
    {
      acStatus = true;
      sendACCommand();
    }
    else if (results.value == 0x40bf906f)
    {
      acStatus = false;
      sendACCommand();
    }

    irrecv.resume();
  }
}

// ============ XÁC THỰC ============
bool authenticateRequest(AsyncWebServerRequest *request)
{
  if (request->hasHeader("Authorization"))
  {
    String auth = request->header("Authorization");
    if (auth.startsWith("Bearer "))
      auth = auth.substring(7);
    if (auth == API_KEY)
      return true;
  }

  if (request->hasParam("api_key"))
  {
    if (request->getParam("api_key")->value() == API_KEY)
      return true;
  }

  addLog("WARN", "Unauth");
  return false;
}

// ============ SETUP WEBSERVER ============
void setupWebServer()
{
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { request->send(200, "application/json",
                            "{\"name\":\"AC Control\",\"version\":\"4.3\",\"status\":\"ok\"}"); });

  server.on("/sensors", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    DynamicJsonDocument doc(512);
    doc["temperature"] = temperature;
    doc["humidity"] = humidity;
    doc["light"] = lightLevel;
    doc["motion"] = motionDetected;
    doc["ac_status"] = acStatus;
    doc["ac_temp"] = acTemp;
    doc["llm_enabled"] = llmEnabled;
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  server.on("/llm/query", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
            {
      if (!authenticateRequest(request)) {
        request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
      }
      
      DynamicJsonDocument doc(512);
      deserializeJson(doc, (const char*)data);
      String query = doc["query"] | "Analyze";
      
      addLog("INFO", "POST /llm/query");
      String response = callLLM(query);
      
      // TỰ ĐỘNG XỬ LÝ QUYẾT ĐỊNH
      if (response.length() > 0 && response.indexOf("error") == -1)
      {
        processLLMDecision(response);
      }
      
      request->send(200, "application/json", response); });

  server.on("/llm/control", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }

    addLog("INFO", "POST /llm/control");
    String response = callLLM("Optimize AC settings based on current conditions");
    
    // ĐẢM BẢO XỬ LÝ QUYẾT ĐỊNH
    bool success = false;
    if (response.length() > 0 && response.indexOf("error") == -1)
    {
      processLLMDecision(response);
      success = true;
    }

    request->send(200, "application/json", 
      "{\"success\":" + String(success ? "true" : "false") + 
      ",\"response\":" + response + "}"); });

  server.on("/llm/toggle", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    llmEnabled = !llmEnabled;
    addLog("INFO", "LLM: " + String(llmEnabled ? "ON" : "OFF"));
    digitalWrite(LED_LLM, llmEnabled ? HIGH : LOW);
    request->send(200, "application/json", 
      "{\"llm\":\"" + String(llmEnabled ? "on" : "off") + "\"}"); });

  server.on("/llm/mode", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    request->send(200, "application/json", 
      "{\"mode\":\"" + String(useMockLLM ? "mock" : "real") + "\"}"); });

  server.on("/stats", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    DynamicJsonDocument doc(512);
    doc["uptime"] = millis() / 1000;
    doc["llm_requests"] = llmRequests;
    doc["temperature"] = temperature;
    doc["ac_status"] = acStatus;
    doc["free_heap"] = ESP.getFreeHeap();
    doc["llm_enabled"] = llmEnabled;
    doc["llm_processing"] = llmProcessing;
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  server.on("/logs", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    DynamicJsonDocument doc(4096);
    JsonArray logs = doc.createNestedArray("logs");
    
    for (int i = 0; i < logCount; i++) {
      int idx = (logIndex - logCount + i + MAX_LOGS) % MAX_LOGS;
      JsonObject log = logs.createNestedObject();
      log["time"] = logBuffer[idx].timestamp;
      log["level"] = logBuffer[idx].level;
      log["msg"] = logBuffer[idx].message;
    }
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  server.on("/ac/command", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
            {
      if (!authenticateRequest(request)) {
        request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
      }
      
      DynamicJsonDocument doc(300);
      deserializeJson(doc, (const char*)data);
      
      if (doc.containsKey("status")) acStatus = doc["status"];
      if (doc.containsKey("temp")) acTemp = constrain(doc["temp"].as<int>(), 16, 30);
      
      sendACCommand();
      request->send(200, "application/json", "{\"success\":true}"); });

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  server.begin();
  addLog("SUCCESS", "WebServer OK");
}

// ============ SETUP ============
void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n╔═══════════════════════════╗");
  Serial.println("║  AC CONTROL v4.3 + LLM   ║");
  Serial.println("║    NO COOLDOWN FIXED     ║");
  Serial.println("╚═══════════════════════════╝");

  pinMode(LED_STATUS, OUTPUT);
  pinMode(LED_WIFI, OUTPUT);
  pinMode(LED_ERROR, OUTPUT);
  pinMode(LED_LLM, OUTPUT);
  pinMode(BTN_MANUAL, INPUT_PULLUP);
  pinMode(BTN_LLM, INPUT_PULLUP);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(PIR_PIN, INPUT);
  pinMode(LDR_PIN, INPUT);

  digitalWrite(LED_STATUS, LOW);
  digitalWrite(LED_WIFI, LOW);
  digitalWrite(LED_ERROR, LOW);
  digitalWrite(LED_LLM, LOW);
  digitalWrite(RELAY_PIN, LOW);

  addLog("INFO", "Starting...");

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("AC Control v4.3");
  lcd.setCursor(0, 1);
  lcd.print("Init...");
  beep(100, 1);

  if (!rtc.begin())
  {
    addLog("ERROR", "RTC fail");
  }
  else if (!rtc.isrunning())
  {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  dht.begin();
  delay(2000);

  irsend.begin();
  irrecv.enableIRIn();
  addLog("SUCCESS", "IR OK");

  lcd.clear();
  lcd.print("WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30)
  {
    delay(500);
    Serial.print(".");
    digitalWrite(LED_WIFI, !digitalRead(LED_WIFI));
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    digitalWrite(LED_WIFI, HIGH);
    addLog("SUCCESS", "WiFi: " + WiFi.localIP().toString());

    lcd.clear();
    lcd.print("WiFi OK!");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP());
    beep(100, 2);

    timeClient.begin();
    timeClient.update();
    rtc.adjust(DateTime(timeClient.getEpochTime()));
    delay(2000);
  }
  else
  {
    addLog("ERROR", "WiFi fail");
    digitalWrite(LED_ERROR, HIGH);
  }

  setupWebServer();

  lcd.clear();
  lcd.print("Testing...");
  readSensors();
  delay(1000);

  addLog("SUCCESS", "Ready!");
  Serial.println("✓ System Ready!");
  Serial.printf("🔐 API Key: %s\n", API_KEY);
  Serial.printf("🎭 Mode: %s\n", useMockLLM ? "MOCK" : "REAL");
  Serial.println("🚀 COOLDOWN: DISABLED");
  Serial.println("📡 Endpoints:");
  Serial.println("  /sensors - Sensor data");
  Serial.println("  /llm/query - Query LLM (auto process)");
  Serial.println("  /llm/control - Auto control");
  Serial.println("  /stats - Statistics");

  lcd.clear();
  lcd.print("Ready!");
  lcd.setCursor(0, 1);
  lcd.print("AI:");
  lcd.print(llmEnabled ? "ON" : "OFF");
  digitalWrite(LED_LLM, llmEnabled ? HIGH : LOW);
  beep(200, 1);
}

// ============ LOOP ============
void loop()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    digitalWrite(LED_WIFI, LOW);
    digitalWrite(LED_ERROR, HIGH);
    addLog("ERROR", "WiFi lost");
    WiFi.reconnect();
    delay(5000);
    return;
  }

  if (millis() - lastSensorRead > sensorInterval)
  {
    lastSensorRead = millis();
    readSensors();
    updateLCD();
  }

  handleButtons();
  receiveIR();

  if (llmEnabled)
  {
    autoLLMOptimize();
  }

  static unsigned long lastBlink = 0;
  if (llmEnabled && millis() - lastBlink > 2000)
  {
    digitalWrite(LED_LLM, !digitalRead(LED_LLM));
    lastBlink = millis();
  }

  delay(10);
}