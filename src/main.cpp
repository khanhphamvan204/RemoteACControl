#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

// ============ CẤU HÌNH CHÂN ============
#define DHT_PIN 4
#define IR_RECV_PIN 18
#define IR_SEND_PIN 17
#define PIR_PIN 25
#define RADAR_TRIG_PIN 23
#define RADAR_ECHO_PIN 26
#define LDR_PIN 34
#define LED_STATUS 2
#define LED_WIFI 15
#define LED_ERROR 13
#define LED_AI 14            // Đổi tên từ LED_LLM thành LED_AI
#define BTN_POWER 19         // Đổi tên từ BTN_MANUAL thành BTN_POWER
#define BTN_AI 27            // Đổi tên từ BTN_LLM thành BTN_AI
#define BTN_TEST_PRESENCE 33 // NÚT MỚI - Test presence cho Wokwi
#define BUZZER_PIN 12

// ============ MÃ IR REMOTE AC ============
#define IR_AC_POWER 0xff45ba
#define IR_AC_TEMP_UP 0xff40bf
#define IR_AC_TEMP_DOWN 0xff19e6
#define IR_AC_MODE 0xff07f8
#define IR_AC_FAN 0xff09f6

// ============ CẤU HÌNH WIFI ============
#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASSWORD ""

// ============ CẤU HÌNH LLM API ============
#define LLM_API_URL "http://172.16.0.2:5000/llm/query"
#define VOICE_API_URL "http://172.16.0.2:5000/voice/command"
#define API_KEY "AC_SECRET_KEY_2024_LLM_V5"

// ============ KHỞI TẠO THIẾT BỊ ============
#define DHT_TYPE DHT22
DHT dht(DHT_PIN, DHT_TYPE);
IRrecv irrecv(IR_RECV_PIN);
IRsend irsend(IR_SEND_PIN);
decode_results results;
LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS1307 rtc;
AsyncWebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600);

// ============ BIẾN CẢM BIẾN ============
float temperature = 0;
float humidity = 0;
int lightLevel = 0;
bool motionDetected = false;
bool presenceDetected = false;
float presenceDistance = 0;
unsigned long lastMotionTime = 0;
unsigned long lastPresenceTime = 0;
DateTime now;

// BIẾN TEST MODE CHO WOKWI
bool testPresenceMode = false; // Chế độ giả lập có người

// ============ BIẾN ĐIỀU KHIỂN AC ============
bool acStatus = false;
int acTemp = 25;
String acMode = "COOL";
int acFan = 2;

// ============ BIẾN LLM ============
bool llmEnabled = true;
bool llmProcessing = false;
String lastLLMResponse = "";
unsigned long lastLLMRequest = 0;

// ============ IR HUB ============
struct LearnedIRCommand
{
  String name;
  uint64_t code;
  int type;
};

#define MAX_IR_COMMANDS 10
LearnedIRCommand learnedCommands[MAX_IR_COMMANDS];
int learnedCount = 0;
bool irLearningMode = false;

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

// ============ THỐNG KÊ ============
unsigned long totalRequests = 0;
unsigned long llmRequests = 0;
unsigned long irCommands = 0;
unsigned long autoOptimizations = 0;
unsigned long voiceCommands = 0;
unsigned long lastSensorRead = 0;
const long sensorInterval = 2000;

