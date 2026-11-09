import os
import asyncio
import datetime
import time
import re
import threading
import requests
import syncedlyrics
from winsdk.windows.media.control import (
    GlobalSystemMediaTransportControlsSessionManager as MediaManager,
    GlobalSystemMediaTransportControlsSessionPlaybackStatus as PlaybackStatus
)

# Replace with your ESP32's local IP address (shown on OLED for 10s at boot)
ESP32_IP = os.getenv("ESP32_IP", "192.168.1.XX")
ESP32_URL = f"http://{ESP32_IP}/update"

SYNC_OFFSET_SECONDS = 0.5  # Look-ahead offset in seconds for smooth transition

http_session = requests.Session()

current_track_key = ""
lyrics_array = []
last_sent_index = -1
is_paused = False

# Smooth continuous monotonic clock state
song_start_mono = 0.0
last_raw_win_pos = -1.0
last_win_change_mono = 0.0

def _post_worker(line1, line2):
    try:
        payload = {'line1': line1, 'line2': line2}
        http_session.post(ESP32_URL, data=payload, timeout=0.6)
    except Exception:
        pass

def send_to_esp32(line1, line2=""):
    """Sends HTTP update in a background thread so the lyric timer never freezes."""
    threading.Thread(
        target=_post_worker,
        args=(line1.strip(), line2.strip()),
        daemon=True
    ).start()

def parse_lrc(lrc_text):
    result = []
    time_regex = re.compile(r'\[(\d+):(\d+)(?:[\.:](\d+))?\]')
    
    for line in lrc_text.split('\n'):
        matches = list(time_regex.finditer(line))
        if matches:
            text = time_regex.sub('', line).strip()
            if not text:
                continue
            for match in matches:
                minutes = int(match.group(1))
                seconds = int(match.group(2))
                ms_str = match.group(3) or "0"
                ms = int(ms_str.ljust(3, '0')[:3])
                total_seconds = (minutes * 60) + seconds + (ms / 1000.0)
                result.append({'time': total_seconds, 'text': text})
                
    return sorted(result, key=lambda x: x['time'])

def clean_metadata(title, artist):
    if " - " in title and (not artist or artist.lower() in ["youtube music", "youtube", "chrome"]):
        parts = title.split(" - ", 1)
        artist = parts[0].strip()
        title = parts[1].strip()

    c_title = re.sub(r'\(.*?\)|\[.*?\]', '', title)
    noise = r'(?i)\b(official\s*(music\s*)?(video|audio)|visualizer|lyrics?|remastered|version|topic|hd|hq)\b'
    c_title = re.sub(noise, '', c_title)
    c_title = re.sub(r'\s+', ' ', c_title).strip(' -|')

    c_artist = re.split(r'[,&•/]', artist)[0].strip()
    c_artist = re.sub(r'(?i)\s*-\s*topic$', '', c_artist).strip()

    return c_title, c_artist

def fetch_lyrics(title, artist):
    global lyrics_array, last_sent_index
    lyrics_array = []
    last_sent_index = -1

    clean_title, clean_artist = clean_metadata(title, artist)
    search_term = f"{clean_title} {clean_artist}".strip()

    print(f"\n[Track] '{clean_title}' by '{clean_artist}'")
    print(f"[Searching Synced Lyrics] '{search_term}'...")

    try:
        lrc_data = syncedlyrics.search(search_term, synced_only=True)
        if not lrc_data and clean_title:
            lrc_data = syncedlyrics.search(clean_title, synced_only=True)

        if lrc_data:
            lyrics_array = parse_lrc(lrc_data)
            if lyrics_array:
                print(f"[Success] Loaded {len(lyrics_array)} lines!")
                return
    except Exception as e:
        print(f"[Search Warning]: {e}")

    headers = {"User-Agent": "ESP32LyricBridge/3.1"}
    try:
        url = f"https://lrclib.net/api/search?q={requests.utils.quote(search_term)}"
        res = requests.get(url, headers=headers, timeout=4)
        if res.status_code == 200:
            for item in res.json():
                if item.get("syncedLyrics"):
                    lyrics_array = parse_lrc(item["syncedLyrics"])
                    print(f"[Success via LRCLIB] Loaded {len(lyrics_array)} lines!")
                    return
    except Exception:
        pass

    print("[Fallback] No synced lyrics found. Showing Title & Artist.")
    send_to_esp32(clean_title, clean_artist)

