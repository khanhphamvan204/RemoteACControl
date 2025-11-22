#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <ir_Daikin.h>
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
#define BTN_POWER 19
#define BTN_AI 27
#define BTN_TEST_PRESENCE 33
#define BUZZER_PIN 12

// ============ CẤU HÌNH WIFI ============
#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASSWORD ""

// ============ CẤU HÌNH API ============
#define VOICE_API_URL "http://172.16.0.2:5000/voice/command"
#define API_KEY "AC_SECRET_KEY_2024_LLM_V5"

// ============ KHỞI TẠO THIẾT BỊ ============
#define DHT_TYPE DHT22
DHT dht(DHT_PIN, DHT_TYPE);
IRrecv irrecv(IR_RECV_PIN);
IRDaikinESP irsend(IR_SEND_PIN);
decode_results results;
LiquidCrystal_I2C lcd(0x27, 16, 2);
RTC_DS1307 rtc;
AsyncWebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600);

// ============ ENUM CHO TỐC ĐỘ QUẠT ============
enum FanSpeed
{
  FAN_QUIET = 1,
  FAN_LOW = 2,
  FAN_MEDIUM = 3,
  FAN_HIGH = 4,
  FAN_AUTO = 5
};

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
bool testPresenceMode = false;

// ============ BIẾN ĐIỀU KHIỂN AC DAIKIN ============
bool acStatus = false;
int acTemp = 25;
String acMode = "COOL";
FanSpeed acFan = FAN_MEDIUM;

// ============ BIẾN AI ============
bool aiEnabled = false;
bool aiProcessing = false;
String lastAIResponse = "";
unsigned long lastAIOptimization = 0;
const unsigned long AI_COOLDOWN = 5000; // GIẢM XUỐNG 5 GIÂY

// ============ LCD DISPLAY MODES ============
enum DisplayMode
{
  DISP_BASIC,     // T:25.5C H:60%
  DISP_AC_STATUS, // AC:24C COOL MED
  DISP_PRESENCE,  // Presence: 50cm
  DISP_AI_STATUS  // AI: Optimizing...
};
DisplayMode currentDisplayMode = DISP_BASIC;
unsigned long lastDisplayChange = 0;

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
unsigned long voiceCommands = 0;
unsigned long irCommands = 0;
unsigned long autoOptimizations = 0;
unsigned long lastSensorRead = 0;
const long sensorInterval = 2000;

// ============ KHAI BÁO PROTOTYPE ============
void updateLCD();
void sendDaikinCommand(String commandName);
String callVoiceAPI(String voiceText);
void processAIDecision(String aiResponse);
void mockLLMOptimize();

// ============ HÀM CHUYỂN ĐỔI FAN SPEED ============
String fanSpeedToString(FanSpeed speed)
{
  switch (speed)
  {
  case FAN_QUIET:
    return "QUIET";
  case FAN_LOW:
    return "LOW";
  case FAN_MEDIUM:
    return "MED";
  case FAN_HIGH:
    return "HIGH";
  case FAN_AUTO:
    return "AUTO";
  default:
    return "MED";
  }
}

FanSpeed stringToFanSpeed(String speedStr)
{
  speedStr.toUpperCase();
  if (speedStr == "QUIET" || speedStr == "1")
    return FAN_QUIET;
  if (speedStr == "LOW" || speedStr == "2")
    return FAN_LOW;
  if (speedStr == "MEDIUM" || speedStr == "MED" || speedStr == "3")
    return FAN_MEDIUM;
  if (speedStr == "HIGH" || speedStr == "4")
    return FAN_HIGH;
  if (speedStr == "AUTO" || speedStr == "5")
    return FAN_AUTO;
  return FAN_MEDIUM;
}

int fanSpeedToInt(FanSpeed speed)
{
  return static_cast<int>(speed);
}

FanSpeed intToFanSpeed(int speed)
{
  if (speed < 1)
    speed = 1;
  if (speed > 5)
    speed = 5;
  return static_cast<FanSpeed>(speed);
}

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

void reportError(String errorMsg, int blinkCount = 3)
{
  addLog("ERROR", errorMsg);
  beep(200, blinkCount);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("ERROR!");
  lcd.setCursor(0, 1);
  lcd.print(errorMsg.substring(0, 16));
  delay(2000);
  updateLCD();
}

