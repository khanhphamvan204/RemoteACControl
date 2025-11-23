from flask import Flask, request, jsonify, send_file
import requests
import json
import re
import os
import tempfile
from gtts import gTTS
import hashlib

app = Flask(__name__)

API_KEY = "AC_SECRET_KEY_2024_LLM_V5"
GEMINI_KEY = "AIzaSyBkdasAhs0XgsUBfNyRUMKhKPfcUbHoQtw"
GEMINI_URL = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent"

# Cache directory for TTS files
TTS_CACHE_DIR = tempfile.gettempdir() + "/tts_cache"
os.makedirs(TTS_CACHE_DIR, exist_ok=True)

# ============ VOICE COMMAND PROMPT ============
VOICE_PROMPT = """
Bạn là "Trợ lý Điều hòa AI" - trợ lý giọng nói thông minh bằng tiếng Việt.

CHÚ Ý: LUÔN trả về JSON hợp lệ với ĐẦY ĐỦ các field (KHÔNG được dùng null):
{
  "action": "turn_on|turn_off|adjust|maintain",
  "temperature": <16-30>,
  "fan_speed": "QUIET|LOW|MEDIUM|HIGH|AUTO",
  "mode": "COOL|DRY|FAN|HEAT|AUTO",
  "reason": "<giải thích ngắn gọn 50-100 ký tự>"
}

QUAN TRỌNG - LOGIC ACTION:
- turn_on: Khi AC đang TẮT và cần BẬT lên
- turn_off: Khi AC đang BẬT và cần TẮT đi (vẫn cần trả temperature/fan_speed/mode hiện tại)
- adjust: Khi AC đang BẬT và cần THAY ĐỔI cài đặt
- maintain: Khi AC đang BẬT và GIỮ NGUYÊN (không cần thay đổi)

LỆNH CƠ BẢN:
- Bật: "bật điều hòa", "mở máy lạnh", "bật lên"
  → AC đang TẮT: action = "turn_on"
  
- Tắt: "tắt điều hòa", "tắt đi", "ngưng"
  → AC đang BẬT: action = "turn_off"
  
- Mát hơn: "lạnh hơn", "giảm nhiệt", "cho mát"
  → AC đang TẮT: action = "turn_on" (bật với nhiệt độ thấp)
  → AC đang BẬT: action = "adjust" (giảm 1-2°C)
  
- Ấm hơn: "ấm hơn", "bớt lạnh", "tăng nhiệt"
  → AC đang BẬT: action = "adjust" (tăng 1-2°C)
  → Nếu đã đủ ấm: action = "turn_off"
  
- Đặt nhiệt độ: "24 độ", "chỉnh 25", "để 23"
  → AC đang TẮT: action = "turn_on" (bật với nhiệt độ chỉ định)
  → AC đang BẬT: action = "adjust" (thay đổi nhiệt độ)
  
- Quạt/chế độ: "quạt mạnh", "hút ẩm"
  → AC đang BẬT: action = "adjust"

TỰ ĐỘNG ĐIỀU CHỈNH:
Khi người dùng nói: "điều chỉnh cho phù hợp", "tự động", "chỉnh thoải mái", "nhiệt độ tốt nhất"
→ Phân tích cảm biến và quyết định action:

AC ĐANG TẮT:
- Nhiệt độ phòng < 24°C: action = "maintain" (không cần bật, giải thích lý do)
- Nhiệt độ phòng 24-27°C: action = "turn_on" với AC = Phòng - 1°C
- Nhiệt độ phòng 28-30°C: action = "turn_on" với AC = Phòng - 4°C
- Nhiệt độ phòng > 30°C: action = "turn_on" với AC = 21-23°C, quạt HIGH

AC ĐANG BẬT:
- So sánh nhiệt độ AC hiện tại với nhiệt độ tối ưu
- Nếu cần thay đổi > 1°C: action = "adjust"
- Nếu đã phù hợp: action = "maintain"
- Nếu phòng quá lạnh (< 20°C): action = "turn_off"

Độ ẩm:
- > 75%: Ưu tiên mode = "DRY"
- < 50%: mode = "COOL"

PHONG CÁCH:
- Dùng "mình" và "bạn", thân thiện
- Giải thích ngắn gọn đã làm gì
- Khi tự động: Đề cập dữ liệu cảm biến
- VD TẮT→BẬT: "Phòng 32°C quá nóng, mình đã bật điều hòa 23°C quạt mạnh!"
- VD BẬT→CHỈNH: "Mình đã giảm từ 26°C xuống 24°C cho bạn mát hơn!"
- VD GIỮ NGUYÊN: "Nhiệt độ 25°C hiện tại đã phù hợp rồi bạn nhé!"

LƯU Ý:
- Nhiệt độ hợp lệ: 16-30°C
- LUÔN trả về ĐẦY ĐỦ các field, KHÔNG được null
- Khi turn_off: Giữ nguyên temperature/fan_speed/mode hiện tại của AC
- Luôn kiểm tra trạng thái AC hiện tại trước khi quyết định action
- Nếu không rõ và AC TẮT → turn_on với 25°C, MEDIUM, COOL
- Nếu không rõ và AC BẬT → maintain (giữ nguyên)
"""

