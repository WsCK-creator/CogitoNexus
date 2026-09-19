# CogitoNexus Server

Unified C++ server for Booster K1 robot with:
- **TTS** via Meta's MMS-TTS Polish model, running as a persistent **Python
  worker** (see "TTS Architecture" below)
- **Audio streaming** (microphone → TCP)
- **Controller button events** (real-time subscription)
- **Dance execution** via B1LocoClient
- **JSON command protocol** (text + dance before/during/after, with optional
  per-request speed/volume)

## Structure

```
├── CMakeLists.txt
├── build.sh              # Build script for cogito_server (C++ side)
├── cogito-server.service # systemd service file
├── mms_worker.py         # Persistent MMS-TTS worker (Python side)
├── test_client.py        # Python test client
├── README.md
├── mms-model/            # MMS-TTS model, pre-downloaded locally (see below)
│   └── pl_PL/
├── tts_venv/             # Dedicated venv for transformers/torch/scipy (see below)
├── piper_worker.py       # RETIRED -- kept only in case of rollback, see
│                         # "Switching the TTS engine" below
├── piper-model/          # RETIRED Piper voice models (same reason)
│   └── pl_PL-bass-high.onnx
└── src/
    └── booster-CogitoNexus.cpp # Unified server
```

## TTS Architecture

TTS runs via **`mms_worker.py`**, a persistent subprocess `cogito_server`
spawns once and keeps running for the server's whole lifetime -- same
"engine-agnostic worker" design as before, just pointed at a different
engine and model:

- The model is **Meta's MMS-TTS Polish voice** (`facebook/mms-tts-pol`, a
  single-speaker VITS model), loaded via Hugging Face `transformers`
  (`VitsModel`/`AutoTokenizer`), NOT Piper. This replaced Piper entirely
  after listening tests: none of the available Piper `pl_PL` voices
  (`bass-high`, `darkman`, `gosia`, `mc_speech`) sounded right, and
  MMS-TTS's voice won out. It's the same VITS architecture family as
  Piper, so it keeps roughly the same speed/latency profile on this
  Jetson (near-realtime), just a different, separately-trained voice.
- The worker loads the model once, does one warm-up synthesis at startup
  (pays the one-time CUDA-context/first-kernel-launch cost up front), then
  prints `READY` and enters a loop: one line of text in via stdin, one WAV
  file path out via stdout -- the **exact same protocol** `piper_worker.py`
  used, which is why the C++ `PiperTTS` class (kept that name to avoid a
  wider rename) needed zero protocol changes, only different startup
  arguments (model directory + worker script path, see below).
- Each line can be plain text, or a JSON object
  `{"text": "...", "length_scale": 1.23}` to override the speaking speed for
  just that one utterance (see JSON Protocol below) without restarting the
  worker. `length_scale` keeps Piper's semantics (`>1.0` slower, `<1.0`
  faster) even though VITS's own native parameter (`speaking_rate`) is the
  inverse -- `mms_worker.py` converts internally so nothing about the JSON
  protocol had to change meaning.
- Two extra tuning knobs with no Piper equivalent, set at startup (not
  per-request): `noise_scale` (higher = more variation in voice
  timbre/intonation, default `0.667`) and `noise_scale_duration` (higher =
  more variation in rhythm/syllable timing, default `0.8`) -- VITS-specific
  "expressiveness" parameters.
- **Sample rate differs from Piper**: MMS-TTS's Polish model runs at
  **16000 Hz**, not Piper's 22050 Hz. `PiperTTS::getSampleRate()` in the
  C++ side no longer hardcodes either value -- it reads the actual rate out
  of each synthesized WAV's own header, so the AudioPlayer is always
  configured correctly regardless of which engine/model is active.
- MMS-TTS's tokenizer does **not** use espeak-ng at all (unlike Piper), so
  the whole espeak-ng build/install story below no longer applies.
  `--espeak-data` is still accepted by `mms_worker.py` (and ignored) purely
  so the C++ side's argument list didn't need to change shape.

### Setting up the MMS-TTS Python environment

Torch/transformers get their own **dedicated venv**, kept completely
separate from whatever `python3`/`onnxruntime`/numpy versions anything else
on the robot needs (this matters: mixing them risked breaking other
tooling via numpy/scipy version conflicts during testing):

```bash
cd ~/projects/booster-CogitoNexus
python3 -m venv tts_venv
source tts_venv/bin/activate
pip install --upgrade pip
pip install transformers torch scipy numpy
```