// ============ ĐỌC CẢM BIẾN============
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

  // XỬ LÝ TEST PRESENCE MODE - VẪN CẬP NHẬT PRESENCE
  if (testPresenceMode)
  {
    presenceDetected = true; // BẮT BUỘC TRUE KHI TEST
    motionDetected = true;
    presenceDistance = 50.0;
    lastPresenceTime = millis();
    lastMotionTime = millis();
    now = rtc.now();
    addLog("INFO", "T=" + String(temperature, 1) + "C H=" + String(humidity, 0) + "% TEST_MODE:ON PRESENCE:FORCED");
    return; // Không đọc cảm biến thật
  }

  // ĐỌC CẢM BIẾN THẬT KHI KHÔNG TEST
  if (digitalRead(PIR_PIN) == HIGH)
  {
    motionDetected = true;
    lastMotionTime = millis();
  }
  else if (millis() - lastMotionTime > 5000)
  {
    motionDetected = false;
  }

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
                     "% Motion:" + String(motionDetected) + " Presence:" + String(presenceDetected) +
                     " Dist=" + String(presenceDistance, 0) + "cm");
}

// ============ CẬP NHẬT LCD (HIỂN THỊ LUÂN PHIÊN) ============
void updateLCD()
{
  lcd.clear();

  //  DÒNG 1: Luôn hiển thị nhiệt độ + độ ẩm
  lcd.setCursor(0, 0);
  lcd.print("T:");
  lcd.print(temperature, 1);
  lcd.print("C ");
  lcd.print("H:");
  lcd.print((int)humidity);
  lcd.print("%");

  // Hiển thị chỉ báo presence/test ở góc phải
  if (presenceDetected)
  {
    lcd.setCursor(14, 0);
    lcd.print(testPresenceMode ? "T" : "P");
  }

  if (aiEnabled)
  {
    lcd.setCursor(15, 0);
    lcd.print("*");
  }

  // DÒNG 2: Luân phiên hiển thị thông tin
  lcd.setCursor(0, 1);

  if (acStatus)
  {
    // KHI AC BẬT: Hiển thị nhiệt độ + mode + fan speed CHUẨN
    // Format: "AC:24C C QUI" hoặc "AC:24C C HI" (16 ký tự)
    lcd.print("AC:");
    lcd.print(acTemp);
    lcd.print("C ");

    // Mode: 1 ký tự
    lcd.print(acMode.substring(0, 1));
    lcd.print(" ");

    // FAN SPEED: Hiển thị tên rút gọn chuẩn
    String fanDisplay = "";
    switch (acFan)
    {
    case FAN_QUIET:
      fanDisplay = "QUI"; // QUIET
      break;
    case FAN_LOW:
      fanDisplay = "LOW";
      break;
    case FAN_MEDIUM:
      fanDisplay = "MED";
      break;
    case FAN_HIGH:
      fanDisplay = "HI ";
      break;
    case FAN_AUTO:
      fanDisplay = "AUT"; // AUTO
      break;
    default:
      fanDisplay = "MED";
    }
    lcd.print(fanDisplay);
  }
  else
  {
    // Khi AC tắt: luân phiên thông tin
    unsigned long now_ms = millis();
    int cycle = (now_ms / 3000) % 3; // Đổi mỗi 3 giây

    switch (cycle)
    {
    case 0: // Giờ + presence
      lcd.printf("%02d:%02d", now.hour(), now.minute());
      lcd.print(presenceDetected ? " PRESENT" : " EMPTY  ");
      break;

    case 1: // Khoảng cách
      if (presenceDetected)
      {
        lcd.print("Dist: ");
        lcd.print((int)presenceDistance);
        lcd.print("cm   ");
      }
      else
      {
        lcd.print("No presence    ");
      }
      break;

    case 2: // Ánh sáng
      lcd.print("Light: ");
      lcd.print(lightLevel);
      lcd.print("   ");
      break;
    }
  }
}