def call_gemini(prompt, user_message, retry=True):
    """Gọi Gemini API - CHỈ dùng cho voice commands"""
    try:
        payload = {
            "contents": [{
                "parts": [{"text": f"{prompt}\n\n{user_message}"}]
            }],
            "generationConfig": {
                "temperature": 0.4,
                "topP": 0.9,
                "topK": 40,
                # "maxOutputTokens": 900
            },
            "safetySettings": [
                {"category": "HARM_CATEGORY_HARASSMENT", "threshold": "BLOCK_NONE"},
                {"category": "HARM_CATEGORY_HATE_SPEECH", "threshold": "BLOCK_NONE"},
                {"category": "HARM_CATEGORY_SEXUALLY_EXPLICIT", "threshold": "BLOCK_NONE"},
                {"category": "HARM_CATEGORY_DANGEROUS_CONTENT", "threshold": "BLOCK_NONE"}
            ]
        }

        print(f"[GEMINI] Calling API for voice command...")
        print(f"[GEMINI] User message: {user_message[:150]}...")
        
        res = requests.post(
            f"{GEMINI_URL}?key={GEMINI_KEY}",
            json=payload,
            timeout=20
        )
        
        if res.status_code != 200:
            print(f"[GEMINI ERROR] HTTP {res.status_code}: {res.text[:500]}")
            return None

        resp = res.json()
        
        # Debug: In toàn bộ response
        print(f"[GEMINI DEBUG] Full response: {json.dumps(resp, ensure_ascii=False)[:500]}")
        
        if "candidates" not in resp or len(resp["candidates"]) == 0:
            print(f"[GEMINI ERROR] No candidates in response")
            print(f"[GEMINI ERROR] Response keys: {resp.keys()}")
            if "error" in resp:
                print(f"[GEMINI ERROR] API Error: {resp['error']}")
            return None
        
        candidate = resp["candidates"][0]
        finish = candidate.get("finishReason", "")
        
        print(f"[GEMINI DEBUG] Finish reason: {finish}")
        
        if finish == "SAFETY":
            print("[GEMINI WARN] Response blocked by safety filters")
            if "safetyRatings" in candidate:
                print(f"[GEMINI WARN] Safety ratings: {candidate['safetyRatings']}")
            if retry:
                print("[GEMINI] Retrying with safer prompt...")
                safer_message = user_message.replace("61.29999924", "32").replace("°C", " degrees")
                return call_gemini(prompt, safer_message, retry=False)
            return None
        
        if "content" not in candidate:
            print(f"[GEMINI ERROR] No 'content' in candidate")
            print(f"[GEMINI ERROR] Candidate keys: {candidate.keys()}")
            print(f"[GEMINI ERROR] Full candidate: {json.dumps(candidate, ensure_ascii=False)[:500]}")
            return None
            
        if "parts" not in candidate["content"]:
            print(f"[GEMINI ERROR] No 'parts' in content")
            print(f"[GEMINI ERROR] Content: {candidate['content']}")
            return None
        
        parts = candidate["content"]["parts"]
        if len(parts) == 0 or "text" not in parts[0]:
            print(f"[GEMINI ERROR] No text in parts")
            print(f"[GEMINI ERROR] Parts: {parts}")
            return None
        
        text = parts[0]["text"].strip()
        print(f"[GEMINI SUCCESS] Got response: {text[:200]}...")
        
        return text

    except requests.Timeout:
        print("[GEMINI ERROR] Request timeout")
        return None
    except Exception as e:
        print(f"[GEMINI ERROR] Exception: {e}")
        import traceback
        traceback.print_exc()
        return None