// ============ KHAI BÁO PROTOTYPE ============
void updateLCD();
void processLLMDecision(String llmResponse);
void sendACCommand(uint32_t code, String commandName);

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

  // NẾU ĐANG Ở CHẾ ĐỘ TEST, BỎ QUA ĐỌC CẢM BIẾN THẬT
  if (testPresenceMode)
  {
    // Giữ nguyên giá trị đã set bởi nút test
    now = rtc.now();
    addLog("INFO", "T=" + String(temperature, 1) + "C H=" + String(humidity, 0) +
                       "% TEST_MODE:ON");
    return;
  }

  // ĐỌC CẢM BIẾN BÌNH THƯỜNG KHI KHÔNG Ở CHẾ ĐỘ TEST
  // Đọc PIR
  if (digitalRead(PIR_PIN) == HIGH)
  {
    motionDetected = true;
    lastMotionTime = millis();
  }
  else if (millis() - lastMotionTime > 5000)
  {
    motionDetected = false;
  }

  // Đọc RADAR (HC-SR04)
  digitalWrite(RADAR_TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(RADAR_TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(RADAR_TRIG_PIN, LOW);
  long duration = pulseIn(RADAR_ECHO_PIN, HIGH, 30000);

  presenceDistance = (duration * 0.0343) / 2.0;

  if (presenceDistance > 1.0 && presenceDistance < 150.0)
  {
    presenceDetected = true;
    lastPresenceTime = millis();
  }
  else if (millis() - lastPresenceTime > 10000)
  {
    presenceDetected = false;
  }

  now = rtc.now();
  addLog("INFO", "T=" + String(temperature, 1) + "C H=" + String(humidity, 0) +
                     "% Dist=" + String(presenceDistance, 0) + "cm");
}

// ============ CẬP NHẬT LCD - THÊM HIỂN THỊ MỨC QUẠT ============
void updateLCD()
{
  lcd.clear();

  // DÒNG 1: Nhiệt độ + Độ ẩm + Presence indicator
  lcd.setCursor(0, 0);
  lcd.print("T:");
  lcd.print(temperature, 1);
  lcd.print("C H:");
  lcd.print(humidity, 0);
  lcd.print("%");

  if (presenceDetected)
  {
    lcd.setCursor(15, 0);
    lcd.write(testPresenceMode ? 'T' : 0xFF);
  }

  // DÒNG 2: Trạng thái AC
  lcd.setCursor(0, 1);
  if (acStatus)
  {
    // Khi AC BẬT: hiển thị "AC:24C C F2" hoặc "AC:26C D F3 AI"
    lcd.print("AC:");
    lcd.print(acTemp);
    lcd.print("C ");

    // Hiển thị chế độ (1 ký tự): C=COOL, D=DRY, F=FAN
    lcd.print(acMode.substring(0, 1));

    // Hiển thị mức quạt: F1, F2, F3
    lcd.print(" F");
    lcd.print(acFan);

    // Hiển thị AI ở cuối nếu còn chỗ
    if (llmEnabled)
    {
      lcd.setCursor(13, 1);
      lcd.print("AI");
    }
  }
  else
  {
    // Khi AC TẮT: hiển thị thời gian + trạng thái presence
    lcd.printf("%02d:%02d ", now.hour(), now.minute());
    lcd.print(presenceDetected ? "Presence" : "Empty   ");
  }
}

// ============ GỬI LỆNH AC ============
void sendACCommand(uint32_t code, String commandName)
{
  irsend.sendNEC(code, 32);
  irCommands++;
  addLog("INFO", "IR SEND: " + commandName + " (0x" + String(code, HEX) + ")");

  digitalWrite(LED_STATUS, acStatus);
  beep(acStatus ? 100 : 50, acStatus ? 1 : 2);
  updateLCD();
}

// ============ IR LEARNING MODE ============
void startIRLearning()
{
  irLearningMode = true;
  addLog("INFO", "IR Learning started");
  lcd.clear();
  lcd.print("IR Learn Mode");
  lcd.setCursor(0, 1);
  lcd.print("Press remote...");
  beep(100, 3);
}

void captureIRCommand()
{
  if (!irLearningMode)
    return;

  if (irrecv.decode(&results))
  {
    if (results.value != 0xFFFFFFFF && learnedCount < MAX_IR_COMMANDS)
    {
      learnedCommands[learnedCount].code = results.value;
      learnedCommands[learnedCount].type = results.decode_type;
      learnedCommands[learnedCount].name = "CMD_" + String(learnedCount);

      addLog("SUCCESS", "Learned IR: 0x" + String(results.value, HEX));

      lcd.clear();
      lcd.print("Learned #");
      lcd.print(learnedCount + 1);
      lcd.setCursor(0, 1);
      lcd.print("0x");
      lcd.print(String(results.value, HEX).substring(0, 12));

      learnedCount++;
      beep(200, 1);

      if (learnedCount >= MAX_IR_COMMANDS)
      {
        irLearningMode = false;
        addLog("INFO", "IR Learning complete");
        beep(100, 3);
      }
    }
    irrecv.resume();
  }
}

// ============ MOCK LLM - LOCAL AI ============
String mockLLM(String userMessage)
{
  addLog("INFO", "Mock LLM - Local AI Analysis");

  String action = "maintain";
  int targetTemp = 25;
  String reason = "";
  int fanSpeed = 2;
  String mode = "COOL";

  // Logic thông minh dựa trên cảm biến
  if (temperature > 28 && !acStatus && presenceDetected)
  {
    action = "turn_on";
    targetTemp = 24;
    fanSpeed = 3;
    reason = "Hot + person present";
  }
  else if (temperature < 23 && acStatus)
  {
    action = "turn_off";
    reason = "Cool enough";
  }
  else if (!presenceDetected && !motionDetected && acStatus)
  {
    if (millis() - lastPresenceTime > 300000) // 5 phút
    {
      action = "turn_off";
      reason = "No presence detected";
    }
  }
  else if (humidity > 70 && temperature > 26)
  {
    action = "turn_on";
    mode = "DRY";
    targetTemp = 26;
    reason = "High humidity";
  }
  else if (lightLevel > 3000 && now.hour() >= 22)
  {
    if (acStatus && acTemp < 26)
    {
      action = "adjust";
      targetTemp = 26;
      fanSpeed = 1;
      reason = "Night mode - save energy";
    }
  }
  else if (acStatus && abs(temperature - acTemp) > 2)
  {
    action = "adjust";
    if (temperature > acTemp + 2)
    {
      targetTemp = acTemp - 1;
      fanSpeed = 3;
      reason = "Not cool enough";
    }
    else
    {
      targetTemp = acTemp + 1;
      fanSpeed = 1;
      reason = "Too cold";
    }
  }

  DynamicJsonDocument doc(512);
  doc["action"] = action;
  doc["temperature"] = targetTemp;
  doc["fan_speed"] = fanSpeed;
  doc["mode"] = mode;
  doc["reason"] = reason.length() > 0 ? reason : "Maintain comfort";

  String response;
  serializeJson(doc, response);
  addLog("SUCCESS", "Local AI: " + action + " - " + reason);
  return response;
}

// ============ XỬ LÝ QUYẾT ĐỊNH LLM ============
void processLLMDecision(String llmResponse)
{
  addLog("INFO", "Process AI Decision...");

  if (llmResponse.indexOf("\"error\"") != -1)
  {
    addLog("ERROR", "AI returned error");
    return;
  }

  int jsonStart = llmResponse.indexOf('{');
  int jsonEnd = llmResponse.lastIndexOf('}');
  if (jsonStart == -1 || jsonEnd == -1)
  {
    addLog("ERROR", "No JSON in response");
    return;
  }

  String jsonStr = llmResponse.substring(jsonStart, jsonEnd + 1);
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
    sendACCommand(IR_AC_POWER, "AI_ON");
    addLog("SUCCESS", "AC ON " + String(acTemp) + "C");
    autoOptimizations++;
  }
  else if (action == "turn_off")
  {
    acStatus = false;
    sendACCommand(IR_AC_POWER, "AI_OFF");
    addLog("SUCCESS", "AC OFF");
    autoOptimizations++;
  }
  else if (action == "adjust")
  {
    if (acStatus)
    {
      int newTemp = constrain(doc["temperature"] | acTemp, 16, 30);
      acFan = constrain(doc["fan_speed"] | acFan, 1, 3);
      acMode = doc["mode"] | acMode;

      if (newTemp > acTemp)
        sendACCommand(IR_AC_TEMP_UP, "AI_TEMP+");
      else if (newTemp < acTemp)
        sendACCommand(IR_AC_TEMP_DOWN, "AI_TEMP-");

      acTemp = newTemp;
      addLog("SUCCESS", "AC adj " + String(acTemp) + "C");
      autoOptimizations++;
    }
  }

  String reason = doc["reason"] | "No reason";
  addLog("INFO", "Why: " + reason.substring(0, 30));
}