**On disk space:** expect this venv to land somewhere around 0.5-3 GB
depending on which `torch` build pip resolves (a plain `pip install torch`
on the Jetson's aarch64 CPU tends to land a CPU-only build in the hundreds
of MB; check `du -sh tts_venv` afterwards for the real number on your
device, and `df -h` beforehand if space is tight).

**On GPU acceleration:** a plain `pip install torch` from PyPI on aarch64
is very likely to install a **CPU-only** build -- it does *not*
automatically get you the Jetson's CUDA/cuDNN via the SDK's own stack the
way `onnxruntime-gpu` did for Piper. Verify with:

```bash
tts_venv/bin/python3 -c "import torch; print(torch.__version__, torch.cuda.is_available())"
```

If this prints `False`, `cogito_server`'s `--cuda` flag will have no effect
(the worker logs a warning and falls back to CPU) -- synthesis will still
work, just slower. Getting real GPU acceleration requires a Jetson-specific
PyTorch build matched to your exact JetPack/L4T version (from NVIDIA's own
Jetson PyTorch wheels, not plain PyPI) -- a separate, optional step; measure
actual latency on CPU first and only chase this if it's not fast enough.

### Downloading the model locally (one-time, needs internet)

The C++ side loads the model from a **local directory**, not a bare
Hugging Face hub id -- this means `mms_worker.py` never needs network
access at runtime (important: the robot's normal operating network may not
have a route to the internet at all), and avoids a slow/failing hub lookup
on every restart.

```bash
tts_venv/bin/python3 -c "
from huggingface_hub import snapshot_download
path = snapshot_download('facebook/mms-tts-pol',
                          local_dir='/home/booster/projects/booster-CogitoNexus/mms-model/pl_PL')
print('Downloaded to:', path)
"
```

This needs internet access once, on whatever network the robot has at
setup time; after that, everything loads from disk.

## Quick Start

**Default robot password:** `123456`

### 1. Transfer to robot

```bash
scp -r CMakeLists.txt build.sh cogito-server.service mms_worker.py test_client.py README.md src booster@<IP>:/home/booster/projects/booster-CogitoNexus/
```