def extract_json(text):
    """Trích xuất và parse JSON từ text response"""
    try:
        text = re.sub(r'```json\s*', '', text)
        text = re.sub(r'```\s*', '', text)
        text = text.strip()
        
        start = text.find('{')
        end = text.rfind('}')
        
        if start == -1 or end == -1:
            print(f"[JSON ERROR] No braces found in: {text[:100]}")
            return None
        
        json_str = text[start:end+1]
        parsed = json.loads(json_str)
        
        # Xử lý các field thiếu hoặc null
        if "action" not in parsed or parsed["action"] is None:
            parsed["action"] = "maintain"
        if "temperature" not in parsed or parsed["temperature"] is None:
            parsed["temperature"] = 25
        if "fan_speed" not in parsed or parsed["fan_speed"] is None:
            parsed["fan_speed"] = "MEDIUM"
        if "mode" not in parsed or parsed["mode"] is None:
            parsed["mode"] = "COOL"
        if "reason" not in parsed or parsed["reason"] is None:
            parsed["reason"] = "Đã xử lý yêu cầu của bạn!"
        
        # Validate và uppercase fan_speed
        fan_str = str(parsed["fan_speed"]).upper()
        if fan_str not in ["QUIET", "LOW", "MEDIUM", "HIGH", "AUTO"]:
            parsed["fan_speed"] = "MEDIUM"
        else:
            parsed["fan_speed"] = fan_str
        
        # Validate và uppercase mode
        mode_str = str(parsed["mode"]).upper()
        if mode_str not in ["COOL", "DRY", "FAN", "HEAT", "AUTO"]:
            parsed["mode"] = "COOL"
        else:
            parsed["mode"] = mode_str
        
        # Validate temperature
        try:
            parsed["temperature"] = max(16, min(30, int(float(parsed["temperature"]))))
        except (ValueError, TypeError):
            parsed["temperature"] = 25
        
        print(f"[JSON SUCCESS] Parsed - Action: {parsed['action']}, Temp: {parsed['temperature']}, Fan: {parsed['fan_speed']}")
        return parsed
        
    except json.JSONDecodeError as e:
        print(f"[JSON ERROR] Decode error: {e}")
        return None
    except Exception as e:
        print(f"[JSON ERROR] Unexpected: {e}")
        return None

def analyze_voice_fallback(text, temp, ac_on, ac_temp):
    """Fallback logic khi Gemini fail"""
    text_lower = text.lower()
    
    if any(word in text_lower for word in ["turn on", "bật", "mở", "start", "power on"]):
        target_temp = 24
        fan = "MEDIUM"
        reason = ""
        
        if temp > 30:
            target_temp = 22
            fan = "HIGH"
            reason = f"Phòng đang {temp}°C, rất nóng nên mình đã bật điều hòa ở {target_temp}°C với quạt mạnh!"
        elif temp > 28:
            target_temp = 24
            fan = "MEDIUM"
            reason = f"Trời hơi nóng ({temp}°C), mình bật điều hòa ở {target_temp}°C cho bạn!"
        else:
            target_temp = 25
            fan = "LOW"
            reason = f"Mình đã bật điều hòa ở {target_temp}°C cho bạn!"
            
        return {
            "action": "turn_on",
            "temperature": target_temp,
            "fan_speed": fan,
            "mode": "COOL",
            "reason": reason
        }
    
    if any(word in text_lower for word in ["turn off", "tắt", "stop", "power off"]):
        return {
            "action": "turn_off",
            "temperature": ac_temp,
            "fan_speed": "MEDIUM",
            "mode": "COOL",
            "reason": "Dạ, mình đã tắt điều hòa theo yêu cầu của bạn!"
        }
    
    if any(word in text_lower for word in ["cool", "cold", "mát", "lạnh", "giảm"]):
        if ac_on:
            new_temp = max(16, ac_temp - 2)
            return {
                "action": "adjust",
                "temperature": new_temp,
                "fan_speed": "HIGH",
                "mode": "COOL",
                "reason": f"Mình đã giảm nhiệt độ từ {ac_temp}°C xuống {new_temp}°C!"
            }
        else:
            return {
                "action": "turn_on",
                "temperature": 22,
                "fan_speed": "HIGH",
                "mode": "COOL",
                "reason": "Bạn muốn mát nên mình đã bật điều hòa ở 22°C với quạt mạnh!"
            }
    
    if not ac_on:
        return {
            "action": "turn_on",
            "temperature": 24,
            "fan_speed": "MEDIUM",
            "mode": "COOL",
            "reason": "Mình đã bật điều hòa ở 24°C - nhiệt độ thoải mái!"
        }
    else:
        return {
            "action": "maintain",
            "temperature": ac_temp,
            "fan_speed": "MEDIUM",
            "mode": "COOL",
            "reason": f"Điều hòa đang hoạt động tốt ở {ac_temp}°C rồi!"
        }

