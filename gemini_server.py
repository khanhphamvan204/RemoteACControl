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
GEMINI_KEY = "AIzaSyDCHnIMawPafYPbtHAppUXWSE1_mJlcuII"
GEMINI_URL = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent"

# Cache directory for TTS files
TTS_CACHE_DIR = tempfile.gettempdir() + "/tts_cache"
os.makedirs(TTS_CACHE_DIR, exist_ok=True)

# ============ VOICE COMMAND PROMPT ============
VOICE_PROMPT = """
You are "Trợ lý AI" - a friendly Vietnamese-speaking AC voice assistant. Analyze user's voice command and provide appropriate AC control.

IMPORTANT: Respond with VALID JSON ONLY (no markdown, no code blocks):
{
  "action": "turn_on|turn_off|adjust|maintain",
  "temperature": <16-30>,
  "fan_speed": "QUIET|LOW|MEDIUM|HIGH|AUTO",
  "mode": "COOL|DRY|FAN|HEAT|AUTO",
  "reason": "<friendly Vietnamese explanation, 50-100 chars>"
}

Understanding Commands:
- Turn on: "bật", "mở", "turn on", "start", "power on"
- Turn off: "tắt", "turn off", "stop", "power off"
- Cooler: "mát hơn", "lạnh hơn", "cooler", "colder", "giảm nhiệt"
- Warmer: "ấm hơn", "warmer", "tăng nhiệt"
- Hot: "nóng", "hot" → Turn on with LOW temp (22-23°C)
- Cold: "lạnh", "cold" → Turn off or INCREASE temp
- Humid: "ẩm", "humid" → Use DRY mode
- Set temp: "24 độ", "set to 24", "24 degrees"
- Quiet: "yên tĩnh", "quiet", "im" → QUIET fan
- Strong: "mạnh", "strong", "nhanh" → HIGH fan

Response Style (Vietnamese):
- Use friendly tone with "mình" (I) and "bạn" (you)
- Example: "Bạn nói nóng quá nên mình đã bật điều hòa ở 22°C với quạt mạnh để làm mát nhanh cho bạn nhé!"
- Be conversational and warm
- Explain what you did and why
- Keep explanations concise but friendly (50-100 chars)

Smart Context Awareness:
- If AC is OFF and user says "cooler" → Turn ON with low temp
- If AC is ON and user says "warmer" → Increase temp or turn OFF
- Consider current temperature when deciding
- If unclear, choose safe comfortable defaults (25°C, MEDIUM fan)

Fan Speed Selection:
- QUIET: When user wants silence or it's night time
- LOW: Small adjustments, energy saving
- MEDIUM: Default, balanced
- HIGH: Quick cooling, hot conditions
- AUTO: When unsure
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
                # "maxOutputTokens": 512
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
            print(f"[GEMINI ERROR] HTTP {res.status_code}: {res.text[:300]}")
            return None

        resp = res.json()
        
        if "candidates" not in resp or len(resp["candidates"]) == 0:
            print(f"[GEMINI ERROR] No candidates in response")
            return None
        
        candidate = resp["candidates"][0]
        finish = candidate.get("finishReason", "")
        
        if finish == "SAFETY":
            print("[GEMINI WARN] Response blocked by safety filters")
            if retry:
                print("[GEMINI] Retrying with modified prompt...")
                return call_gemini(prompt, "Please provide a safe and helpful response. " + user_message, retry=False)
            return None
        
        if "content" not in candidate or "parts" not in candidate["content"]:
            print(f"[GEMINI ERROR] No content/parts in response")
            return None
        
        parts = candidate["content"]["parts"]
        if len(parts) == 0 or "text" not in parts[0]:
            print(f"[GEMINI ERROR] No text in parts")
            return None
        
        text = parts[0]["text"].strip()
        print(f"[GEMINI SUCCESS] Got response: {text[:200]}...")
        
        return text

    except requests.Timeout:
        print("[GEMINI ERROR] Request timeout")
        return None
    except Exception as e:
        print(f"[GEMINI ERROR] Exception: {e}")
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
        
        if "action" not in parsed:
            parsed["action"] = "maintain"
        if "temperature" not in parsed:
            parsed["temperature"] = 25
        if "fan_speed" not in parsed:
            parsed["fan_speed"] = "MEDIUM"
        if "mode" not in parsed:
            parsed["mode"] = "COOL"
        if "reason" not in parsed:
            parsed["reason"] = "Đã xử lý yêu cầu của bạn!"
        
        fan_str = str(parsed["fan_speed"]).upper()
        if fan_str not in ["QUIET", "LOW", "MEDIUM", "HIGH", "AUTO"]:
            parsed["fan_speed"] = "MEDIUM"
        else:
            parsed["fan_speed"] = fan_str
        
        mode_str = str(parsed["mode"]).upper()
        if mode_str not in ["COOL", "DRY", "FAN", "HEAT", "AUTO"]:
            parsed["mode"] = "COOL"
        else:
            parsed["mode"] = mode_str
        
        parsed["temperature"] = max(16, min(30, int(parsed["temperature"])))
        
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
        "version": "6.0-TTS",
        "status": "ok",
        "model": "Gemini 2.0 Flash Experimental",
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
    print("🎤 AC Voice Command Server v6.0 - WITH TTS")
    print("=" * 70)
    print("📡 Server: http://0.0.0.0:5000")
    print("🤖 AI Model: Gemini 2.0 Flash Experimental")
    print("🔊 TTS Engine: Google gTTS (Vietnamese)")
    print("\n📋 Endpoints:")
    print("  POST /voice/command  - Voice commands + Auto TTS")
    print("  POST /tts/speak      - Generate TTS audio")
    print("  GET  /tts/audio/<id> - Serve audio files")
    print("\n✨ NEW: Auto text-to-speech for all responses!")
    print("=" * 70)
    app.run(host="0.0.0.0", port=5000, debug=True)