// ============ TỰ ĐỘNG TỐI ƯU - DÙNG LOCAL AI ============
void autoLLMOptimize()
{
  static unsigned long lastCheck = 0;
  static float lastTemp = 0;
  static bool lastPresence = false;
  static bool lastAcStatus = false;

  if (millis() - lastCheck < 15000)
    return;

  bool trigger = false;
  String triggerReason = "";

  // Trigger 1: Nhiệt độ thay đổi > 1.5°C
  if (abs(temperature - lastTemp) > 1.5)
  {
    triggerReason = "Temp change: " + String(lastTemp, 1) + " -> " + String(temperature, 1);
    addLog("INFO", triggerReason);
    trigger = true;
  }

  // Trigger 2: Presence thay đổi
  if (presenceDetected != lastPresence)
  {
    triggerReason = "Presence: " + String(lastPresence ? "Y" : "N") + " -> " + String(presenceDetected ? "Y" : "N");
    addLog("INFO", triggerReason);
    trigger = true;
  }

  // Trigger 3: Quá nóng mà AC chưa bật
  if (temperature > 28 && !acStatus && presenceDetected)
  {
    triggerReason = "Too hot: " + String(temperature, 1) + "C, AC OFF, person present";
    addLog("WARN", triggerReason);
    trigger = true;
  }

  // Trigger 4: Quá lạnh mà AC vẫn bật
  if (temperature < 23 && acStatus)
  {
    triggerReason = "Too cold: " + String(temperature, 1) + "C, AC ON";
    addLog("WARN", triggerReason);
    trigger = true;
  }

  // Trigger 5: Không có người > 5 phút mà AC vẫn bật
  if (!presenceDetected && !motionDetected && acStatus)
  {
    if (millis() - lastPresenceTime > 300000) // 5 phút
    {
      triggerReason = "No presence 5min, AC still ON";
      addLog("WARN", triggerReason);
      trigger = true;
    }
  }

  // Trigger 6: AC status thay đổi (bật/tắt thủ công)
  if (acStatus != lastAcStatus)
  {
    triggerReason = "AC manual change: " + String(lastAcStatus ? "ON" : "OFF") + " -> " + String(acStatus ? "ON" : "OFF");
    addLog("INFO", triggerReason);
    // Không trigger khi thay đổi thủ công, chỉ log
    lastAcStatus = acStatus;
  }

  if (trigger && llmEnabled)
  {
    // DÙNG LOCAL AI (mockLLM) thay vì gọi API
    addLog("INFO", "AI analyzing: " + triggerReason);
    String response = mockLLM("Auto optimize: " + triggerReason);
    if (response.length() > 0 && response.indexOf("error") == -1)
    {
      processLLMDecision(response);
    }
    lastTemp = temperature;
    lastPresence = presenceDetected;
    lastAcStatus = acStatus;
  }
  else if (!llmEnabled && trigger)
  {
    addLog("WARN", "AI disabled, skip optimization");
  }

  lastCheck = millis();
}