def generate_tts_audio(text):
    """Tạo file audio từ text sử dụng gTTS"""
    try:
        # Tạo cache key từ text
        cache_key = hashlib.md5(text.encode('utf-8')).hexdigest()
        cache_file = os.path.join(TTS_CACHE_DIR, f"{cache_key}.mp3")
        
        # Kiểm tra cache
        if os.path.exists(cache_file):
            print(f"[TTS] Using cached audio: {cache_key}")
            return cache_file
        
        # Tạo audio mới
        print(f"[TTS] Generating audio for: {text}")
        tts = gTTS(text=text, lang='vi', slow=False)
        tts.save(cache_file)
        print(f"[TTS] Audio saved: {cache_file}")
        return cache_file
        
    except Exception as e:
        print(f"[TTS ERROR] {e}")
        return None

def authenticate():
    """Xác thực API key"""
    auth = request.headers.get("Authorization", "")
    if auth.startswith("Bearer "):
        return auth.split(" ")[1] == API_KEY
    return request.args.get("api_key") == API_KEY

# ============ API ENDPOINTS ============
@app.route("/")
def index():
    """API info endpoint"""
    return jsonify({
        "service": "AC Voice Command Server with TTS",
        "version": "6.1-TTS",
        "status": "ok",
        "model": "Gemini 2.5 Flash",
        "tts": "Google gTTS",
        "endpoints": {
            "POST /voice/command": "Process voice commands with Gemini AI + TTS",
            "POST /tts/speak": "Generate TTS audio from text"
        }
    })

@app.route("/voice/command", methods=["POST"])
def voice_command():
    """Endpoint xử lý lệnh giọng nói VÀ TRẢ VỀ AUDIO"""
    if not authenticate():
        return jsonify({"error": "Unauthorized"}), 401

    try:
        data = request.get_json(force=True)
        print(f"\n[VOICE] ========== New Voice Command ==========")
        
        voice_text = data.get("text", "")
        if not voice_text:
            return jsonify({"error": "Missing text field"}), 400
        
        print(f"[VOICE] User said: '{voice_text}'")
        
        temperature = data.get("temperature", 27)
        humidity = data.get("humidity", 65)
        ac_status = data.get("ac_status", False)
        ac_temp = data.get("ac_temp", 25)
        ac_mode = data.get("ac_mode", "COOL")
        ac_fan = data.get("ac_fan", "MEDIUM")
        
        context = f"""
User Voice Command: "{voice_text}"

Current Environment:
- Room Temperature: {temperature}°C
- Humidity: {humidity}%
- AC Status: {'ON' if ac_status else 'OFF'}
{f"- AC Settings: {ac_temp}°C, {ac_mode} mode, {ac_fan} fan" if ac_status else ""}

Analyze the user's command and provide appropriate AC control action.
"""
        
        text = call_gemini(VOICE_PROMPT, context)
        
        if not text:
            print("[VOICE] Gemini API failed, using fallback logic")
            fallback = analyze_voice_fallback(voice_text, temperature, ac_status, ac_temp)
            
            # Tạo audio từ fallback reason
            audio_file = generate_tts_audio(fallback['reason'])
            if audio_file:
                fallback['audio_url'] = f"/tts/audio/{os.path.basename(audio_file)}"
            
            return jsonify(fallback), 200
        
        parsed = extract_json(text)
        
        if parsed:
            print(f"[VOICE SUCCESS] ✓ Reason: {parsed['reason']}")
            
            # Tạo audio từ reason
            audio_file = generate_tts_audio(parsed['reason'])
            if audio_file:
                parsed['audio_url'] = f"/tts/audio/{os.path.basename(audio_file)}"
            
            return jsonify(parsed), 200
        else:
            fallback = analyze_voice_fallback(voice_text, temperature, ac_status, ac_temp)
            audio_file = generate_tts_audio(fallback['reason'])
            if audio_file:
                fallback['audio_url'] = f"/tts/audio/{os.path.basename(audio_file)}"
            return jsonify(fallback), 200

    except Exception as e:
        print(f"[VOICE ERROR] Exception: {e}")
        import traceback
        traceback.print_exc()
        return jsonify({"error": str(e)}), 500