// ============ GỬI LỆNH DAIKIN ============
void sendDaikinCommand(String commandName)
{
  if (!acStatus)
  {
    irsend.off();
  }
  else
  {
    irsend.on();
    irsend.setTemp(acTemp);

    if (acMode == "COOL")
      irsend.setMode(kDaikinCool);
    else if (acMode == "HEAT")
      irsend.setMode(kDaikinHeat);
    else if (acMode == "DRY")
      irsend.setMode(kDaikinDry);
    else if (acMode == "FAN")
      irsend.setMode(kDaikinFan);
    else if (acMode == "AUTO")
      irsend.setMode(kDaikinAuto);

    switch (acFan)
    {
    case FAN_QUIET:
      irsend.setFan(kDaikinFanQuiet);
      break;
    case FAN_LOW:
      irsend.setFan(kDaikinFanMin);
      break;
    case FAN_MEDIUM:
      irsend.setFan(kDaikinFanMed);
      break;
    case FAN_HIGH:
      irsend.setFan(kDaikinFanMax);
      break;
    case FAN_AUTO:
      irsend.setFan(kDaikinFanAuto);
      break;
    }
  }

  irsend.send();
  irCommands++;

  addLog("INFO", "DAIKIN→ " + commandName + " | PWR:" + String(acStatus ? "ON" : "OFF") +
                     " T:" + String(acTemp) + "C M:" + acMode + " F:" + fanSpeedToString(acFan));

  beep(acStatus ? 100 : 50, acStatus ? 1 : 2);
  updateLCD();
}

// ============ MOCK LLM - TỰ ĐỘNG TỐI ƯU  ============
void mockLLMOptimize()
{
  static unsigned long lastCheck = 0;
  static float lastTemp = 0;
  static bool lastPresence = false;

  // Kiểm tra cooldown
  if (millis() - lastCheck < AI_COOLDOWN)
    return;

  // Kiểm tra AI có được bật không
  if (!aiEnabled)
  {
    lastCheck = millis();
    return;
  }

  if (aiProcessing)
    return;

  // BỎ KIỂM TRA TEST MODE - CHO AI CHẠY LUÔN
  // Test mode CHỈ giả lập cảm biến, không chặn AI

  bool trigger = false;
  String triggerReason = "";
  String action = "maintain";
  int targetTemp = acTemp;
  FanSpeed targetFan = acFan;
  String targetMode = acMode;
  String reason = "";

  addLog("AI", "[MOCK LLM] Analyzing... T=" + String(temperature, 1) + "C Presence:" + String(presenceDetected));

  // ===== RULE 1: Không có người - Tắt AC =====
  if (!presenceDetected && !motionDetected && acStatus)
  {
    if (millis() - lastPresenceTime > 10000) // GIẢM XUỐNG 10 GIÂY
    {
      trigger = true;
      action = "turn_off";
      reason = "No presence 1min";
      addLog("AI", "✓ Rule 1: No presence → Turn OFF");
    }
  }

  // ===== RULE 2: Quá nóng + Có người - Bật AC =====
  if (!trigger && temperature >= 29 && (presenceDetected || motionDetected) && !acStatus)
  {
    trigger = true;
    action = "turn_on";
    targetTemp = (temperature >= 31) ? 22 : 24;
    targetFan = (temperature >= 31) ? FAN_HIGH : FAN_MEDIUM;
    targetMode = "COOL";
    reason = "Very hot (" + String(temperature, 1) + "C)";
    addLog("AI", "✓ Rule 2: Too hot → Turn ON at " + String(targetTemp) + "C");
  }

  // ===== RULE 3: Hơi nóng + Có người - Bật AC =====
  if (!trigger && temperature >= 27 && (presenceDetected || motionDetected) && !acStatus)
  {
    trigger = true;
    action = "turn_on";
    targetTemp = 25;
    targetFan = FAN_MEDIUM;
    targetMode = "COOL";
    reason = "Hot (" + String(temperature, 1) + "C)";
    addLog("AI", "✓ Rule 3: Hot → Turn ON");
  }

  // ===== RULE 4: Quá lạnh - Tắt AC =====
  if (!trigger && temperature <= 22 && acStatus)
  {
    trigger = true;
    action = "turn_off";
    reason = "Too cold (" + String(temperature, 1) + "C)";
    addLog("AI", "✓ Rule 4: Too cold → Turn OFF");
  }

  // ===== RULE 5: Độ ẩm cao - Dùng DRY mode =====
  if (!trigger && humidity >= 75 && temperature >= 24 && temperature <= 28 && acStatus)
  {
    if (acMode != "DRY")
    {
      trigger = true;
      action = "adjust";
      targetTemp = 26;
      targetFan = FAN_MEDIUM;
      targetMode = "DRY";
      reason = "High humidity (" + String(humidity, 0) + "%)";
      addLog("AI", "✓ Rule 5: High humidity → DRY mode");
    }
  }

  // ===== RULE 6: Điều chỉnh nhiệt độ khi AC đang bật =====
  if (!trigger && acStatus && (presenceDetected || motionDetected))
  {
    // Quá nóng so với setting
    if (temperature > acTemp + 3)
    {
      trigger = true;
      action = "adjust";
      targetTemp = max(16, acTemp - 2);
      targetFan = FAN_HIGH;
      targetMode = acMode;
      reason = "Still warm, lowering";
      addLog("AI", "✓ Rule 6a: Too hot vs setting → Lower to " + String(targetTemp) + "C");
    }
    // Gần đạt nhiệt độ mục tiêu
    else if (abs(temperature - acTemp) <= 1 && acFan != FAN_LOW)
    {
      trigger = true;
      action = "adjust";
      targetTemp = acTemp;
      targetFan = FAN_LOW;
      targetMode = acMode;
      reason = "Near target, reduce";
      addLog("AI", "✓ Rule 6b: Near target → Reduce fan");
    }
  }

  // ===== RULE 7: Ban đêm → Chế độ QUIET =====
  if (!trigger && acStatus && (presenceDetected || motionDetected))
  {
    if ((now.hour() >= 22 || now.hour() <= 6) && acFan != FAN_QUIET && lightLevel > 2500)
    {
      trigger = true;
      action = "adjust";
      targetTemp = acTemp;
      targetFan = FAN_QUIET;
      targetMode = acMode;
      reason = "Night mode";
      addLog("AI", "✓ Rule 7: Night → QUIET mode");
    }
  }

  // ===== Thực hiện action =====
  if (trigger)
  {
    addLog("AI", "⚡ MOCK LLM → " + action + ": " + reason);

    if (action == "turn_off")
    {
      acStatus = false;
      sendDaikinCommand("AI_OFF");
      lastAIResponse = reason;
      autoOptimizations++;
    }
    else if (action == "turn_on")
    {
      acStatus = true;
      acTemp = targetTemp;
      acFan = targetFan;
      acMode = targetMode;
      sendDaikinCommand("AI_ON");
      lastAIResponse = reason;
      autoOptimizations++;
    }
    else if (action == "adjust")
    {
      acTemp = targetTemp;
      acFan = targetFan;
      acMode = targetMode;
      sendDaikinCommand("AI_ADJUST");
      lastAIResponse = reason;
      autoOptimizations++;
    }

    lastTemp = temperature;
    lastPresence = presenceDetected;
  }
  else
  {
    addLog("AI", "⏸ MOCK LLM: Maintain - All OK");
  }

  lastCheck = millis();
}