`mms-model/` and `tts_venv/` are **not** scp'd -- create/download them
directly on the robot instead (see "Setting up the MMS-TTS Python
environment" / "Downloading the model locally" above), since the venv in
particular is much larger than is sensible to transfer and may bundle
platform-specific binaries anyway.

### 2. Build on robot

```bash
ssh booster@<IP>
cd /home/booster/projects/booster-CogitoNexus
chmod +x build.sh
./build.sh
```

Also set up `tts_venv` and download the model as described above (one-time
setup; not part of `build.sh` yet).

### 3. Run manually

```bash
./build/cogito_server 9000 \
    /home/booster/projects/booster-CogitoNexus/mms-model/pl_PL \
    "" \
    /home/booster/projects/booster-CogitoNexus/mms_worker.py \
    1.0 \
    1.0
```

Arguments, in order: `port`, `model_dir` (local MMS-TTS model directory),
`unused` (kept for CLI-position compatibility with the old Piper
invocation -- pass `""`, it's ignored), `worker.py path`, `length_scale`
(>1.0 = slower, <1.0 = faster; `1.0` is the model's own natural speed),
`volume` (0.0-1.0+ multiplier on TTS output; `1.0` = unchanged). Both
default to `1.0` and are optional -- they fall back to the sensible
defaults baked into `main()` if omitted.

### 4. Run as systemd service (auto-start on boot)

```bash
# Copy service file
sudo cp cogito-server.service /etc/systemd/system/

# Enable and start
sudo systemctl daemon-reload
sudo systemctl enable cogito-server
sudo systemctl start cogito-server

# Check status
sudo systemctl status cogito-server

# View logs
sudo journalctl -u cogito-server -f

# Restart if needed
sudo systemctl restart cogito-server
```

The service will:
- Auto-start on robot boot
- Auto-restart if it crashes (with rate limiting)
- Run as `booster` user with security restrictions

To set a non-default `length_scale`/`volume` in production, add the full
positional argument list to `ExecStart=` in `cogito-server.service` (they're
positional, so you can't skip straight to `volume` without also specifying
the arguments before it) -- both now default to `1.0` if you omit them
entirely.

**Gotcha -- `ProtectSystem=strict` and `/tmp`:** the service file hardens
`cogito_server` with `ProtectSystem=strict` and
`ReadWritePaths=/home/booster/projects/booster-CogitoNexus` (plus
`ProtectHome=read-only`). `ProtectSystem=strict` makes the **entire**
filesystem read-only for this service except the paths explicitly listed
in `ReadWritePaths=` -- and `/tmp` is not one of them. The worker writes
its synthesized WAV files to a work directory (`PiperTTS::workDir_` in
`booster-CogitoNexus.cpp`), which therefore has to live under the
already-writable project path, not `/tmp` -- otherwise it fails to even
start under systemd (though it runs fine from an interactive SSH shell,
where `/tmp` is normally writable). This bit us for real with
`piper_worker.py`, which Python reported as a confusing
`AttributeError: 'Wave_write' object has no attribute '_file'` from
`wave.py`'s `close()` rather than the real "read-only file system" error --
`mms_worker.py` writes to the same already-whitelisted work directory, so
it doesn't hit this, but the same constraint applies to anything else you
add. If you ever add another directory the C++ side or the worker script
needs to write to, either put it under
`/home/booster/projects/booster-CogitoNexus/` or add it to
`ReadWritePaths=` in `cogito-server.service`.

## JSON Protocol

### Commands (client → server)

```json
{"text": "Hello world", "dance": "wave"}
```

`dance` runs **after** the message (unchanged, back-compat name). Two more
fields let you place a dance around -- or on top of -- the message:

```json
{"text": "Hi", "dance_before": "wave", "dance_during": "handshake", "dance": "dabbing"}
```

- `dance_before` -- runs **before** the message starts. Fully sequential:
  the server waits for it to completely finish (including its
  completion-wait time) before starting TTS synthesis/playback.
- `dance_during` -- runs **concurrently while the message is being spoken**,
  on its own thread. Only **`"wave"`** and **`"handshake"`** are accepted
  here -- anything else is refused (logged server-side and silently
  dropped) because those are the only two gestures that layer on top of
  whatever body-control state the robot is already in, rather than
  requiring an exclusive transition the way `Dance()`/`WholeBodyDance()` do.
  Sending e.g. `"dance_during": "roundhousekick"` will NOT execute --
  a whole-body dance or kick must never run at the same time as anything
  else (see "Dance safety" below).
- `dance` -- runs **after** the message finishes. Unchanged from before.

All three fields are independent and optional, and can be combined freely
(or used one at a time, or not at all). If `text` is omitted but
`dance_during` is set, it just runs as a normal sequential dance instead of
being silently dropped (there's nothing to run it concurrently with).

### Dance safety -- why some dances can never be interrupted

Every call into a dance/gesture (`Dance()`, `WholeBodyDance()`, `WaveHand()`,
`Handshake()`, `VisualKick()`) goes through a single process-wide mutex that
is held for the dance's **entire** duration, including the time the server
waits for it to physically finish. The server accepts multiple simultaneous
TCP connections, and they all share one `B1LocoClient` talking to the same
robot -- without this lock, a dance command arriving on one connection could
interleave with another dance already in progress on a different
connection (imagine a `roundhousekick` from one client getting stepped on by
a `wave` from another, mid-kick). With the lock, a second dance request
simply **blocks and waits its turn** until the first is completely done --
nothing can ever interrupt a whole-body dance or a kick once it starts.
This is also exactly why `dance_during` is restricted to `wave`/`handshake`
only: those two don't take an exclusive lock on the robot's gait/movement
state, so they're safe to run "on the side" while a normal dance elsewhere
could not be.

Before actually dancing, the server also checks the robot's current mode
(`GetMode()`):
- already `Walking` -- dances immediately, no transition needed.
- `Prepare` (standing) -- switches straight to `Walking` first, no need to
  re-run `GetUp()`.
- anything else (`Damping`, `Custom`, `Soccer`, ...) -- **refuses to dance
  at all** and logs why, rather than attempting an unknown/unsafe mode
  transition.

**Important -- non-ASCII text (Polish diacritics, etc.):** the server's
JSON parsing (`j_get`/`j_get_num` in `booster-CogitoNexus.cpp`) is a
minimal hand-rolled parser, not a real JSON library. It slices out the raw
bytes between quotes and does **not** decode `\uXXXX` escape sequences. So
the command line sent to the server must contain literal UTF-8 bytes for
things like `ę`/`ó`/`ł`, not their escaped `ę`/`ó`/`ł` form.
In Python, that means calling `json.dumps(cmd, ensure_ascii=False)` --
`ensure_ascii` defaults to `True`, which silently escapes non-ASCII
characters and makes the server (and thus the synthesized speech) see the
literal escape sequence instead of the real character. `test_client.py`
already does this correctly.

Optional per-request overrides (both optional; omit either to use the
server's startup defaults):

```json
{"text": "Hello world", "length_scale": 1.3, "volume": 0.5}
```

- `length_scale` -- speaking speed for just this utterance. >1.0 slower,
  <1.0 faster. Sent through to the GPU worker, which re-synthesizes at that
  speed without restarting.
- `volume` -- output loudness for just this utterance, as a multiplier on
  the PCM samples (0.0-1.0+, e.g. `0.5` = half as loud). Applied on the
  C++ side after synthesis, before playback.

### Changing the runtime defaults (`set_volume` / `set_length_scale`)

The per-request `length_scale`/`volume` above only affect the one message
they're sent with. To change what *every future request* falls back to
when it doesn't specify its own override -- i.e. change the server's
running defaults on the fly, without a restart -- send:

```json
{"set_volume": 0.5, "set_length_scale": 1.2}
```

Both fields are independent and optional, and can be sent alone (no
`text`/`dance` required) or combined with them in the same command. The
change takes effect immediately for the next request and stays in effect
for as long as the server process keeps running -- **it is not written to
disk anywhere**, so restarting `cogito_server` (or the systemd service)
goes back to whatever `length_scale`/`volume` were passed on the command
line (or their built-in defaults of `1.0`/`1.0` if none were given). The
server replies with a `"settings"` event confirming what was changed:

```json
{"type": "settings", "status": "updated", "volume": 0.5, "length_scale": 1.2}
```

(only the field(s) actually present in the command are echoed back).

### Events (server → client)

```json
{"type": "button", "button": "a", "pressed": true}
{"type": "tts", "status": "started", "text": "Hello"}
{"type": "tts", "status": "completed"}
{"type": "dance", "status": "started", "dance": "wave", "phase": "before"}
{"type": "dance", "status": "completed", "phase": "before"}
{"type": "audio", "samples": 1600, "sample_rate": 16000, "channels": 1, "bits_per_sample": 16, "data": "<base64 raw PCM>"}
{"type": "settings", "status": "updated", "volume": 0.5, "length_scale": 1.2}
```

`"button"` events come from the SDK's `RemoteControllerState` DDS stream
(topic `rt/remote_controller_state` -- not documented in any public SDK
header/example; confirmed against an independent SDK reimplementation
whose topic naming matches the constants already in
`booster/robot/b1/b1_api_const.hpp`). It's a continuous state snapshot, not
a discrete event stream, so the server diffs each new snapshot against the
previous one and emits one event per button whose state actually flipped.
`"button"` is always lowercase and one of: `a`, `b`, `x`, `y`, `lb`, `rb`,
`lt`, `rt`, `ls`, `rs`, `back`, `start`, `hat_c`, `hat_u`, `hat_d`, `hat_l`,
`hat_r`, `hat_lu`, `hat_ld`, `hat_ru`, `hat_rd` (DualShock 4 layout -- see
`ButtonSub` in `booster-CogitoNexus.cpp`).

#### "back" button: speaks the robot's own IP address

Pressing `back` (edge-triggered -- only on release→press, not while held)
makes the server itself speak its current IPv4 address on the `wlP1p1s0`
wireless interface, e.g. *"Cześć! Jestem Booster. Mój adres internetowy to:
192 kropka 168 kropka 1 kropka 5"*. This is entirely server-side: it happens whether or not any TCP client
is connected, doesn't go through the JSON protocol at all, and uses the
same TTS/AudioPlayer pipeline as normal TTS (at the server's current
runtime-default volume). The address is looked up live via `getifaddrs()`
each time the button is pressed (no caching), so it always reflects
whatever network the robot is actually on right now. If the interface has
no IPv4 address (e.g. not connected to Wi-Fi), it says so instead of an
address. See `getInterfaceIPv4()` / `announceIpAddress()` /
`ButtonSub::setOnBackPressed()` in `booster-CogitoNexus.cpp`; to announce a
different interface, change the `"wlP1p1s0"` argument passed to
`announceIpAddress()` in `main()`.

`"dance"` events now carry a `"phase"` field -- `"before"`, `"during"`, or
`"after"` -- so a client can tell which of the three slots (`dance_before`
/ `dance_during` / `dance`) an event belongs to when more than one is sent
in the same command. The base shape (`type`, `status`, `dance`) is
unchanged, so existing clients that ignore unknown fields keep working.

The `audio` event carries the actual microphone PCM (base64-encoded in
`data`), not just a sample count -- it's broadcast continuously to every
connected client whenever the capture stream has frames. `test_client.py`'s
`record_audio()` shows how to decode and accumulate these into a WAV file.

A client should wait for the actual `"status":"completed"` event (matching
the right `"phase"` when more than one dance is in flight) before assuming
an utterance or dance has finished, rather than sleeping a guessed number
of seconds -- `test_client.py` does this with a small event-waiting helper
(`wait_for()`).

## Available Dances

### Upper-body dances (DanceId)

| Name | Description |
|------|-------------|
| `newyear` | New Year dance |
| `nezha` | Nezha dance |
| `towardsfuture` | Towards the Future dance |
| `dabbing` | Dabbing gesture |
| `ultraman` | Ultraman gesture |
| `respect` | Respect/salute gesture |
| `cheering` | Cheering gesture |
| `luckycat` | Lucky-cat gesture |

### Whole-body dances (WholeBodyDanceId)

| Name | Description |
|------|-------------|
| `arabic` | Arabic dance |
| `michael1` | Michael dance 1 |
| `michael2` | Michael dance 2 |
| `michael3` | Michael dance 3 |
| `boxingkick` | Boxing-style kick |
| `roundhousekick` | Roundhouse kick |
| `shanheguren` | Shan He Gu Ren dance |
| `gaigechunfeng` | Gai Ge Chun Feng dance |
| `michael1and2` | Combined Michael dances 1 and 2 |
| `bowandarrow` | T2 bow-and-arrow routine |
| `charleston` | T2 Charleston dance |

Whole-body dances and kicks (`boxingkick`, `roundhousekick`, and everything
in this table) can only be used in `dance_before` / `dance` -- never in
`dance_during` (see "Dance safety" above) -- and, once started, cannot be
interrupted by any other dance command until they finish.

### Actions

| Name | Description |
|------|-------------|
| `wave` | Hand wave |
| `handshake` | Handshake |
| `kick` | Visual kick |

`wave` and `handshake` are the only two actions allowed in `dance_during`
(i.e. concurrently with speech).

## Python Client Example

```python
import socket, json

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect(('robot_ip', 9000))

# Send command (length_scale/volume are optional per-request overrides).
# dance_before/dance_during/dance let you place a gesture around, or
# layered on top of, the spoken message -- see "JSON Protocol" above.
# ensure_ascii=False is required so Polish diacritics reach the server as
# real UTF-8 bytes instead of \uXXXX escapes -- see the note under
# "JSON Protocol" above.
cmd = {
    "text": "Witaj świecie!",
    "dance_before": "wave",
    "dance_during": "handshake",
    "dance": "dabbing",
    "length_scale": 1.0,
    "volume": 0.6,
}
sock.sendall((json.dumps(cmd, ensure_ascii=False) + "\n").encode('utf-8'))

# Receive events
while True:
    line = sock.recv(4096).decode('utf-8')
    for msg in line.strip().split('\n'):
        if msg:
            event = json.loads(msg)
            print(event)
```

Or use the test client:
```bash
python test_client.py <robot_ip> 9000
```

## Build Requirements

- Booster Robotics SDK (`/home/booster/Workspace/sdk_release-main`)
- cmake >= 3.26 (install with `pip install cmake`)
- g++ with C++17 support
- The dedicated `tts_venv` (see "Setting up the MMS-TTS Python environment"
  above) with `transformers`/`torch`/`scipy`/`numpy` installed --
  `cogito_server` spawns `tts_venv/bin/python3 mms_worker.py` directly (an
  absolute interpreter path, hardcoded in `main()`'s `PiperTTS`
  construction -- not whatever `python3` happens to be on `PATH`) and will
  fail to initialize if that import doesn't work.
- The MMS-TTS model pre-downloaded locally (see "Downloading the model
  locally" above) -- `cogito_server` will refuse to start if the model
  directory it's pointed at doesn't exist.
- GPU acceleration is optional and NOT automatic: a plain `pip install
  torch` on the Jetson's aarch64 typically gets you a CPU-only build (see
  "On GPU acceleration" above) -- CPU-only still works, just check the
  actual latency (`torch.cuda.is_available()`) before assuming
  near-realtime holds on your hardware.
