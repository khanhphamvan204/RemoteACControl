from flask import Flask, request, jsonify
import requests
import json
import re

app = Flask(__name__)

API_KEY = "AC_SECRET_KEY_2024_LLM_V5"
GEMINI_KEY = "AIzaSyBfwAhNb3W22MzaeKOh58ZPh_vlSHH1L2A"
GEMINI_URL = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent"

# System prompt cho phân tích môi trường
SYSTEM_PROMPT = """
You are an intelligent AC control AI. Analyze environmental sensor data (temperature, humidity, CO2, light, motion, presence, AC status) and provide a JSON response to optimize AC settings for comfort and energy efficiency. Return:
{
  "action": "turn_on|turn_off|adjust|maintain",
  "temperature": <16-30>,
  "fan_speed": <1-3>,
  "mode": "COOL|DRY|FAN",
  "reason": "<concise explanation, max 30 chars>"
}
Consider: turn_on if too hot (>28°C) or humid (>70%), turn_off if no presence or cool enough (<23°C), adjust for minor tweaks, maintain if optimal.
"""

# System prompt cho voice command
VOICE_PROMPT = """
You are a friendly and helpful AC voice assistant named "Trợ lý AI". Analyze the user's voice command and current environment data to provide an appropriate AC control action.

User may say things like:
- "Turn on the AC" / "Bật điều hòa"
- "Make it cooler" / "Làm mát hơn"
- "Turn off AC" / "Tắt điều hòa"
- "Set temperature to 24" / "Đặt nhiệt độ 24 độ"
- "It's too hot" / "Nóng quá"
- "I'm cold" / "Lạnh quá"
- "Switch to dry mode" / "Chuyển sang chế độ hút ẩm"

Respond with JSON only:
{
  "action": "turn_on|turn_off|adjust|maintain",
  "temperature": <16-30>,
  "fan_speed": <1-3>,
  "mode": "COOL|DRY|FAN",
  "reason": "<friendly explanation in VIETNAMESE using 'mình' for 'I' and addressing user warmly>"
}

IMPORTANT: 
- "reason" field MUST be in Vietnamese language
- Use friendly tone: "mình đã...", "để bạn...", "cho bạn..."
- Be conversational and warm like talking to a friend
- Explain what you did and why

Examples of good "reason":
- "Bạn nói nóng quá nên mình đã bật điều hòa ở 22°C để làm mát phòng cho bạn nhé!"
- "Mình đã giảm nhiệt độ xuống 2°C như bạn yêu cầu, giờ là 24°C rồi!"
- "Dạ, mình đã tắt điều hòa theo yêu cầu của bạn!"

Be smart: understand intent, not just keywords. Consider current temperature and AC status.
"""

def authenticate():
    auth = request.headers.get("Authorization", "")
    if auth.startswith("Bearer "):
        return auth.split(" ")[1] == API_KEY
    return request.args.get("api_key") == API_KEY

def call_gemini(prompt, user_message):
    """Gọi Gemini API"""
    try:
        payload = {
            "contents": [{
                "parts": [{"text": f"{prompt}\n\n{user_message}"}]
            }],
            "generationConfig": {
                "temperature": 0.3,
                "topP": 0.8
            }
        }

        print(f"[INFO] Calling Gemini...")
        
        res = requests.post(
            f"{GEMINI_URL}?key={GEMINI_KEY}",
            json=payload,
            timeout=20
        )
        
        if res.status_code != 200:
            print(f"[ERROR] HTTP {res.status_code}: {res.text[:300]}")
            return None

        resp = res.json()
        
        if "candidates" not in resp or len(resp["candidates"]) == 0:
            print(f"[ERROR] No candidates")
            return None
        
        candidate = resp["candidates"][0]
        finish = candidate.get("finishReason", "")
        
        print(f"[DEBUG] Finish: {finish}")
        
        if finish == "SAFETY":
            print("[ERROR] Blocked by safety")
            return None
        
        if "content" not in candidate or "parts" not in candidate["content"]:
            print(f"[ERROR] No content/parts")
            return None
        
        parts = candidate["content"]["parts"]
        if len(parts) == 0 or "text" not in parts[0]:
            print(f"[ERROR] No text in parts")
            return None
        
        text = parts[0]["text"].strip()
        print(f"[SUCCESS] Response: {text[:150]}...")
        
        return text

    except requests.Timeout:
        print("[ERROR] Timeout")
        return None
    except Exception as e:
        print(f"[ERROR] {e}")
        return None

