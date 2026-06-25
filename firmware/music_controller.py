import serial
import subprocess
import time
import re

PORT = "/dev/ttyACM0" 
BAUD = 115200

SPEAKER_MAC = "41:42:E1:EE:2D:F4" #HARDCODED SPEAKER MAC ADDRESS, HAVE TO FIND THiS BEFORE PAIRING

PP_WINDOW = 0.8
TRIPLE_PRESS = 3

RADIO_STATIONS = [
    ("FIP",            "https://icecast.radiofrance.fr/fip-midfi.mp3"),
    ("FIP Rock",       "https://icecast.radiofrance.fr/fiprock-midfi.mp3"),
    ("FIP Jazz",       "https://icecast.radiofrance.fr/fipjazz-midfi.mp3"),
    ("FIP Groove",     "https://icecast.radiofrance.fr/fipgroove-midfi.mp3"),
    ("FIP Monde",      "https://icecast.radiofrance.fr/fipworld-midfi.mp3"),
    ("FIP Nouveautes", "https://icecast.radiofrance.fr/fipnouveautes-midfi.mp3"),
    ("FIP Reggae",     "https://icecast.radiofrance.fr/fipreggae-midfi.mp3"),
    ("FIP Electro",    "https://icecast.radiofrance.fr/fipelectro-midfi.mp3"),
    ("TSF Jazz",       "https://tsfjazz.ice.infomaniak.ch/tsfjazz-high.mp3"),
]
#Customizable radio stations, but theser are the ones i found

VALID_COMMANDS = {
    "PLAY_PAUSE",
    "NEXT",
    "PREV",
    "VOL_UP",
    "VOL_DOWN",
    "BT_CONNECT",
    # Service menu (triggered by holding NEXT on the remote)
    "SVC_BT",        # disconnect + reconnect the Bluetooth speaker
    "SVC_SPOTIFY",   # restart the spotifyd user service
    "SVC_RADIO",     # restart the current radio stream
    "SVC_WIFI",      # bounce Wi-Fi
    "SVC_SHUTDOWN",  # safe power off
    "SVC_REBOOT",    # reboot
}

# ----- runtime state -----
radio_mode = False
radio_index = 0
radio_player = None       

pp_count = 0              
pp_deadline = 0.0     


def log(*args):
    print(*args, flush=True)


def run(command):
    return subprocess.run(
        command,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )


def connect_bluetooth_speaker():
    log(f"Trying to connect Bluetooth speaker: {SPEAKER_MAC}")

    run(["bluetoothctl", "power", "on"])
    run(["bluetoothctl", "trust", SPEAKER_MAC])

    result = subprocess.run(
        ["bluetoothctl", "connect", SPEAKER_MAC],
        capture_output=True,
        text=True
    )

    output = (result.stdout + result.stderr).lower()

    if "connection successful" in output or "already connected" in output:
        log("Bluetooth speaker connected.")
        return True

    log("Bluetooth connect output:")
    log(result.stdout.strip())
    log(result.stderr.strip())
    return False


# ----- Spotify (playerctl / spotifyd) -----
def spotify_action(action):
    result = subprocess.run(
        ["playerctl", action],
        capture_output=True,
        text=True
    )
    if result.returncode != 0:
        log("playerctl error:", result.stderr.strip())


# ----- Radio (mpv) -----
def radio_is_playing():
    return radio_player is not None and radio_player.poll() is None


def radio_stop():
    global radio_player
    if radio_is_playing():
        radio_player.terminate()
        try:
            radio_player.wait(timeout=2)
        except subprocess.TimeoutExpired:
            radio_player.kill()
    radio_player = None


def radio_start(index):
    global radio_player, radio_index
    radio_stop()
    radio_index = index % len(RADIO_STATIONS)
    name, url = RADIO_STATIONS[radio_index]
    log(f"Radio: playing {name} ({url})")
    radio_player = subprocess.Popen(
        ["mpv", "--no-video", "--really-quiet", url],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )


def radio_next():
    radio_start(radio_index + 1)


def radio_prev():
    radio_start(radio_index - 1)


def radio_play_pause():
    if radio_is_playing():
        log("Radio: stop")
        radio_stop()
    else:
        log("Radio: resume")
        radio_start(radio_index)


def toggle_radio_mode():
    global radio_mode
    if not radio_mode:
        log("=== Switching to RADIO mode ===")
        radio_mode = True
        spotify_action("pause")          # quiet Spotify before radio starts
        radio_start(radio_index)
    else:
        log("=== Switching to SPOTIFY mode ===")
        radio_mode = False
        radio_stop()
        spotify_action("play")           # resume Spotify playback


# ----- PLAY_PAUSE burst handling -----
def register_play_pause():
    """Record a PLAY_PAUSE press and (re)arm the burst evaluation window."""
    global pp_count, pp_deadline
    pp_count += 1
    pp_deadline = time.monotonic() + PP_WINDOW


def flush_play_pause():
    """Evaluate a completed PLAY_PAUSE burst."""
    global pp_count, pp_deadline
    n = pp_count
    pp_count = 0
    pp_deadline = 0.0
    if n <= 0:
        return
    if n >= TRIPLE_PRESS:
        toggle_radio_mode()
    else:
        # 1 or 2 presses -> a single play/pause toggle in the current mode
        if radio_mode:
            radio_play_pause()
        else:
            spotify_action("play-pause")