// ============ GỌI VOICE API (GEMINI) ============
String callVoiceAPI(String voiceText)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    addLog("ERROR", "WiFi not connected");
    return "";
  }

  HTTPClient http;
  http.begin(VOICE_API_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(API_KEY));
  http.setTimeout(15000);

  DynamicJsonDocument doc(1024);
  doc["text"] = voiceText;
  doc["temperature"] = temperature;
  doc["humidity"] = humidity;
  doc["ac_status"] = acStatus;
  doc["ac_temp"] = acTemp;
  doc["ac_mode"] = acMode;
  doc["ac_fan"] = fanSpeedToString(acFan);

  String payload;
  serializeJson(doc, payload);

  addLog("INFO", "→ VOICE API: " + voiceText);

  aiProcessing = true;
  int httpCode = http.POST(payload);
  aiProcessing = false;
  voiceCommands++;

  String response = "";

  if (httpCode > 0)
  {
    response = http.getString();
    addLog("SUCCESS", "← VOICE HTTP " + String(httpCode));
  }
  else
  {
    addLog("ERROR", "VOICE failed: " + String(httpCode));
  }

  http.end();
  return response;
}

// ============ XỬ LÝ QUYẾT ĐỊNH AI ============
void processAIDecision(String aiResponse)
{
  addLog("INFO", "Process AI Decision...");

  if (aiResponse.length() == 0 || aiResponse.indexOf("\"error\"") != -1)
  {
    addLog("ERROR", "Invalid AI response");
    return;
  }

  int jsonStart = aiResponse.indexOf('{');
  int jsonEnd = aiResponse.lastIndexOf('}');
  if (jsonStart == -1 || jsonEnd == -1)
  {
    addLog("ERROR", "No JSON in response");
    return;
  }

  String jsonStr = aiResponse.substring(jsonStart, jsonEnd + 1);
  DynamicJsonDocument doc(768);
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

    if (doc.containsKey("fan_speed"))
    {
      if (doc["fan_speed"].is<String>())
      {
        acFan = stringToFanSpeed(doc["fan_speed"].as<String>());
      }
      else
      {
        int fanInt = doc["fan_speed"] | 3;
        acFan = intToFanSpeed(fanInt);
      }
    }

    acMode = doc["mode"] | "COOL";
    sendDaikinCommand("VOICE_ON");
    addLog("SUCCESS", "AC ON " + String(acTemp) + "C " + fanSpeedToString(acFan));
  }
  else if (action == "turn_off")
  {
    acStatus = false;
    sendDaikinCommand("VOICE_OFF");
    addLog("SUCCESS", "AC OFF");
  }
  else if (action == "adjust")
  {
    if (acStatus)
    {
      acTemp = constrain(doc["temperature"] | acTemp, 16, 30);

      if (doc.containsKey("fan_speed"))
      {
        if (doc["fan_speed"].is<String>())
        {
          acFan = stringToFanSpeed(doc["fan_speed"].as<String>());
        }
        else
        {
          int fanInt = doc["fan_speed"] | fanSpeedToInt(acFan);
          acFan = intToFanSpeed(fanInt);
        }
      }

      acMode = doc["mode"] | acMode;
      sendDaikinCommand("VOICE_ADJUST");
      addLog("SUCCESS", "AC adj " + String(acTemp) + "C " + fanSpeedToString(acFan));
    }
  }

  String reason = doc["reason"] | "No reason";
  addLog("INFO", "Why: " + reason.substring(0, 30));
  lastAIResponse = reason;
}