def extract_json(text):
    """Trích xuất JSON từ text response"""
    try:
        # Tìm JSON
        start = text.find('{')
        end = text.rfind('}')
        if start != -1 and end != -1:
            json_str = text[start:end+1]
            parsed = json.loads(json_str)
            return parsed
        return None
    except json.JSONDecodeError:
        return None

@app.route("/")
def index():
    return jsonify({
        "service": "AC Control AI Server",
        "version": "2.0-Fixed",
        "status": "ok",
        "endpoints": {
            "POST /llm/query": "Analyze environment",
            "POST /llm/control": "Auto control",
            "POST /voice/command": "Voice control"
        }
    })

@app.route("/llm/query", methods=["POST"])
def llm_query():
    """Endpoint cũ - phân tích môi trường"""
    if not authenticate():
        return jsonify({"error": "Unauthorized"}), 401

    try:
        data = request.get_json(force=True)
        
        # Parse query từ 2 format
        if "query" in data:
            user_query = data["query"]
        elif "contents" in data:
            user_query = data["contents"][0]["parts"][0]["text"]
        else:
            return jsonify({"error": "Missing query"}), 400

        print(f"[INFO] Environment Query: {user_query[:100]}...")
        
        # Call Gemini
        text = call_gemini(SYSTEM_PROMPT, user_query)
        
        if not text:
            return jsonify({"error": "Gemini API failed"}), 500
        
        # Parse JSON
        parsed = extract_json(text)
        if parsed:
            print(f"[SUCCESS] Action: {parsed.get('action')}")
            return jsonify(parsed), 200
        else:
            return jsonify({"raw_text": text}), 200

    except Exception as e:
        print(f"[ERROR] {e}")
        return jsonify({"error": str(e)}), 500

@app.route("/llm/control", methods=["POST"])
def llm_control():
    """Alias cho llm/query"""
    if not authenticate():
        return jsonify({"error": "Unauthorized"}), 401
    return llm_query()

@app.route("/voice/command", methods=["POST"])
def voice_command():
    """Endpoint MỚI - xử lý lệnh giọng nói"""
    if not authenticate():
        return jsonify({"error": "Unauthorized"}), 401

    try:
        data = request.get_json(force=True)
        
        # Lấy text từ giọng nói
        voice_text = data.get("text", "")
        if not voice_text:
            return jsonify({"error": "Missing text"}), 400
        
        # Lấy context môi trường (có giá trị mặc định hợp lý)
        temperature = data.get("temperature", 27)  # Mặc định 27°C (hơi nóng)
        humidity = data.get("humidity", 65)        # Mặc định 65% (bình thường)
        ac_status = data.get("ac_status", False)   # Mặc định tắt
        ac_temp = data.get("ac_temp", 25)          # Mặc định 25°C
        
        # Tạo context message
        context = f"""
Voice Command: "{voice_text}"

Current Environment:
- Temperature: {temperature}°C
- Humidity: {humidity}%
- AC Status: {'ON' if ac_status else 'OFF'}
- AC Temperature: {ac_temp}°C

Analyze the user's voice command and decide the appropriate action.
If environment data is default/unknown, make reasonable assumptions based on the command.
"""
        
        print(f"[INFO] Voice Command: {voice_text}")
        print(f"[INFO] Context: T={temperature}°C (default), AC={'ON' if ac_status else 'OFF'}")
        
        # Call Gemini với voice prompt
        text = call_gemini(VOICE_PROMPT, context)
        
        if not text:
            return jsonify({"error": "Gemini API failed"}), 500
        
        # Parse JSON
        parsed = extract_json(text)
        if parsed:
            print(f"[SUCCESS] Voice Action: {parsed.get('action')}")
            print(f"[SUCCESS] Reason: {parsed.get('reason')}")
            return jsonify(parsed), 200
        else:
            # Fallback: phân tích đơn giản bằng keywords
            print("[WARN] No JSON, using fallback")
            fallback = analyze_voice_fallback(voice_text, temperature, ac_status, ac_temp)
            return jsonify(fallback), 200

    except Exception as e:
        print(f"[ERROR] {e}")
        return jsonify({"error": str(e)}), 500