# ----- Service menu actions -----
def svc_reconnect_bt():
    log("Service: reconnect Bluetooth speaker")
    run(["bluetoothctl", "disconnect", SPEAKER_MAC])
    time.sleep(2)
    connect_bluetooth_speaker()


def svc_restart_spotify():
    log("Service: restart spotifyd")
    run(["systemctl", "--user", "restart", "spotifyd"])


def svc_restart_radio():
    log("Service: restart radio stream")
    if radio_mode:
        radio_start(radio_index)        # radio_start() stops any current stream first
    else:
        log("(not in radio mode; nothing to restart)")


def svc_restart_wifi():
    log("Service: bounce Wi-Fi")
    run(["nmcli", "radio", "wifi", "off"])
    time.sleep(2)
    run(["nmcli", "radio", "wifi", "on"])


def svc_power(action):
    """Safe shutdown / reboot. Needs the passwordless sudoers rule (see setup)."""
    log(f"Service: {action}")
    radio_stop()                      
    r = subprocess.run(
        ["sudo", "-n", "systemctl", action],
        capture_output=True, text=True
    )
    if r.returncode != 0:
        log(f"{action} failed (is the sudoers rule installed?):",
            r.stderr.strip())


def handle_command(line):
    """Handle any non-PLAY_PAUSE command. Pending bursts are flushed first."""
    flush_play_pause()

    if line == "NEXT":
        if radio_mode:
            radio_next()
        else:
            spotify_action("next")

    elif line == "PREV":
        if radio_mode:
            radio_prev()
        else:
            spotify_action("previous")

    elif line == "VOL_UP":
        run(["pactl", "set-sink-volume", "@DEFAULT_SINK@", "+5%"])

    elif line == "VOL_DOWN":
        run(["pactl", "set-sink-volume", "@DEFAULT_SINK@", "-5%"])

    elif line == "BT_CONNECT":
        connect_bluetooth_speaker()

    elif line == "SVC_BT":
        svc_reconnect_bt()

    elif line == "SVC_SPOTIFY":
        svc_restart_spotify()

    elif line == "SVC_RADIO":
        svc_restart_radio()

    elif line == "SVC_WIFI":
        svc_restart_wifi()

    elif line == "SVC_SHUTDOWN":
        svc_power("poweroff")

    elif line == "SVC_REBOOT":
        svc_power("reboot")



# The Pi pushes the current state back over the same serial line as a single
# pipe-delimited line:  ST|<mode>|<state>|<line1>|<line2>|<volume%>
# e.g.  ST|SPOTIFY|PLAYING|Get Lucky|Daft Punk|63
STATUS_INTERVAL = 2.0     # seconds between periodic status pushes
last_status = 0.0


def _clean(text):
    return text.replace("|", "/").replace("\n", " ").replace("\r", " ").strip()


def get_volume_percent():
    r = subprocess.run(
        ["pactl", "get-sink-volume", "@DEFAULT_SINK@"],
        capture_output=True, text=True
    )
    m = re.search(r"(\d+)%", r.stdout)
    return m.group(1) if m else "?"


def get_spotify_meta():
    r = subprocess.run(
        ["playerctl", "metadata", "--format", "{{status}}\t{{artist}}\t{{title}}"],
        capture_output=True, text=True
    )
    if r.returncode != 0:
        return ("STOPPED", "", "")
    parts = (r.stdout.strip().split("\t") + ["", "", ""])[:3]
    return (parts[0].upper(), parts[1], parts[2])


def send_status():
    vol = get_volume_percent()
    if radio_mode:
        mode = "RADIO"
        state = "PLAYING" if radio_is_playing() else "PAUSED"
        line1, line2 = RADIO_STATIONS[radio_index][0], "Internet Radio"
    else:
        mode = "SPOTIFY"
        status, artist, title = get_spotify_meta()
        state = "PLAYING" if status == "PLAYING" else "PAUSED"
        line1, line2 = title, artist
    msg = "ST|{}|{}|{}|{}|{}\n".format(
        mode, state, _clean(line1), _clean(line2), vol
    )
    try:
        ser.write(msg.encode("utf-8", errors="ignore"))
    except Exception as e:
        log("status write error:", e)


log("Connecting to Bluetooth speaker...")
connect_bluetooth_speaker()

log("Opening serial port...")
ser = serial.Serial(PORT, BAUD, timeout=0.2)

time.sleep(2)
ser.reset_input_buffer()

log("ESP32 controller ready (Spotify mode).")
log("Triple-press PLAY_PAUSE to toggle radio mode.")
send_status()

try:
    while True:
        raw = ser.readline()
        line = raw.decode(errors="ignore").strip()

        if line in VALID_COMMANDS:
            log("Command:", line)
            if line == "PLAY_PAUSE":
                register_play_pause()
            else:
                handle_command(line)
                send_status()
        elif line:
            log(f"Ignored junk/partial command: {line}")

        if pp_count > 0 and time.monotonic() >= pp_deadline:
            flush_play_pause()
            send_status()

        now = time.monotonic()
        if now - last_status >= STATUS_INTERVAL:
            send_status()
            last_status = now

except KeyboardInterrupt:
    log("Stopping...")
finally:
    radio_stop()
    ser.close()