// ============ XỬ LÝ NÚT BẤM ============
void handleButtons()
{
  static bool lastPowerBtn = HIGH;
  static bool lastAIBtn = HIGH;
  static bool lastTestPresenceBtn = HIGH;
  static unsigned long lastPressTime = 0;

  if (millis() - lastPressTime < 500)
    return;

  bool currentPowerBtn = digitalRead(BTN_POWER);
  bool currentAIBtn = digitalRead(BTN_AI);
  bool currentTestPresenceBtn = digitalRead(BTN_TEST_PRESENCE);

  if (lastPowerBtn == HIGH && currentPowerBtn == LOW)
  {
    lastPressTime = millis();
    acStatus = !acStatus;
    addLog("INFO", acStatus ? "BTN: AC ON" : "BTN: AC OFF");
    beep(acStatus ? 100 : 50, acStatus ? 1 : 2);
    sendDaikinCommand("BTN_POWER");
    updateLCD();
  }

  if (lastAIBtn == HIGH && currentAIBtn == LOW)
  {
    lastPressTime = millis();
    aiEnabled = !aiEnabled;
    addLog("INFO", aiEnabled ? "BTN: AI ON" : "BTN: AI OFF");
    beep(100, aiEnabled ? 2 : 3);
    lcd.clear();
    lcd.print(aiEnabled ? "AI Mode: ON" : "AI Mode: OFF");
    delay(1500);
    updateLCD();
  }

  if (lastTestPresenceBtn == HIGH && currentTestPresenceBtn == LOW)
  {
    lastPressTime = millis();
    testPresenceMode = !testPresenceMode;

    if (testPresenceMode)
    {
      // Khi BẬT test mode - giả lập có người
      presenceDetected = true;
      motionDetected = true;
      presenceDistance = 50.0;
      lastPresenceTime = millis();
      lastMotionTime = millis();
      addLog("INFO", "✓ TEST MODE: PRESENCE FORCED ON");
      beep(100, 3);
    }
    else
    {
      // Khi TẮT test mode - reset về thực tế
      addLog("INFO", "✓ TEST MODE: OFF - REAL SENSORS");
      beep(50, 3);
    }

    lcd.clear();
    lcd.print("TEST: PRESENCE");
    lcd.setCursor(0, 1);
    lcd.print(testPresenceMode ? "Status: ON" : "Status: OFF");
    delay(1500);
    updateLCD();
  }

  lastPowerBtn = currentPowerBtn;
  lastAIBtn = currentAIBtn;
  lastTestPresenceBtn = currentTestPresenceBtn;
}