def get_smooth_position(timeline):
    global song_start_mono, last_raw_win_pos, last_win_change_mono

    now = time.monotonic()

    if timeline and timeline.position:
        raw_pos = timeline.position.total_seconds()
        if raw_pos > 0 and abs(raw_pos - last_raw_win_pos) > 0.01:
            last_raw_win_pos = raw_pos
            last_win_change_mono = now

            true_pos = raw_pos
            if timeline.last_updated_time:
                now_utc = datetime.datetime.now(datetime.timezone.utc)
                delta = (now_utc - timeline.last_updated_time).total_seconds()
                if 0 <= delta < 5.0:
                    true_pos += delta

            current_internal = now - song_start_mono
            if abs(current_internal - true_pos) > 1.2:
                song_start_mono = now - true_pos

    return max(0.0, (now - song_start_mono) + SYNC_OFFSET_SECONDS)

async def main():
    global current_track_key, last_sent_index, is_paused
    global song_start_mono, last_raw_win_pos

    manager = await MediaManager.request_async()
    pause_started_mono = 0.0

    print("=" * 55)
    print(" Smooth Non-Blocking Lyrics Bridge Running")
    print(" Target Endpoint:", ESP32_URL)
    print("=" * 55)

    while True:
        try:
            session = manager.get_current_session()
            if session:
                info = await session.try_get_media_properties_async()
                timeline = session.get_timeline_properties()
                playback_info = session.get_playback_info()

                title = info.title.strip() if (info and info.title) else ""
                artist = info.artist.strip() if (info and info.artist) else ""

                if not title:
                    await asyncio.sleep(0.1)
                    continue

                status = playback_info.playback_status if playback_info else None
                if status == PlaybackStatus.PAUSED:
                    if not is_paused:
                        is_paused = True
                        pause_started_mono = time.monotonic()
                        send_to_esp32("|| Paused", title)
                    await asyncio.sleep(0.2)
                    continue
                else:
                    if is_paused:
                        is_paused = False
                        song_start_mono += (time.monotonic() - pause_started_mono)
                        last_sent_index = -1

                track_key = f"{title} - {artist}"

                if track_key != current_track_key:
                    current_track_key = track_key
                    now = time.monotonic()
                    win_initial = timeline.position.total_seconds() if (timeline and timeline.position) else 0.0
                    
                    if win_initial > 15.0:
                        win_initial = 0.0
                        
                    song_start_mono = now - win_initial
                    last_raw_win_pos = win_initial
                    last_sent_index = -1
                    
                    c_title, c_artist = clean_metadata(title, artist)
                    send_to_esp32("> " + c_title, c_artist)
                    fetch_lyrics(title, artist)

                if lyrics_array:
                    current_sec = get_smooth_position(timeline)

                    active_idx = -1
                    for idx, line in enumerate(lyrics_array):
                        if current_sec >= line['time']:
                            active_idx = idx
                        else:
                            break

                    if active_idx != -1 and active_idx != last_sent_index:
                        last_sent_index = active_idx
                        current_line = lyrics_array[active_idx]['text']
                        next_line = lyrics_array[active_idx + 1]['text'] if (active_idx + 1 < len(lyrics_array)) else "---"

                        print(f"[{int(current_sec // 60):02d}:{int(current_sec % 60):02d}] {current_line}")
                        send_to_esp32(current_line, next_line)

        except Exception:
            pass

        await asyncio.sleep(0.08)

if __name__ == "__main__":
    asyncio.run(main())
