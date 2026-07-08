"""Qonur AI controller.

Records speech from the microphone (push-to-talk with Enter), sends it to
Gemini for a spoken reply, plays the TTS audio, and drives the bear's mouth
and eye servos over USB serial to the ESP32.

Run:
    export GEMINI_API_KEY=your_key
    python main.py

Requires an ESP32 flashed with firmware/qonur_firmware and connected via USB.
"""

import io
import json
import os
import queue
import sys
import threading
import time

import numpy as np
import pygame
import serial
import sounddevice as sd
import soundfile as sf
from scipy.io.wavfile import write
from google import genai
from google.genai import types

GOOGLE_API_KEY = os.environ.get("GEMINI_API_KEY")
if not GOOGLE_API_KEY:
    sys.exit("GEMINI_API_KEY is not set. Run: export GEMINI_API_KEY=your_key")
client = genai.Client(api_key=GOOGLE_API_KEY)

ESP32_PORT = "/dev/ttyUSB0"
ESP32_BAUDRATE = 115200
esp32_serial = None

# PCA9685 channels driven from Python. The remaining channels (arms, head)
# are handled by the firmware and the Blynk app; see firmware header for the
# full channel map.
SERVO_MAPPINGS = {
    "eye_right": 7,
    "eye_left": 8,
    "mouth": 9,
}


def connect_esp32():
    global esp32_serial
    try:
        esp32_serial = serial.Serial(ESP32_PORT, ESP32_BAUDRATE, timeout=1)
        time.sleep(2)  # ESP32 resets on serial open; give it time to boot
        print(f"Connected to ESP32 on {ESP32_PORT}")
        return True
    except Exception as e:
        print(f"Failed to connect to ESP32: {e}")
        return False


def send_servo_command(channel, angle, duration=0.5):
    if esp32_serial is None:
        print("ESP32 not connected")
        return False
    angle = max(0, min(180, angle))
    cmd = json.dumps({"servo": channel, "angle": angle, "duration": duration}) + "\n"
    esp32_serial.write(cmd.encode())
    return True


def set_led_listening_state(is_listening):
    if esp32_serial:
        esp32_serial.write((json.dumps({"listening": is_listening}) + "\n").encode())


def close_mouth():
    send_servo_command(SERVO_MAPPINGS["mouth"], 90, 0.3)


def animate_mouth_during_speech(audio_duration):
    if audio_duration <= 0:
        return
    # Fixed open/close pattern spread over the estimated speech length.
    mouth_positions = [70, 85, 75, 90, 80, 75, 85, 70, 80, 90]
    dur = max(0.1, audio_duration / len(mouth_positions))
    for i, pos in enumerate(mouth_positions):
        if i * dur >= audio_duration:
            break
        send_servo_command(SERVO_MAPPINGS["mouth"], pos, dur)
        time.sleep(dur)
    close_mouth()


def estimate_audio_duration(audio_data, sample_rate=24000, channels=1):
    # Gemini TTS returns mono 16-bit PCM at 24 kHz, so 2 bytes per sample.
    try:
        total_samples = len(audio_data) // (2 * channels)
        return max(0.5, total_samples / sample_rate)
    except Exception:
        return 2.0


# Mixer frequency must match the TTS sample rate or playback is pitch-shifted.
pygame.mixer.init(frequency=24000, size=-16, channels=2, buffer=512)
response_cache = {}


def get_cache_key(text):
    return hash(text.strip().lower())


def cache_response(text, audio_data, mime_type):
    response_cache[get_cache_key(text)] = (audio_data, mime_type)


def get_cached_response(text):
    return response_cache.get(get_cache_key(text))


def record_audio(fs=16000, channels=1):
    """Record from the microphone between two Enter presses.

    Returns the recording as WAV bytes. The listening LED stays on after
    recording stops; the main loop turns it off when the reply is ready, so
    the bear never looks frozen while the cloud is thinking.
    """
    print("Press Enter to start recording...")
    input()
    print("Recording... press Enter again to stop.")
    set_led_listening_state(True)

    frames = []

    def callback(indata, frame_count, time_info, status):
        frames.append(indata.copy())

    with sd.InputStream(samplerate=fs, channels=channels, dtype="int16", callback=callback):
        input()

    audio_data = np.concatenate(frames, axis=0)
    buf = io.BytesIO()
    write(buf, fs, audio_data)
    return buf.getvalue()


def retry_with_backoff(func, max_retries=2, base_delay=1.0, **kwargs):
    last_exc = None
    for i in range(max_retries):
        try:
            return func(**kwargs)
        except Exception as e:
            last_exc = e
            if i < max_retries - 1:
                time.sleep(base_delay * (2**i))
    raise last_exc or RuntimeError("Max retries exceeded")