// ============ NHẬN IR ============
void receiveIR()
{
  if (irrecv.decode(&results))
  {
    uint32_t irCode = results.value;
    if (irCode != 0xFFFFFFFF)
    {
      addLog("INFO", "IR RECV: 0x" + String(irCode, HEX));
      irCommands++;
      beep(80, 1);
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
  return false;
}

// ============ SETUP WEBSERVER ============
// ============ TRONG HÀM setupWebServer() - THÊM VÀO ĐẦU ============

// ============ SETUP WEBSERVER ============
// ============ TRONG HÀM setupWebServer() - THÊM VÀO ĐẦU ============

void setupWebServer()
{
  // BẮT BUỘC: XỬ LÝ CORS TRƯỚC KHI ĐỊNH NGHĨA ROUTES

  // 1. Thêm default headers cho TẤT CẢ responses
  // CHỈ CẦN CÀI ĐẶT 1 LẦN TẠI ĐÂY
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Requested-With");
  DefaultHeaders::Instance().addHeader("Access-Control-Max-Age", "3600");

  // 2. XỬ LÝ OPTIONS PREFLIGHT CHO MỌI ENDPOINT
  server.onNotFound([](AsyncWebServerRequest *request)
                    {
    if (!request || request->_tempObject) return;
    
    if (request->method() == HTTP_OPTIONS) {
      if (!request->_tempObject) {
        request->send(200);
      }
    } else {
      if (!request->_tempObject) {
        request->send(404, "application/json", "{\"error\":\"Not found\"}");
      }
    } });

  // 3. OPTIONS handler tổng quát
  server.on("/*", HTTP_OPTIONS, [](AsyncWebServerRequest *request)
            { 
              // Tự động áp dụng DefaultHeaders
              request->send(200); });

  // ============ CÁC ROUTES KHÁC GIỮ NGUYÊN ============

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!request || request->_tempObject) return;
    
    DynamicJsonDocument doc(512);
    doc["name"] = "Daikin AC Control";
    doc["version"] = "7.3-PCB-Fixed";
    doc["model"] = "Daikin";
    doc["ai_mode"] = "Mock LLM (Embedded)";
    doc["status"] = "ok";
    String response;
    serializeJson(doc, response);
    
    if (!request->_tempObject) {
      AsyncWebServerResponse *resp = request->beginResponse(200, "application/json", response);
      request->send(resp);
    } });

  server.on("/sensors", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!request || request->_tempObject) return;
    
    if (!authenticateRequest(request)) {
      if (!request->_tempObject) {
        AsyncWebServerResponse *resp = request->beginResponse(401, "application/json", "{\"error\":\"Unauthorized\"}");
        request->send(resp);
      }
      return;
    }
    
    DynamicJsonDocument doc(768);
    doc["temperature"] = temperature;
    doc["humidity"] = humidity;
    doc["light"] = lightLevel;
    doc["motion"] = motionDetected;
    doc["presence"] = presenceDetected;
    doc["presence_distance"] = presenceDistance;
    doc["test_mode"] = testPresenceMode;
    doc["ac_status"] = acStatus;
    doc["ac_temp"] = acTemp;
    doc["ac_mode"] = acMode;
    doc["ac_fan"] = fanSpeedToString(acFan);
    doc["ac_fan_level"] = fanSpeedToInt(acFan);
    doc["llm_enabled"] = aiEnabled;
    doc["model"] = "Daikin";
    
    String response;
    serializeJson(doc, response);
    
    if (!request->_tempObject) {
      AsyncWebServerResponse *resp = request->beginResponse(200, "application/json", response);
      request->send(resp);
    } });

  // FIX: /ac/command
  server.on("/ac/command", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
            {
    // Kiểm tra request còn hợp lệ
    if (!request || request->_tempObject) return;
    
    if (!authenticateRequest(request)) {
      if (request->_tempObject == nullptr) {
        AsyncWebServerResponse *resp = request->beginResponse(401, "application/json", "{\"error\":\"Unauthorized\"}");
        request->send(resp);
      }
      return;
    }
    
    DynamicJsonDocument doc(512);
    deserializeJson(doc, (const char*)data);
    
    bool changed = false;
    
    if (doc.containsKey("status")) {
      acStatus = doc["status"].as<bool>();
      changed = true;
    }
    
    if (doc.containsKey("temperature")) {
      int temp = doc["temperature"];
      if (temp >= 16 && temp <= 30) {
        acTemp = temp;
        changed = true;
      }
    }
    
    if (doc.containsKey("mode")) {
      String mode = doc["mode"].as<String>();
      mode.toUpperCase();
      if (mode == "COOL" || mode == "HEAT" || mode == "DRY" || 
          mode == "FAN" || mode == "AUTO") {
        acMode = mode;
        changed = true;
      }
    }
    
    if (doc.containsKey("fan_speed")) {
      if (doc["fan_speed"].is<String>()) {
        acFan = stringToFanSpeed(doc["fan_speed"].as<String>());
      } else {
        int fan = doc["fan_speed"];
        acFan = intToFanSpeed(fan);
      }
      changed = true;
    }
    
    if (changed) {
      sendDaikinCommand("API_COMMAND");
      
      DynamicJsonDocument respDoc(512);
      respDoc["success"] = true;
      respDoc["status"] = acStatus ? "on" : "off";
      respDoc["temperature"] = acTemp;
      respDoc["mode"] = acMode;
      respDoc["fan_speed"] = fanSpeedToString(acFan);
      respDoc["fan_level"] = fanSpeedToInt(acFan);
      
      String response;
      serializeJson(respDoc, response);
      
      if (!request->_tempObject) {
        AsyncWebServerResponse *resp = request->beginResponse(200, "application/json", response);
        request->send(resp);
      }
    } else {
      if (!request->_tempObject) {
        AsyncWebServerResponse *resp = request->beginResponse(400, "application/json", "{\"error\":\"No valid settings\"}");
        request->send(resp);
      }
    } });

  // FIX: /ai/toggle
  server.on("/ai/toggle", HTTP_POST, [](AsyncWebServerRequest *request)
            {
    if (!request || request->_tempObject) return;
    
    if (!authenticateRequest(request)) {
      if (!request->_tempObject) {
        AsyncWebServerResponse *resp = request->beginResponse(401, "application/json", "{\"error\":\"Unauthorized\"}");
        request->send(resp);
      }
      return;
    }
    
    aiEnabled = !aiEnabled;
    
    DynamicJsonDocument doc(256);
    doc["success"] = true;
    doc["ai_enabled"] = aiEnabled;
    doc["message"] = aiEnabled ? "AI enabled" : "AI disabled";
    
    String response;
    serializeJson(doc, response);
    
    addLog("INFO", aiEnabled ? "AI Mode: ENABLED" : "AI Mode: DISABLED");
    
    if (!request->_tempObject) {
      AsyncWebServerResponse *resp = request->beginResponse(200, "application/json", response);
      request->send(resp);
    }
    
    updateLCD(); });

  // FIX: /voice/command
  server.on("/voice/command", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
            {
    // Kiểm tra request còn hợp lệ
    if (!request || request->_tempObject) return;
    
    if (!authenticateRequest(request)) {
      if (request->_tempObject == nullptr) {
        AsyncWebServerResponse *resp = request->beginResponse(401, "application/json", "{\"error\":\"Unauthorized\"}");
        request->send(resp);
      }
      return;
    }
    
    DynamicJsonDocument doc(512);
    deserializeJson(doc, (const char*)data);
    
    String voiceText = doc["text"] | "";
    if (voiceText.length() == 0) {
      AsyncWebServerResponse *resp = request->beginResponse(400, "application/json", "{\"error\":\"Missing text\"}");
      request->send(resp);
      return;
    }
    
    addLog("INFO", "Voice: " + voiceText);

    // Forward đến Flask Gemini Server
    String apiResponse = callVoiceAPI(voiceText);
    
    if (apiResponse.length() > 0 && apiResponse.indexOf("error") == -1) {
      processAIDecision(apiResponse);
      
      // Parse response để lấy đầy đủ thông tin
      DynamicJsonDocument respDoc(768);
      int jsonStart = apiResponse.indexOf('{');
      int jsonEnd = apiResponse.lastIndexOf('}');
      
      if (jsonStart != -1 && jsonEnd != -1) {
        String jsonStr = apiResponse.substring(jsonStart, jsonEnd + 1);
        DynamicJsonDocument geminiDoc(512);
        DeserializationError error = deserializeJson(geminiDoc, jsonStr);
        
        if (!error) {
          respDoc["success"] = true;
          respDoc["action"] = geminiDoc["action"] | "unknown";
          respDoc["temperature"] = geminiDoc["temperature"] | acTemp;
          respDoc["fan_speed"] = geminiDoc["fan_speed"] | fanSpeedToString(acFan);
          respDoc["mode"] = geminiDoc["mode"] | acMode;
          respDoc["reason"] = geminiDoc["reason"] | lastAIResponse;
          
          //Thêm audio_url nếu có
          if (geminiDoc.containsKey("audio_url")) {
            respDoc["audio_url"] = geminiDoc["audio_url"].as<String>();
          }
        } else {
          respDoc["success"] = true;
          respDoc["reason"] = lastAIResponse;
        }
      } else {
        respDoc["success"] = true;
        respDoc["reason"] = lastAIResponse;
      }
      
      String response;
      serializeJson(respDoc, response);
      
      if (!request->_tempObject) {
        AsyncWebServerResponse *resp = request->beginResponse(200, "application/json", response);
        request->send(resp);
      }
      
    } else {
      if (!request->_tempObject) {
        AsyncWebServerResponse *resp = request->beginResponse(500, "application/json", 
          "{\"error\":\"Voice API failed\",\"reason\":\"Không kết nối được Gemini server\"}");
        request->send(resp);
      }
    } });

  // ============ CÁC ENDPOINT KHÁC - ĐÃ XÓA CORS HEADERS ============

  server.on("/ac/status", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!request || request->_tempObject) return;
    
    if (!authenticateRequest(request)) {
      if (!request->_tempObject) {
        AsyncWebServerResponse *resp = request->beginResponse(401, "application/json", "{\"error\":\"Unauthorized\"}");
        request->send(resp);
      }
      return;
    }
    
    DynamicJsonDocument doc(768);
    doc["status"] = acStatus ? "on" : "off";
    doc["temperature"] = acTemp;
    doc["mode"] = acMode;
    doc["fan_speed"] = fanSpeedToString(acFan);
    doc["fan_level"] = fanSpeedToInt(acFan);
    doc["llm_enabled"] = aiEnabled;
    doc["model"] = "Daikin";
    
    String response;
    serializeJson(doc, response);
    
    if (!request->_tempObject) {
      AsyncWebServerResponse *resp = request->beginResponse(200, "application/json", response);
      request->send(resp);
    } });

  server.on("/stats", HTTP_GET, [](AsyncWebServerRequest *request)
            {
    if (!request || request->_tempObject) return;
    
    if (!authenticateRequest(request)) {
      if (!request->_tempObject) {
        AsyncWebServerResponse *resp = request->beginResponse(401, "application/json", "{\"error\":\"Unauthorized\"}");
        request->send(resp);
      }
      return;
    }
    
    DynamicJsonDocument doc(768);
    doc["uptime"] = millis() / 1000;
    doc["model"] = "Daikin";
    doc["ir_commands"] = irCommands;
    doc["voice_commands"] = voiceCommands;
    doc["auto_optimizations"] = autoOptimizations;
    
    String response;
    serializeJson(doc, response);
    
    if (!request->_tempObject) {
      AsyncWebServerResponse *resp = request->beginResponse(200, "application/json", response);
      request->send(resp);
    } });

  server.begin();
  addLog("SUCCESS", "WebServer OK (v7.3 - PCB NULL Fixed)");
}

