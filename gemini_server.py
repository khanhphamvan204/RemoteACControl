from flask import Flask, request, jsonify
import requests
import json

app = Flask(__name__)

API_KEY = "AC_SECRET_KEY_2024_LLM"
GEMINI_KEY = "AIzaSyBe7YTTbbJeuEAP1tBkaYhHcuOg7tihvBw"
GEMINI_URL = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash-exp:generateContent"

# System prompt NGẮN GỌN
SYSTEM_PROMPT = """Bạn là AI điều khiển AC. Phân tích sensor và trả về JSON:
{"action":"turn_on|turn_off|adjust|maintain","temperature":25,"fan_speed":2,"mode":"COOL","reason":"lý do ngắn"}"""

def authenticate():
    auth = request.headers.get("Authorization", "")
    if auth.startswith("Bearer "):
        return auth.split(" ")[1] == API_KEY
    return request.args.get("api_key") == API_KEY

@app.route("/")
def index():
    return jsonify({"service": "Gemini Proxy", "version": "1.2", "status": "ok"})

@app.route("/llm/query", methods=["POST"])
def llm_query():
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

        # Tạo payload tối ưu
        payload = {
            "contents": [{
                "parts": [{"text": f"{SYSTEM_PROMPT}\n\n{user_query}"}]
            }],
            "generationConfig": {
                "temperature": 0.3,
                "maxOutputTokens": 256,  # Giảm xuống chỉ đủ cho JSON
                "topP": 0.8
            }
        }

        print(f"[INFO] Query: {user_query[:100]}...")
        
        # Call Gemini
        res = requests.post(
            f"{GEMINI_URL}?key={GEMINI_KEY}",
            json=payload,
            timeout=20
        )
        
        if res.status_code != 200:
            print(f"[ERROR] HTTP {res.status_code}: {res.text[:300]}")
            return jsonify({"error": f"Gemini error {res.status_code}"}), 500

        resp = res.json()
        
        # Check candidates
        if "candidates" not in resp or len(resp["candidates"]) == 0:
            print(f"[ERROR] No candidates")
            return jsonify({"error": "No response from Gemini"}), 500
        
        candidate = resp["candidates"][0]
        finish = candidate.get("finishReason", "")
        
        print(f"[DEBUG] Finish: {finish}")
        
        # Handle finish reasons
        if finish == "SAFETY":
            return jsonify({"error": "Blocked by safety"}), 400
        
        if finish == "MAX_TOKENS":
            print("[WARN] Hit max tokens")
        
        # Extract text
        if "content" not in candidate or "parts" not in candidate["content"]:
            print(f"[ERROR] No content/parts")
            return jsonify({"error": "Invalid response structure"}), 500
        
        parts = candidate["content"]["parts"]
        if len(parts) == 0 or "text" not in parts[0]:
            print(f"[ERROR] No text in parts")
            return jsonify({"error": "Empty response"}), 500
        
        text = parts[0]["text"].strip()
        print(f"[SUCCESS] Response: {text[:150]}...")
        
        # Parse JSON from text
        try:
            # Tìm JSON
            start = text.find('{')
            end = text.rfind('}')
            if start != -1 and end != -1:
                json_str = text[start:end+1]
                parsed = json.loads(json_str)
                print(f"[SUCCESS] Action: {parsed.get('action')}")
                return jsonify(parsed), 200
            else:
                return jsonify({"raw_text": text}), 200
        except json.JSONDecodeError:
            return jsonify({"raw_text": text}), 200

    except requests.Timeout:
        return jsonify({"error": "Timeout"}), 504
    except Exception as e:
        print(f"[ERROR] {e}")
        return jsonify({"error": str(e)}), 500

@app.route("/llm/control", methods=["POST"])
def llm_control():
    if not authenticate():
        return jsonify({"error": "Unauthorized"}), 401
    return llm_query()

@app.after_request
def after_request(response):
    response.headers["Access-Control-Allow-Origin"] = "*"
    response.headers["Access-Control-Allow-Headers"] = "Content-Type, Authorization"
    response.headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS"
    return response

if __name__ == "__main__":
    print("🚀 Gemini Proxy v1.2")
    print("📡 http://0.0.0.0:5000")
    app.run(host="0.0.0.0", port=5000, debug=True)