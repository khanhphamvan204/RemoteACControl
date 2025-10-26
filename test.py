import os
import requests
from gtts import gTTS
import pygame

TEXT = "Mình đã điều chỉnh nhiệt độ cho bạn rồi nhé"

# ====== Hàm phát âm thanh ổn định ======
def play_audio(file_path):
    try:
        pygame.mixer.init()
        pygame.mixer.music.load(file_path)
        pygame.mixer.music.play()
        while pygame.mixer.music.get_busy():
            pygame.time.Clock().tick(10)
    except Exception as e:
        print("⚠️ Không thể phát bằng pygame:", e)
        try:
            os.startfile(file_path)  # fallback Windows
        except:
            print("❌ Không thể phát file, vui lòng mở thủ công:", file_path)

# ====== 1️⃣ Google gTTS ======
def test_gtts():
    print("▶ Test Google gTTS...")
    try:
        tts = gTTS(TEXT, lang='vi')
        file = "tts_gtts.mp3"
        tts.save(file)
        play_audio(file)
        print("✅ Google gTTS hoàn thành.\n")
    except Exception as e:
        print("❌ Lỗi gTTS:", e)

# ====== MAIN ======
if __name__ == "__main__":
    test_gtts()