def convert_and_play_audio(audio_data: bytes):
    try:
        arr = np.frombuffer(audio_data, dtype=np.int16)
        # Duplicate mono to stereo for the mixer.
        arr = np.column_stack((arr, arr))
        buf = io.BytesIO()
        sf.write(buf, arr, samplerate=24000, format="WAV")
        buf.seek(0)
        pygame.mixer.music.load(buf)
        pygame.mixer.music.play()
        while pygame.mixer.music.get_busy():
            time.sleep(0.1)
    except Exception as e:
        print(f"Playback error: {e}")


# The bear's personality lives here. Write your own prompt (character, tone,
# language, scenario rules) and you have your own bear.
actor_prompt = """PROMPT"""

# Strict blocking on all categories by default: the audience is children.
safety_settings = [
    types.SafetySetting(category=c, threshold="BLOCK_LOW_AND_ABOVE")
    for c in (
        "HARM_CATEGORY_HARASSMENT",
        "HARM_CATEGORY_HATE_SPEECH",
        "HARM_CATEGORY_SEXUALLY_EXPLICIT",
        "HARM_CATEGORY_DANGEROUS_CONTENT",
    )
]

chat_config = types.GenerateContentConfig(
    system_instruction=actor_prompt,
    safety_settings=safety_settings,
)

# Session-only conversation memory so the bear can hold a coherent multi-turn
# conversation. Nothing is written to disk. Audio turns are re-sent each
# request, so history is capped to bound cost and latency.
chat_history = []
MAX_HISTORY_ITEMS = 20  # 10 user/model turn pairs

tts_config = types.GenerateContentConfig(
    temperature=0.8,
    response_modalities=["audio"],
    speech_config=types.SpeechConfig(
        voice_config=types.VoiceConfig(
            prebuilt_voice_config=types.PrebuiltVoiceConfig(voice_name="Aoede")
        )
    ),
)


def process_tts_with_cache(text, result_queue):
    try:
        cached = get_cached_response(text)
        if cached:
            result_queue.put(cached)
            return
        tts_stream = retry_with_backoff(
            func=client.models.generate_content_stream,
            model="gemini-2.5-flash-preview-tts",
            contents=[types.Content(role="user", parts=[types.Part.from_text(text=text)])],
            config=tts_config,
        )
        audio_data, mime_type = b"", None
        for chunk in tts_stream:
            if chunk.candidates and chunk.candidates[0].content.parts[0].inline_data:
                part = chunk.candidates[0].content.parts[0].inline_data
                mime_type = mime_type or part.mime_type
                audio_data += part.data
        cache_response(text, audio_data, mime_type)
        result_queue.put((audio_data, mime_type))
    except Exception as e:
        print(f"TTS Error: {e}")
        result_queue.put((None, None))


def main():
    if not connect_esp32():
        print("Cannot start without ESP32")
        return

    # Neutral positions for the Python-controlled servos.
    send_servo_command(SERVO_MAPPINGS["eye_left"], 90)
    send_servo_command(SERVO_MAPPINGS["eye_right"], 90)
    send_servo_command(SERVO_MAPPINGS["mouth"], 90)
    print("Actor ready.")

    try:
        while True:
            wav_bytes = record_audio()
            print("Processing...")

            # Inline audio bytes instead of the Files API: saves one upload
            # round trip per turn, and turns are only seconds long anyway.
            audio_part = types.Part.from_bytes(data=wav_bytes, mime_type="audio/wav")
            chat_history.append(types.Content(role="user", parts=[audio_part]))
            del chat_history[:-MAX_HISTORY_ITEMS]

            response_stream = retry_with_backoff(
                func=client.models.generate_content_stream,
                model="gemini-2.5-flash",
                contents=chat_history,
                config=chat_config,
            )
            full_text = "".join(
                part.text
                for chunk in response_stream
                if chunk.candidates
                for part in chunk.candidates[0].content.parts
                if hasattr(part, "text")
            ).strip()
            print("AI:", full_text)

            if not full_text:
                # Safety block or empty candidate: drop the turn so a dead
                # entry does not poison the history.
                chat_history.pop()
                set_led_listening_state(False)
                print("No reply generated. Enter to record again, Ctrl+C to exit.")
                continue
            chat_history.append(
                types.Content(role="model", parts=[types.Part.from_text(text=full_text)])
            )

            tts_q = queue.Queue()
            process_tts_with_cache(full_text, tts_q)
            audio_data, _ = tts_q.get()
            set_led_listening_state(False)
            if audio_data:
                dur = estimate_audio_duration(audio_data)
                mouth_t = threading.Thread(target=animate_mouth_during_speech, args=(dur,))
                mouth_t.start()
                convert_and_play_audio(audio_data)
                mouth_t.join()

            print("Ready for next turn. Enter to record again, Ctrl+C to exit.")
    except KeyboardInterrupt:
        print("Shutting down.")
        close_mouth()
        if esp32_serial:
            esp32_serial.close()
        pygame.mixer.quit()


if __name__ == "__main__":
    main()