def analyze_voice_fallback(text, temp, ac_on, ac_temp):
    """Phân tích giọng nói đơn giản (fallback khi Gemini fail)"""
    text_lower = text.lower()
    
    # Turn on - Luôn đặt nhiệt độ thoải mái
    if any(word in text_lower for word in ["turn on", "bật", "mở", "start", "power on"]):
        # Quyết định nhiệt độ dựa vào temp hiện tại
        target_temp = 24  # Mặc định thoải mái
        fan = 2
        reason = ""
        
        if temp > 30:  # Rất nóng
            target_temp = 22
            fan = 3
            reason = f"Phòng đang {temp}°C, rất nóng nên mình đã bật điều hòa ở {target_temp}°C để làm mát nhanh cho bạn nhé!"
        elif temp > 28:  # Nóng
            target_temp = 24
            fan = 2
            reason = f"Trời đang hơi nóng ({temp}°C), mình đã bật điều hòa ở {target_temp}°C cho bạn!"
        elif temp > 26:  # Hơi nóng
            target_temp = 25
            fan = 2
            reason = f"Mình đã bật điều hòa ở {target_temp}°C như bạn yêu cầu!"
        else:  # Bình thường
            target_temp = 26
            fan = 1
            reason = f"Dạ, mình đã bật điều hòa ở {target_temp}°C cho bạn!"
            
        return {
            "action": "turn_on",
            "temperature": target_temp,
            "fan_speed": fan,
            "mode": "COOL",
            "reason": reason
        }
    
    # Turn off
    if any(word in text_lower for word in ["turn off", "tắt", "stop", "power off"]):
        return {
            "action": "turn_off",
            "temperature": ac_temp,
            "fan_speed": 1,
            "mode": "COOL",
            "reason": "Dạ, mình đã tắt điều hòa theo yêu cầu của bạn!"
        }
    
    # Cooler / colder - Giảm nhiệt
    if any(word in text_lower for word in ["cool", "cold", "mát", "lạnh", "giảm"]):
        if ac_on:
            new_temp = max(16, ac_temp - 2)
            return {
                "action": "adjust",
                "temperature": new_temp,
                "fan_speed": 3,
                "mode": "COOL",
                "reason": f"Mình đã giảm nhiệt độ xuống 2°C từ {ac_temp}°C thành {new_temp}°C để mát hơn cho bạn nhé!"
            }
        else:
            # Nếu chưa bật, bật với nhiệt độ thấp
            return {
                "action": "turn_on",
                "temperature": 22,
                "fan_speed": 3,
                "mode": "COOL",
                "reason": "Bạn muốn mát nên mình đã bật điều hòa ở 22°C với quạt mạnh cho bạn!"
            }
    
    # Warmer - Tăng nhiệt
    if any(word in text_lower for word in ["warm", "ấm", "tăng", "increase"]):
        if ac_on:
            new_temp = min(30, ac_temp + 2)
            return {
                "action": "adjust",
                "temperature": new_temp,
                "fan_speed": 1,
                "mode": "COOL",
                "reason": f"Mình đã tăng nhiệt độ lên 2°C từ {ac_temp}°C thành {new_temp}°C để ấm hơn cho bạn!"
            }
        else:
            return {
                "action": "maintain",
                "temperature": 27,
                "fan_speed": 1,
                "mode": "COOL",
                "reason": "Phòng đang ấm rồi nên mình không cần bật điều hòa đâu bạn nhé!"
            }
    
    # Hot complaint - Phàn nàn nóng
    if any(word in text_lower for word in ["hot", "nóng", "heat"]):
        if ac_on:
            # Đã bật rồi → giảm thêm
            new_temp = max(16, ac_temp - 3)
            return {
                "action": "adjust",
                "temperature": new_temp,
                "fan_speed": 3,
                "mode": "COOL",
                "reason": f"Bạn nói nóng quá nên mình đã giảm nhiệt độ xuống 3°C từ {ac_temp}°C thành {new_temp}°C và bật quạt mạnh cho bạn!"
            }
        else:
            # Chưa bật → bật với nhiệt độ thấp
            return {
                "action": "turn_on",
                "temperature": 22,
                "fan_speed": 3,
                "mode": "COOL",
                "reason": "Bạn cảm thấy nóng nên mình đã bật điều hòa ở 22°C với quạt mạnh để làm mát nhanh cho bạn nhé!"
            }
    
    # Dry mode
    if any(word in text_lower for word in ["dry", "hút ẩm", "dehumid", "ẩm"]):
        return {
            "action": "adjust" if ac_on else "turn_on",
            "temperature": 26,
            "fan_speed": 2,
            "mode": "DRY",
            "reason": "Mình đã chuyển sang chế độ hút ẩm ở 26°C để giảm độ ẩm trong phòng cho bạn!"
        }
    
    # Fan only
    if any(word in text_lower for word in ["fan only", "quạt", "gió"]):
        return {
            "action": "adjust" if ac_on else "turn_on",
            "temperature": ac_temp,
            "fan_speed": 3,
            "mode": "FAN",
            "reason": "Dạ, mình đã chuyển sang chế độ quạt gió như bạn muốn!"
        }
    
    # Số nhiệt độ cụ thể: "24 độ", "set to 24", "24 degrees"
    temp_match = re.search(r'(\d+)\s*(degree|độ|°|度)?', text_lower)
    if temp_match:
        target_temp = int(temp_match.group(1))
        if 16 <= target_temp <= 30:
            if ac_on:
                return {
                    "action": "adjust",
                    "temperature": target_temp,
                    "fan_speed": 2,
                    "mode": "COOL",
                    "reason": f"Mình đã đặt nhiệt độ {target_temp}°C theo yêu cầu của bạn!"
                }
            else:
                return {
                    "action": "turn_on",
                    "temperature": target_temp,
                    "fan_speed": 2,
                    "mode": "COOL",
                    "reason": f"Mình đã bật điều hòa và đặt nhiệt độ {target_temp}°C cho bạn!"
                }
    
    # Default - Nếu không hiểu lệnh
    # Nếu AC đang tắt và không rõ ý → bật với nhiệt độ thoải mái
    if not ac_on:
        return {
            "action": "turn_on",
            "temperature": 24,
            "fan_speed": 2,
            "mode": "COOL",
            "reason": "Mình đã bật điều hòa ở 24°C - nhiệt độ thoải mái cho bạn!"
        }
    else:
        # AC đang bật → giữ nguyên
        return {
            "action": "maintain",
            "temperature": ac_temp,
            "fan_speed": 2,
            "mode": "COOL",
            "reason": f"Điều hòa đang hoạt động tốt ở {ac_temp}°C rồi, mình giữ nguyên nhé!"
        }

@app.after_request
def after_request(response):
    response.headers["Access-Control-Allow-Origin"] = "*"
    response.headers["Access-Control-Allow-Headers"] = "Content-Type, Authorization"
    response.headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS"
    return response

if __name__ == "__main__":
    print("=" * 50)
    print("🚀 AC Control AI Server v2.0-Fixed")
    print("=" * 50)
    print("📡 Server: http://0.0.0.0:5000")
    print("\n📋 Endpoints:")
    print("  POST /llm/query      - Environment analysis")
    print("  POST /llm/control    - Auto control")
    print("  POST /voice/command  - Voice control (NEW!)")
    print("\n🎤 Voice Command Examples:")
    print("  - Turn on the AC")
    print("  - Bật điều hòa")
    print("  - Make it cooler")
    print("  - Set temperature to 24")
    print("  - It's too hot")
    print("  - Switch to dry mode")
    print("=" * 50)
    app.run(host="0.0.0.0", port=5000, debug=True)