// ============ SETUP ============
void setup()
{
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n╔═══════════════════════════════╗");
  Serial.println("║  DAIKIN AC CONTROL v7.3       ║");
  Serial.println("║  ✅ PCB NULL Fixed            ║");
  Serial.println("║  ✅ Request Validation        ║");
  Serial.println("║  ✅ Connection Stable         ║");
  Serial.println("╚═══════════════════════════════╝");

  pinMode(BTN_POWER, INPUT_PULLUP);
  pinMode(BTN_AI, INPUT_PULLUP);
  pinMode(BTN_TEST_PRESENCE, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(PIR_PIN, INPUT);
  pinMode(RADAR_TRIG_PIN, OUTPUT);
  pinMode(RADAR_ECHO_PIN, INPUT);

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.print("Daikin AC v7.3");
  lcd.setCursor(0, 1);
  lcd.print("PCB NULL Fixed");
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
  addLog("SUCCESS", "Daikin IR OK");

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30)
  {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    addLog("SUCCESS", "WiFi: " + WiFi.localIP().toString());
    lcd.clear();
    lcd.print("WiFi OK!");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP());
    beep(100, 2);
    delay(2000);
  }
  else
  {
    addLog("WARN", "WiFi failed - Voice disabled");
  }

  setupWebServer();

  lcd.clear();
  lcd.print("Daikin Ready!");
  lcd.setCursor(0, 1);
  lcd.print("AI:MockLLM ✓");
  beep(200, 1);
  delay(2000);
}

// ============ LOOP ============
void loop()
{
  if (millis() - lastSensorRead > sensorInterval)
  {
    lastSensorRead = millis();
    readSensors();
    updateLCD();
  }

  handleButtons();
  receiveIR();

  // AI luôn chạy khi được bật, không quan tâm test mode
  if (aiEnabled)
  {
    mockLLMOptimize();
  }

  delay(10);
}