@app.route("/test/gemini")
def test_gemini():
    """Test Gemini API connection"""
    try:
        payload = {
            "contents": [{
                "parts": [{"text": "Xin chào, trả lời bằng JSON: {\"status\": \"ok\", \"message\": \"Xin chào\"}"}]
            }],
            "generationConfig": {
                "temperature": 0.1,
                "maxOutputTokens": 100
            },
            "safetySettings": [
                {"category": "HARM_CATEGORY_HARASSMENT", "threshold": "BLOCK_NONE"},
                {"category": "HARM_CATEGORY_HATE_SPEECH", "threshold": "BLOCK_NONE"},
                {"category": "HARM_CATEGORY_SEXUALLY_EXPLICIT", "threshold": "BLOCK_NONE"},
                {"category": "HARM_CATEGORY_DANGEROUS_CONTENT", "threshold": "BLOCK_NONE"}
            ]
        }
        
        res = requests.post(
            f"{GEMINI_URL}?key={GEMINI_KEY}",
            json=payload,
            timeout=10
        )
        
        if res.status_code != 200:
            return jsonify({
                "error": f"HTTP {res.status_code}",
                "details": res.text[:500]
            }), 500
        
        resp = res.json()
        return jsonify({
            "success": True,
            "model": "gemini-1.5-flash",
            "response": resp
        })
        
    except Exception as e:
        return jsonify({
            "error": str(e)
        }), 500

@app.route("/tts/speak", methods=["POST"])
def tts_speak():
    """Endpoint tạo audio từ text"""
    if not authenticate():
        return jsonify({"error": "Unauthorized"}), 401
    
    try:
        data = request.get_json(force=True)
        text = data.get("text", "")
        
        if not text:
            return jsonify({"error": "Missing text field"}), 400
        
        audio_file = generate_tts_audio(text)
        if not audio_file:
            return jsonify({"error": "Failed to generate audio"}), 500
        
        return jsonify({
            "success": True,
            "audio_url": f"/tts/audio/{os.path.basename(audio_file)}"
        })
        
    except Exception as e:
        print(f"[TTS ERROR] {e}")
        return jsonify({"error": str(e)}), 500

@app.route("/tts/audio/<filename>")
def serve_audio(filename):
    """Phục vụ file audio"""
    try:
        file_path = os.path.join(TTS_CACHE_DIR, filename)
        if not os.path.exists(file_path):
            return jsonify({"error": "File not found"}), 404
        return send_file(file_path, mimetype='audio/mpeg')
    except Exception as e:
        return jsonify({"error": str(e)}), 500

@app.after_request
def after_request(response):
    """Add CORS headers"""
    response.headers["Access-Control-Allow-Origin"] = "*"
    response.headers["Access-Control-Allow-Headers"] = "Content-Type, Authorization"
    response.headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS"
    return response

if __name__ == "__main__":
    print("=" * 70)
    print("🎤 AC Voice Command Server v6.1 - WITH TTS")
    print("=" * 70)
    print("📡 Server: http://0.0.0.0:5000")
    print("🤖 AI Model: Gemini 2.5 Flash")
    print("🔊 TTS Engine: Google gTTS (Vietnamese)")
    print("\n📋 Endpoints:")
    print("  POST /voice/command  - Voice commands + Auto TTS")
    print("  POST /tts/speak      - Generate TTS audio")
    print("  GET  /tts/audio/<id> - Serve audio files")
    print("\n✨ Features:")
    print("  ✓ Smart voice command parsing")
    print("  ✓ Auto text-to-speech responses")
    print("  ✓ Sensor-based auto adjustment")
    print("  ✓ Null-safe JSON parsing")
    print("=" * 70)
    app.run(host="0.0.0.0", port=5000, debug=True)