// ============ XỬ LÝ NÚT BẤM - THÊM NÚT THỨ 3 ============
void handleButtons()
{
  static bool lastPowerBtn = HIGH;
  static bool lastAIBtn = HIGH;
  static bool lastTestPresenceBtn = HIGH;
  static unsigned long lastPressTime = 0;

  // Chống dội phím
  if (millis() - lastPressTime < 500)
    return;

  bool currentPowerBtn = digitalRead(BTN_POWER);
  bool currentAIBtn = digitalRead(BTN_AI);
  bool currentTestPresenceBtn = digitalRead(BTN_TEST_PRESENCE);

  // NÚT 1: BẬT/TẮT ĐIỀU HÒA
  if (lastPowerBtn == HIGH && currentPowerBtn == LOW)
  {
    lastPressTime = millis();
    acStatus = !acStatus;

    if (acStatus)
    {
      addLog("INFO", "BTN: AC turned ON");
      beep(100, 1);
    }
    else
    {
      addLog("INFO", "BTN: AC turned OFF");
      beep(50, 2);
    }

    sendACCommand(IR_AC_POWER, "BTN_POWER");
    digitalWrite(LED_STATUS, acStatus);
    updateLCD();
  }

  // NÚT 2: BẬT/TẮT CHẾ ĐỘ AI
  if (lastAIBtn == HIGH && currentAIBtn == LOW)
  {
    lastPressTime = millis();
    llmEnabled = !llmEnabled;

    digitalWrite(LED_AI, llmEnabled);

    if (llmEnabled)
    {
      addLog("INFO", "BTN: AI Mode ENABLED");
      beep(100, 2);
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("AI Mode: ON");
      delay(1500);
    }
    else
    {
      addLog("INFO", "BTN: AI Mode DISABLED");
      beep(50, 3);
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("AI Mode: OFF");
      delay(1500);
    }

    updateLCD();
  }

  // NÚT 3: TEST PRESENCE - GIẢ LẬP CÓ NGƯỜI (CHO WOKWI)
  if (lastTestPresenceBtn == HIGH && currentTestPresenceBtn == LOW)
  {
    lastPressTime = millis();
    testPresenceMode = !testPresenceMode;

    if (testPresenceMode)
    {
      // Giả lập CÓ NGƯỜI
      presenceDetected = true;
      motionDetected = true;
      presenceDistance = 50.0; // 50cm - trong phạm vi phát hiện
      lastPresenceTime = millis();
      lastMotionTime = millis();

      addLog("INFO", "TEST: PRESENCE ON (Simulated)");
      beep(100, 3);

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("TEST: PRESENCE");
      lcd.setCursor(0, 1);
      lcd.print("Status: ON");
      delay(1500);
    }
    else
    {
      // Giả lập KHÔNG CÓ NGƯỜI
      presenceDetected = false;
      motionDetected = false;
      presenceDistance = 200.0; // Ngoài phạm vi

      addLog("INFO", "TEST: PRESENCE OFF (Simulated)");
      beep(50, 3);

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("TEST: PRESENCE");
      lcd.setCursor(0, 1);
      lcd.print("Status: OFF");
      delay(1500);
    }

    updateLCD();
  }

  lastPowerBtn = currentPowerBtn;
  lastAIBtn = currentAIBtn;
  lastTestPresenceBtn = currentTestPresenceBtn;
}

// ============ NHẬN IR ============
void receiveIR()
{
  if (irLearningMode)
  {
    captureIRCommand();
    return;
  }

  if (irrecv.decode(&results))
  {
    uint32_t irCode = results.value;
    if (irCode == 0xFFFFFFFF)
    {
      irrecv.resume();
      return;
    }

    addLog("INFO", "IR RECV: 0x" + String(irCode, HEX));
    irCommands++;

    switch (irCode)
    {
    case IR_AC_POWER:
      acStatus = !acStatus;
      digitalWrite(LED_STATUS, acStatus);
      updateLCD();
      beep(100, 1);
      break;
    case IR_AC_TEMP_UP:
      if (acStatus && acTemp < 30)
      {
        acTemp++;
        updateLCD();
        beep(50, 1);
      }
      break;
    case IR_AC_TEMP_DOWN:
      if (acStatus && acTemp > 16)
      {
        acTemp--;
        updateLCD();
        beep(50, 1);
      }
      break;
    case IR_AC_MODE:
      if (acStatus)
      {
        if (acMode == "COOL")
          acMode = "DRY";
        else if (acMode == "DRY")
          acMode = "FAN";
        else
          acMode = "COOL";
        updateLCD();
        beep(80, 1);
      }
      break;
    case IR_AC_FAN:
      if (acStatus)
      {
        acFan++;
        if (acFan > 3)
          acFan = 1;
        updateLCD();
        beep(60, 1);
      }
      break;
    default:
      for (int i = 0; i < learnedCount; i++)
      {
        if (learnedCommands[i].code == irCode)
        {
          addLog("INFO", "Learned cmd: " + learnedCommands[i].name);
          beep(100, 2);
          break;
        }
      }
      break;
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
  addLog("WARN", "Unauthorized");
  return false;
}
// ============ CORS HANDLER ============
void setupCORS()
{
  // Handle preflight OPTIONS request
  server.onNotFound([](AsyncWebServerRequest *request)
                    {
    if (request->method() == HTTP_OPTIONS) {
      request->send(200);
    } else {
      request->send(404, "application/json", "{\"error\":\"Not found\"}");
    } });
}
// ============ SETUP WEBSERVER ============
void setupWebServer()
{
  // Root endpoint
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            { 
              DynamicJsonDocument doc(512);
              doc["name"] = "AC Control System";
              doc["version"] = "5.2-Fixed";
              doc["status"] = "ok";
              doc["features"]["ai_mode"] = "local";
              doc["features"]["voice_control"] = true;
              doc["features"]["ir_hub"] = true;
              String response;
              serializeJson(doc, response);
              request->send(200, "application/json", response); });

  // GET sensor data
  server.on("/sensors", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    DynamicJsonDocument doc(768);
    doc["temperature"] = temperature;
    doc["humidity"] = humidity;
    doc["light"] = lightLevel;
    doc["motion"] = motionDetected;
    doc["presence"] = presenceDetected;
    doc["presence_distance"] = presenceDistance;
    doc["ac_status"] = acStatus;
    doc["ac_temp"] = acTemp;
    doc["ac_mode"] = acMode;
    doc["ac_fan"] = acFan;
    doc["llm_enabled"] = llmEnabled;
    doc["timestamp"] = millis();
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  // GET AC status
  server.on("/ac/status", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    DynamicJsonDocument doc(384);
    doc["status"] = acStatus ? "on" : "off";
    doc["temperature"] = acTemp;
    doc["mode"] = acMode;
    doc["fan_speed"] = acFan;
    doc["ai_enabled"] = llmEnabled;
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  // POST AC command (manual control)
  server.on("/ac/command", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
            {
      if (!authenticateRequest(request)) {
        request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
      }
      
      DynamicJsonDocument doc(300);
      deserializeJson(doc, (const char*)data);
      
      if (doc.containsKey("status")) {
        acStatus = doc["status"];
        sendACCommand(IR_AC_POWER, "API_POWER");
      }
      if (doc.containsKey("temp")) {
        acTemp = constrain(doc["temp"].as<int>(), 16, 30);
        sendACCommand(IR_AC_TEMP_UP, "API_TEMP");
      }
      if (doc.containsKey("mode")) {
        acMode = doc["mode"].as<String>();
        sendACCommand(IR_AC_MODE, "API_MODE");
      }
      if (doc.containsKey("fan")) {
        acFan = constrain(doc["fan"].as<int>(), 1, 3);
        sendACCommand(IR_AC_FAN, "API_FAN");
      }
      
      request->send(200, "application/json", "{\"success\":true}"); });

  // POST AI optimize (local AI)
  server.on("/ai/optimize", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    if (!llmEnabled) {
      request->send(400, "application/json", "{\"error\":\"AI disabled\"}");
      return;
    }
    
    String response = mockLLM("Optimize AC settings based on sensors");
    if (response.length() > 0 && response.indexOf("error") == -1) {
      processLLMDecision(response);
      request->send(200, "application/json", 
        "{\"success\":true,\"response\":" + response + "}");
    } else {
      request->send(500, "application/json", "{\"error\":\"AI failed\"}");
    } });

  // POST AI toggle - CẬP NHẬT
  server.on("/ai/toggle", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    totalRequests++;
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }

    llmEnabled = !llmEnabled;
    digitalWrite(LED_AI, llmEnabled);
    
    addLog("INFO", llmEnabled ? "AI Mode ENABLED via API" : "AI Mode DISABLED via API");
    
    String response = "{\"ai_mode\":\"" + String(llmEnabled ? "on" : "off") + "\",\"message\":\"AI mode toggled\"}";
    request->send(200, "application/json", response); });

  // GET AI status - CẬP NHẬT
  server.on("/ai/status", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    totalRequests++;
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }

    DynamicJsonDocument doc(256);
    doc["ai_enabled"] = llmEnabled;
    doc["ai_processing"] = llmProcessing;
    doc["last_response"] = lastLLMResponse.substring(0, 50);

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  // POST Voice command (gửi lên server Python để xử lý)
  server.on("/voice/command", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
            {
      if (!authenticateRequest(request)) {
        request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
      }
      
      DynamicJsonDocument inputDoc(512);
      deserializeJson(inputDoc, (const char*)data);
      
      String voiceText = inputDoc["text"] | "";
      if (voiceText.length() == 0) {
        request->send(400, "application/json", "{\"error\":\"Missing text\"}");
        return;
      }
      
      addLog("INFO", "Voice: " + voiceText.substring(0, 30));
      voiceCommands++;
      
      // Gửi lên Python server để xử lý
      if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        String url = String(VOICE_API_URL) + "?api_key=" + API_KEY;
        http.begin(url);
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(15000);
        
        DynamicJsonDocument sendDoc(512);
        sendDoc["text"] = voiceText;
        sendDoc["temperature"] = temperature;
        sendDoc["humidity"] = humidity;
        sendDoc["ac_status"] = acStatus;
        sendDoc["ac_temp"] = acTemp;
        
        String requestBody;
        serializeJson(sendDoc, requestBody);
        
        int httpCode = http.POST(requestBody);
        
        if (httpCode == 200) {
          String pythonResponse = http.getString();
          addLog("SUCCESS", "Voice processed");
          
          // Xử lý quyết định từ Python
          processLLMDecision(pythonResponse);
          
          request->send(200, "application/json", pythonResponse);
        } else {
          addLog("ERROR", "Voice HTTP " + String(httpCode));
          request->send(500, "application/json", 
            "{\"error\":\"Server error\"}");
        }
        http.end();
      } else {
        request->send(503, "application/json", "{\"error\":\"No WiFi\"}");
      } });

  // POST IR learning
  server.on("/ir/learn", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    startIRLearning();
    request->send(200, "application/json", "{\"status\":\"learning_started\"}"); });

  // GET IR commands
  server.on("/ir/commands", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    DynamicJsonDocument doc(1024);
    JsonArray arr = doc.createNestedArray("commands");
    for (int i = 0; i < learnedCount; i++) {
      JsonObject obj = arr.createNestedObject();
      obj["name"] = learnedCommands[i].name;
      obj["code"] = String(learnedCommands[i].code, HEX);
      obj["type"] = learnedCommands[i].type;
    }
    doc["count"] = learnedCount;
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  // GET statistics
  server.on("/stats", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    DynamicJsonDocument doc(768);
    doc["uptime"] = millis() / 1000;
    doc["llm_requests"] = llmRequests;
    doc["ir_commands"] = irCommands;
    doc["auto_optimizations"] = autoOptimizations;
    doc["voice_commands"] = voiceCommands;
    doc["temperature"] = temperature;
    doc["ac_status"] = acStatus;
    doc["ac_temp"] = acTemp;
    doc["free_heap"] = ESP.getFreeHeap();
    doc["ai_mode"] = "local";
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  // GET logs
  server.on("/logs", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    DynamicJsonDocument doc(4096);
    JsonArray arr = doc.createNestedArray("logs");
    for (int i = 0; i < logCount; i++) {
      int idx = (logIndex - logCount + i + MAX_LOGS) % MAX_LOGS;
      JsonObject obj = arr.createNestedObject();
      obj["timestamp"] = logBuffer[idx].timestamp;
      obj["level"] = logBuffer[idx].level;
      obj["message"] = logBuffer[idx].message;
    }
    doc["count"] = logCount;
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  // GET system info
  server.on("/system/info", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    DynamicJsonDocument doc(768);
    doc["version"] = "5.2-Fixed";
    doc["chip_model"] = "ESP32";
    doc["flash_size"] = ESP.getFlashChipSize();
    doc["free_heap"] = ESP.getFreeHeap();
    doc["wifi_rssi"] = WiFi.RSSI();
    doc["ip_address"] = WiFi.localIP().toString();
    doc["ai_mode"] = "local";
    doc["features"]["voice_control"] = true;
    doc["features"]["ir_hub"] = true;
    doc["features"]["auto_optimize"] = true;
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  // POST AC power (bật/tắt)
  server.on("/ac/power", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    acStatus = !acStatus;
    sendACCommand(IR_AC_POWER, acStatus ? "API_POWER_ON" : "API_POWER_OFF");
    
    DynamicJsonDocument doc(256);
    doc["success"] = true;
    doc["ac_status"] = acStatus ? "on" : "off";
    doc["temperature"] = acTemp;
    doc["mode"] = acMode;
    doc["fan"] = acFan;
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  // POST AC temperature up
  server.on("/ac/temp/up", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    if (!acStatus) {
      request->send(400, "application/json", "{\"error\":\"AC is OFF\"}");
      return;
    }
    
    if (acTemp >= 30) {
      request->send(400, "application/json", "{\"error\":\"Max temp reached\"}");
      return;
    }
    
    acTemp++;
    sendACCommand(IR_AC_TEMP_UP, "API_TEMP_UP");
    
    DynamicJsonDocument doc(256);
    doc["success"] = true;
    doc["temperature"] = acTemp;
    doc["message"] = "Temperature increased to " + String(acTemp) + "°C";
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  // POST AC temperature down
  server.on("/ac/temp/down", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    if (!acStatus) {
      request->send(400, "application/json", "{\"error\":\"AC is OFF\"}");
      return;
    }
    
    if (acTemp <= 16) {
      request->send(400, "application/json", "{\"error\":\"Min temp reached\"}");
      return;
    }
    
    acTemp--;
    sendACCommand(IR_AC_TEMP_DOWN, "API_TEMP_DOWN");
    
    DynamicJsonDocument doc(256);
    doc["success"] = true;
    doc["temperature"] = acTemp;
    doc["message"] = "Temperature decreased to " + String(acTemp) + "°C";
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  // POST AC temperature set (set trực tiếp)
  server.on("/ac/temp/set", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
            {
      if (!authenticateRequest(request)) {
        request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
      }
      
      if (!acStatus) {
        request->send(400, "application/json", "{\"error\":\"AC is OFF\"}");
        return;
      }
      
      DynamicJsonDocument doc(256);
      deserializeJson(doc, (const char*)data);
      
      if (!doc.containsKey("temperature")) {
        request->send(400, "application/json", "{\"error\":\"Missing temperature\"}");
        return;
      }
      
      int targetTemp = doc["temperature"];
      if (targetTemp < 16 || targetTemp > 30) {
        request->send(400, "application/json", "{\"error\":\"Temp must be 16-30°C\"}");
        return;
      }
      
      // Tăng/giảm nhiệt độ từ từ
      while (acTemp < targetTemp) {
        acTemp++;
        sendACCommand(IR_AC_TEMP_UP, "API_TEMP_SET_UP");
        delay(300);
      }
      while (acTemp > targetTemp) {
        acTemp--;
        sendACCommand(IR_AC_TEMP_DOWN, "API_TEMP_SET_DOWN");
        delay(300);
      }
      
      DynamicJsonDocument respDoc(256);
      respDoc["success"] = true;
      respDoc["temperature"] = acTemp;
      respDoc["message"] = "Temperature set to " + String(acTemp) + "°C";
      
      String response;
      serializeJson(respDoc, response);
      request->send(200, "application/json", response); });

  // POST AC mode (chuyển chế độ)
  server.on("/ac/mode", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    if (!acStatus) {
      request->send(400, "application/json", "{\"error\":\"AC is OFF\"}");
      return;
    }
    
    // Cycle: COOL -> DRY -> FAN -> COOL
    if (acMode == "COOL") {
      acMode = "DRY";
    } else if (acMode == "DRY") {
      acMode = "FAN";
    } else {
      acMode = "COOL";
    }
    
    sendACCommand(IR_AC_MODE, "API_MODE_CHANGE");
    
    DynamicJsonDocument doc(256);
    doc["success"] = true;
    doc["mode"] = acMode;
    doc["message"] = "Mode changed to " + acMode;
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  // POST AC mode set (set trực tiếp chế độ)
  server.on("/ac/mode/set", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
            {
      if (!authenticateRequest(request)) {
        request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
      }
      
      if (!acStatus) {
        request->send(400, "application/json", "{\"error\":\"AC is OFF\"}");
        return;
      }
      
      DynamicJsonDocument doc(256);
      deserializeJson(doc, (const char*)data);
      
      if (!doc.containsKey("mode")) {
        request->send(400, "application/json", "{\"error\":\"Missing mode\"}");
        return;
      }
      
      String targetMode = doc["mode"].as<String>();
      targetMode.toUpperCase();
      
      if (targetMode != "COOL" && targetMode != "DRY" && targetMode != "FAN") {
        request->send(400, "application/json", "{\"error\":\"Invalid mode. Use: COOL, DRY, FAN\"}");
        return;
      }
      
      // Chuyển mode cho đến khi đạt target
      int maxAttempts = 3;
      while (acMode != targetMode && maxAttempts > 0) {
        if (acMode == "COOL") acMode = "DRY";
        else if (acMode == "DRY") acMode = "FAN";
        else acMode = "COOL";
        
        sendACCommand(IR_AC_MODE, "API_MODE_SET");
        delay(300);
        maxAttempts--;
      }
      
      DynamicJsonDocument respDoc(256);
      respDoc["success"] = true;
      respDoc["mode"] = acMode;
      respDoc["message"] = "Mode set to " + acMode;
      
      String response;
      serializeJson(respDoc, response);
      request->send(200, "application/json", response); });

  // POST AC fan (chuyển mức quạt)
  server.on("/ac/fan", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    if (!acStatus) {
      request->send(400, "application/json", "{\"error\":\"AC is OFF\"}");
      return;
    }
    
    // Cycle: 1 -> 2 -> 3 -> 1
    acFan++;
    if (acFan > 3) acFan = 1;
    
    sendACCommand(IR_AC_FAN, "API_FAN_CHANGE");
    
    DynamicJsonDocument doc(256);
    doc["success"] = true;
    doc["fan_speed"] = acFan;
    doc["message"] = "Fan speed changed to " + String(acFan);
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  // POST AC fan set (set trực tiếp mức quạt)
  server.on("/ac/fan/set", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
            {
      if (!authenticateRequest(request)) {
        request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
        return;
      }
      
      if (!acStatus) {
        request->send(400, "application/json", "{\"error\":\"AC is OFF\"}");
        return;
      }
      
      DynamicJsonDocument doc(256);
      deserializeJson(doc, (const char*)data);
      
      if (!doc.containsKey("speed")) {
        request->send(400, "application/json", "{\"error\":\"Missing speed\"}");
        return;
      }
      
      int targetSpeed = doc["speed"];
      if (targetSpeed < 1 || targetSpeed > 3) {
        request->send(400, "application/json", "{\"error\":\"Speed must be 1-3\"}");
        return;
      }
      
      // Chuyển fan speed cho đến khi đạt target
      int maxAttempts = 3;
      while (acFan != targetSpeed && maxAttempts > 0) {
        acFan++;
        if (acFan > 3) acFan = 1;
        
        sendACCommand(IR_AC_FAN, "API_FAN_SET");
        delay(300);
        maxAttempts--;
      }
      
      DynamicJsonDocument respDoc(256);
      respDoc["success"] = true;
      respDoc["fan_speed"] = acFan;
      respDoc["message"] = "Fan speed set to " + String(acFan);
      
      String response;
      serializeJson(respDoc, response);
      request->send(200, "application/json", response); });

  // GET AC modes (danh sách các chế độ có sẵn)
  server.on("/ac/modes", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    DynamicJsonDocument doc(512);
    JsonArray modes = doc.createNestedArray("modes");
    modes.add("COOL");
    modes.add("DRY");
    modes.add("FAN");
    
    doc["current_mode"] = acMode;
    doc["ac_status"] = acStatus ? "on" : "off";
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  // GET AC fan speeds (danh sách mức quạt)
  server.on("/ac/fan/speeds", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!authenticateRequest(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    
    DynamicJsonDocument doc(512);
    JsonArray speeds = doc.createNestedArray("speeds");
    speeds.add(1);
    speeds.add(2);
    speeds.add(3);
    
    doc["current_speed"] = acFan;
    doc["ac_status"] = acStatus ? "on" : "off";
    
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response); });

  // Thêm CORS headers đầy đủ
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
  DefaultHeaders::Instance().addHeader("Access-Control-Max-Age", "86400");

  setupCORS(); // Gọi hàm xử lý CORS
  server.begin();
  addLog("SUCCESS", "WebServer v5.2-Fixed OK");
}

// ============ SETUP ============
void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n╔═══════════════════════════════╗");
  Serial.println("║  AC CONTROL v5.2 - TEST MODE  ║");
  Serial.println("║  • Local AI (Mock LLM)        ║");
  Serial.println("║  • Voice Control via Python   ║");
  Serial.println("║  • HC-SR04 Presence Detection ║");
  Serial.println("║  • IR Hub (Learn & Send)      ║");
  Serial.println("║  • TEST PRESENCE BUTTON       ║");
  Serial.println("╚═══════════════════════════════╝");

  // Setup các chân
  pinMode(LED_STATUS, OUTPUT);
  pinMode(LED_WIFI, OUTPUT);
  pinMode(LED_ERROR, OUTPUT);
  pinMode(LED_AI, OUTPUT);
  pinMode(BTN_POWER, INPUT_PULLUP);
  pinMode(BTN_AI, INPUT_PULLUP);
  pinMode(BTN_TEST_PRESENCE, INPUT_PULLUP); // NÚT MỚI
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(PIR_PIN, INPUT);
  pinMode(RADAR_TRIG_PIN, OUTPUT);
  pinMode(RADAR_ECHO_PIN, INPUT);

  // Khởi tạo LED
  digitalWrite(LED_STATUS, LOW);
  digitalWrite(LED_WIFI, LOW);
  digitalWrite(LED_ERROR, LOW);
  digitalWrite(LED_AI, llmEnabled);

  addLog("INFO", "Starting v5.2-TestMode...");

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("AC v5.2-TEST");
  lcd.setCursor(0, 1);
  lcd.print("3 Buttons Mode");
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

  irrecv.enableIRIn();
  irsend.begin();
  addLog("SUCCESS", "IR Receiver & Sender OK");

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
  lcd.print("Ready! v5.2");
  lcd.setCursor(0, 1);
  lcd.print("TEST:OFF AI:ON");
  digitalWrite(LED_AI, llmEnabled ? HIGH : LOW);
  beep(200, 1);

  Serial.println("\n🎮 BUTTONS:");
  Serial.println("  [RED]   BTN_POWER (D19)    - AC ON/OFF");
  Serial.println("  [BLUE]  BTN_AI (D27)       - AI Mode Toggle");
  Serial.println("  [GREEN] BTN_TEST (D33)     - Simulate Presence (Wokwi Test)");
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
    digitalWrite(LED_AI, !digitalRead(LED_AI));
    lastBlink = millis();
  }

  delay(10);
}