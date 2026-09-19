/**
 * @file booster-CogitoNexus.cpp
 * @brief CogitoNexus Unified Server for Booster K1
 *
 * Features:
 * 1. TTS via MMS-TTS (facebook/mms-tts-pol) — run OUT OF PROCESS via a persistent worker (see note below)
 * 2. Audio streaming (microphone → TCP)
 * 3. Controller button events subscription
 * 4. JSON command protocol (text + dance)
 * 5. Dance execution via B1LocoClient
 *
 * JSON Command (client → server):
 *   {"text": "Hello", "dance": "wave"}
 *   -- "dance" runs AFTER the message. Optional "dance_before" runs before
 *   the message starts (fully sequential); optional "dance_during" runs
 *   concurrently WITH the message -- but only "wave"/"handshake" are
 *   accepted there (anything else is refused), since those are the only
 *   gestures that don't require an exclusive body-control transition the
 *   way Dance()/WholeBodyDance() do. All three fields are independent and
 *   optional; e.g. {"text": "Hi", "dance_before": "wave", "dance_during":
 *   "handshake", "dance": "dabbing"}.
 *
 *   {"set_volume": 0.5, "set_length_scale": 1.2}
 *   -- changes the server's RUNTIME DEFAULT volume/speed in place, for as
 *   long as this process keeps running (nothing is written to disk -- a
 *   restart goes back to the command-line/systemd-service defaults). Both
 *   fields are independent and optional. This is different from the
 *   per-request "volume"/"length_scale" fields above, which only affect
 *   the one message they're sent with; set_volume/set_length_scale change
 *   what every FUTURE request falls back to when it doesn't specify its
 *   own override. Can be sent alone (no "text"/"dance" required) or
 *   combined with them in the same command. Replies with a "settings"
 *   event (see below).
 *
 * JSON Events (server → client):
 *   {"type": "tts", "status": "started", "text": "Hello"}
 *   {"type": "tts", "status": "completed"}
 *   {"type": "dance", "status": "started", "dance": "wave", "phase": "before"|"during"|"after"}
 *   {"type": "dance", "status": "completed", "phase": "before"|"during"|"after"}
 *   {"type": "audio", "samples": 1600, "sample_rate": 16000, "channels": 1, "bits_per_sample": 16, "data": "<base64 PCM>"}
 *   {"type": "button", "button": "a", "pressed": true}
 *   -- broadcast whenever a controller button's state flips (edge-detected
 *   from the SDK's RemoteControllerState stream -- see ButtonSub); "button"
 *   is one of a/b/x/y/lb/rb/lt/rt/ls/rs/back/start/hat_c/hat_u/hat_d/hat_l/
 *   hat_r/hat_lu/hat_ld/hat_ru/hat_rd.
 *   {"type": "settings", "status": "updated", "volume": 0.5, "length_scale": 1.2}
 *   -- confirms a set_volume/set_length_scale command; only includes
 *   whichever field(s) were actually set in that command.
 *
 * "back" button -> speaks the robot's own IP:
 *   Pressing the controller's "back" button (edge-triggered, only on
 *   release->press) makes the server look up the current IPv4 address of
 *   the wlP1p1s0 (wireless) interface via getifaddrs() and speak it aloud
 *   through the TTS pipeline (e.g. "Cześć! Jestem Booster. Mój adres
 *   internetowy to: 192 kropka 168 kropka 1 kropka 5"), independent of any
 *   connected TCP client -- this is a
 *   server-side-only announcement, not part of the JSON protocol above. See
 *   announceIpAddress()/getInterfaceIPv4() and ButtonSub::setOnBackPressed().
 *
 * NOTE on TTS architecture:
 *   Originally this ran Piper (`piper_worker.py`, piper1-gpl's Python
 *   package) -- see git history / README for that whole story, including
 *   why TTS runs out-of-process at all (linking libpiper directly into
 *   this binary, alongside the Booster SDK's fastrtps/fastcdr/openssl
 *   stack, silently broke espeak-ng's phonemization).
 *
 *   TTS now runs via `mms_worker.py`, using Meta's MMS-TTS Polish model
 *   (facebook/mms-tts-pol, a VITS model loaded through Hugging Face
 *   `transformers`) instead of Piper -- switched because none of the
 *   available Piper pl_PL voices (bass-high, darkman, gosia, mc_speech)
 *   sounded right, and MMS-TTS's Polish voice won in listening tests.
 *   MMS-TTS's tokenizer doesn't use espeak-ng at all, so that specific
 *   original failure mode doesn't even apply here -- but the same
 *   out-of-process subprocess design was kept anyway (it's already built
 *   and working, and keeps PyTorch/transformers' large, version-sensitive
 *   dependency tree fully isolated in its own venv, away from this binary
 *   and the Booster SDK's linked libraries). The stdin/stdout "one line of
 *   text in, one WAV path out" protocol is unchanged, so this was a
 *   drop-in swap at the spawnWorker() level -- see PiperTTS (kept that
 *   class name to avoid a wider rename) and mms_worker.py.
 *
 * Build:
 *   ./build.sh
 */

#include <iostream>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <queue>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <functional>
#include <filesystem>
#include <map>
#include <algorithm>
#include <cctype>
#include <cstdint>

// For running piper_worker.py as a subprocess
#include <unistd.h>
#include <sys/wait.h>
#include <spawn.h>

// For reading the machine's own network interface addresses (used by the
// controller's "back" button -> "speak my IP" feature, see announceIpAddress()).
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

extern char **environ;

// Simple JSON helpers (prefixed with j_ to avoid conflicts)
static std::string j_esc(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '"') r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == '\t') r += "\\t";
        else r += c;
    }
    return r;
}

static std::string j_str(const std::string& k, const std::string& v) {
    return "\"" + j_esc(k) + "\":\"" + j_esc(v) + "\"";
}

static std::string j_int(const std::string& k, int v) {
    return "\"" + j_esc(k) + "\":" + std::to_string(v);
}

static std::string j_bool(const std::string& k, bool v) {
    return "\"" + j_esc(k) + "\":" + (v ? "true" : "false");
}

static std::string j_num(const std::string& k, double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4g", v);
    return "\"" + j_esc(k) + "\":" + buf;
}

static std::string j_obj(const std::vector<std::string>& pairs) {
    std::string r = "{";
    for (size_t i = 0; i < pairs.size(); i++) {
        if (i > 0) r += ",";
        r += pairs[i];
    }
    return r + "}";
}

static std::string j_get(const std::string& j, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = j.find(search);
    if (pos == std::string::npos) return "";
    pos = j.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < j.size() && (j[pos] == ' ' || j[pos] == '\t')) pos++;
    if (pos >= j.size() || j[pos] != '"') return "";
    pos++;
    size_t end = pos;
    while (end < j.size()) {
        if (j[end] == '"' && (end == 0 || j[end-1] != '\\')) break;
        end++;
    }
    return j.substr(pos, end - pos);
}

// Extracts a bare numeric JSON value (e.g. "volume":0.5) -- unlike j_get,
// this is for unquoted numbers, not strings. Returns false if the key is
// absent or its value isn't parseable as a number.
static bool j_get_num(const std::string& j, const std::string& key, double& out) {
    std::string search = "\"" + key + "\"";
    size_t pos = j.find(search);
    if (pos == std::string::npos) return false;
    pos = j.find(':', pos + search.size());
    if (pos == std::string::npos) return false;
    pos++;
    while (pos < j.size() && (j[pos] == ' ' || j[pos] == '\t')) pos++;
    size_t end = pos;
    while (end < j.size() &&
           (std::isdigit(static_cast<unsigned char>(j[end])) ||
            j[end] == '-' || j[end] == '+' || j[end] == '.' ||
            j[end] == 'e' || j[end] == 'E')) {
        end++;
    }
    if (end == pos) return false;
    try {
        out = std::stod(j.substr(pos, end - pos));
    } catch (...) {
        return false;
    }
    return true;
}

// Standard base64 alphabet -- used to embed raw microphone PCM bytes inside
// the "audio" JSON event (see runAudioThread()) so clients can actually
// record real audio instead of just seeing a sample count.
static const char* const B64_CHARS =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8) |
                     static_cast<uint32_t>(data[i + 2]);
        out += B64_CHARS[(n >> 18) & 0x3F];
        out += B64_CHARS[(n >> 12) & 0x3F];
        out += B64_CHARS[(n >> 6) & 0x3F];
        out += B64_CHARS[n & 0x3F];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        out += B64_CHARS[(n >> 18) & 0x3F];
        out += B64_CHARS[(n >> 12) & 0x3F];
        out += "==";
    } else if (rem == 2) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8);
        out += B64_CHARS[(n >> 18) & 0x3F];
        out += B64_CHARS[(n >> 12) & 0x3F];
        out += B64_CHARS[(n >> 6) & 0x3F];
        out += "=";
    }
    return out;
}

// Networking
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    typedef int SOCKET;
    #ifndef INVALID_SOCKET
    #define INVALID_SOCKET (-1)
    #endif
    #ifndef SOCKET_ERROR
    #define SOCKET_ERROR (-1)
    #endif
#endif

// Booster SDK
#include <booster/robot/audio/audio_manager.h>
#include <booster/robot/audio/audio_player.h>
#include <booster/robot/audio/audio_capture_stream.h>
#include <booster/robot/audio/audio_types.h>
#include <booster/robot/channel/channel_factory.hpp>
#include <booster/robot/channel/channel_subscriber.hpp>
#include <booster/robot/b1/b1_loco_client.hpp>
#include <booster/robot/b1/b1_loco_api.hpp>
#include <booster/robot/b1/b1_api_const.hpp>
#include <booster/robot/channel/channel_subscriber.hpp>
#include <booster/idl/b1/RemoteControllerState.h>

namespace fs = std::filesystem;

// ============================================================
// Dance name mapping
// ============================================================

// IDs here select which SDK call executeDance() makes: id<0 is a special
// action (wave/handshake/kick, dispatched by exact value), 0<=id<100 is a
// DanceId (upper-body), and id>=100 is a WholeBodyDanceId (dispatch
// subtracts 100 back off before casting -- see executeDance()).
//
// This offset is NOT cosmetic: DanceId and WholeBodyDanceId are two
// separate SDK enums that both start at 0, so without it "arabic" (meant
// to be WholeBodyDanceId 0) and "newyear" (DanceId 0) would be
// indistinguishable once reduced to a bare int, and executeDance() would
// call the wrong SDK function for every whole-body name that collided
// with an upper-body one (this WAS the case here previously -- e.g.
// "roundhousekick" silently called Dance(6)/"cheering" instead of
// WholeBodyDance(kRoundhouseKick)).
static const std::map<std::string, int> DANCE_MAP = {
    {"newyear", 0}, {"nezha", 1}, {"towardsfuture", 2}, {"dabbing", 3},
    {"ultraman", 4}, {"respect", 5}, {"cheering", 6}, {"luckycat", 7},
    {"stop", 1000},
    {"arabic", 100}, {"michael1", 101}, {"michael2", 102}, {"michael3", 103},
    {"boxingkick", 105}, {"roundhousekick", 106}, {"shanheguren", 107},
    {"gaigechunfeng", 108}, {"michael1and2", 109}, {"bowandarrow", 110},
    {"charleston", 111},
    {"wave", -1}, {"handshake", -2}, {"kick", -3},
};

// ============================================================
// Global state
// ============================================================

static std::atomic<bool> g_running(true);
static std::mutex g_clientsMutex;
static std::vector<SOCKET> g_clients;
static std::atomic<float> g_ttsVolume(1.0f); // 1.0 = unchanged, <1.0 = quieter

// Runtime default speaking speed -- same idea as g_ttsVolume: initialized
// from the command-line/systemd "length_scale" argument at startup, but
// changeable in place at any time via {"set_length_scale": ...} (see
// clientHandler()). Not persisted anywhere -- purely in-memory for the
// lifetime of this process, so a restart goes back to the startup value.
static std::atomic<float> g_ttsLengthScale(1.0f); // 1.0 = voice model's natural speed, >1.0 slower, <1.0 faster

// Serializes every call into executeDance() across ALL client connections.
// The server accepts multiple simultaneous TCP clients, each handled on its
// own thread but sharing one B1LocoClient -- without this, two connections
// sending dance commands at nearly the same moment could interleave calls
// into the same robot (e.g. a second command's ChangeMode/Dance racing in
// mid-roundhouse-kick). Some dances -- whole-body dances and kicks above
// all -- must NEVER be interrupted once started; holding this lock for the
// whole duration of executeDance() (including its completion sleep) means a
// second dance request simply blocks and waits its turn instead of racing
// in. WaveHand/Handshake go through the same lock when run standalone, but
// see runConcurrentGesture() for how they're allowed to overlap with a TTS
// utterance specifically (that path never touches the robot's movement
// state, only audio, so it's safe to run alongside).
static std::mutex g_locoDanceMutex;

// Scales PCM samples in place by `volume` and clamps to the int16 range
// (protects against overflow/wraparound if volume > 1.0 is ever used).
static void applyVolume(std::vector<int16_t>& pcm, float volume) {
    if (volume == 1.0f || pcm.empty()) return;
    for (auto& s : pcm) {
        float v = static_cast<float>(s) * volume;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        s = static_cast<int16_t>(v);
    }
}

// ============================================================
// Piper TTS
// ============================================================

// TTS via a persistent Python subprocess -- originally `piper_worker.py`
// (Piper), NOW `mms_worker.py` (Meta's MMS-TTS, facebook/mms-tts-pol),
// switched because the Piper pl_PL voices available didn't sound right and
// MMS-TTS's Polish voice was preferred in listening tests. Kept running for
// the whole server lifetime (not re-spawned per utterance).
//
// This class is intentionally engine-agnostic despite the name (kept as
// "PiperTTS" to avoid a wider rename across the codebase/README/systemd
// unit) -- it just spawns whatever worker script `workerScriptPath_` points
// at and speaks a generic "one line of text in, one WAV file path out"
// protocol. See the architecture note at the top of this file for why TTS
// runs out-of-process at all rather than linking a TTS library directly
// into this binary (that's what originally motivated the subprocess
// design, for Piper's espeak-ng specifically -- MMS-TTS's own tokenizer
// doesn't need espeak-ng at all, but we kept the same subprocess
// architecture anyway since it's already built, working, and keeps
// PyTorch/transformers' large dependency footprint isolated from this
// binary and the Booster SDK's own linked libraries).
//
// The worker loads the model ONCE at startup (onto CUDA if available and
// requested), does one warmup synthesis to pay the one-time
// CUDA-context/kernel-launch cost up front, then prints "READY" and enters
// its main loop: it reads one line of text per synthesis request from its
// stdin, and for each line writes a timestamped WAV file into <workDir_>
// and prints that file's path as one line on its stdout. We keep its
// stdin/stdout pipes open and reuse the same warmed-up process for every
// request.
//
// NOTE: like the old piper_worker.py, mms_worker.py flushes stdout itself
// after every printed line (print(..., flush=True)), so no `stdbuf -oL`
// wrapper is needed here.
class PiperTTS {
public:
    PiperTTS(const std::string& workerScriptPath, const std::string& voicePath,
             const std::string& espeakDataPath, float lengthScale = 1.0f,
             const std::string& pythonExe = "python3", bool useCuda = true,
             float noiseScale = 0.667f, float noiseScaleDuration = 0.8f)
        : workerScriptPath_(workerScriptPath), voicePath_(voicePath), espeakDataPath_(espeakDataPath),
          lengthScale_(lengthScale), pythonExe_(pythonExe), useCuda_(useCuda),
          noiseScale_(noiseScale), noiseScaleDuration_(noiseScaleDuration) {
        if (!fs::exists(workerScriptPath_)) {
            std::cerr << "[TTS] Worker script not found: " << workerScriptPath_ << std::endl;
            return;
        }
        if (!fs::exists(voicePath_)) {
            std::cerr << "[TTS] Voice/model path not found: " << voicePath_ << std::endl;
            return;
        }
        // Only checked if non-empty -- mms_worker.py doesn't use espeak-ng
        // at all (unlike Piper), but still accepts --espeak-data on its
        // command line purely so callers/config that pass a leftover Piper
        // path don't need to change; pass "" here to skip the check
        // entirely instead.
        if (!espeakDataPath_.empty() && !fs::exists(espeakDataPath_)) {
            std::cerr << "[TTS] espeak data not found: " << espeakDataPath_ << std::endl;
            return;
        }

        std::error_code ec;
        fs::create_directories(workDir_, ec);
        if (ec) {
            std::cerr << "[TTS] Failed to create work dir " << workDir_ << ": " << ec.message() << std::endl;
            return;
        }

        // spawnWorker() blocks here until the worker prints READY -- i.e.
        // until the model is loaded (onto the GPU, if available) and
        // warmed up. This can take a few seconds.
        if (!spawnWorker()) {
            std::cerr << "[TTS] Failed to start persistent worker" << std::endl;
            return;
        }

        initialized_ = true;
        std::cout << "[TTS] Ready (persistent worker via " << workerScriptPath_
                   << ", cuda=" << (useCuda_ ? "on" : "off")
                   << ", length_scale=" << lengthScale_ << ")" << std::endl;
    }

    ~PiperTTS() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopWorker();
    }

    bool isInitialized() const { return initialized_; }

    // Sends `text` to the persistent worker process and returns the
    // resulting PCM. Thread-safe (serializes concurrent callers); playback
    // itself goes through the Booster SDK's AudioPlayer (NOT raw ALSA /
    // aplay) -- the robot's speaker is owned by the `booster-audio` system
    // service, so a plain `aplay` from a second process just blocks
    // forever waiting for the device.
    // lengthScaleOverride <= 0 means "use the worker's default (set at
    // spawn time)". A positive value is sent along as a per-request JSON
    // line so the worker re-synthesizes with that speed just for this one
    // utterance, without needing to restart/respawn the worker.
    std::vector<int16_t> synthesize(const std::string& text, float lengthScaleOverride = -1.0f) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) {
            std::cerr << "[TTS] not initialized" << std::endl;
            return {};
        }

        // Our protocol with the worker is one line in, one line out --
        // strip any embedded newlines so a single request can't desync it.
        std::string sanitized = text;
        std::replace(sanitized.begin(), sanitized.end(), '\n', ' ');
        std::replace(sanitized.begin(), sanitized.end(), '\r', ' ');

        std::string line;
        if (lengthScaleOverride > 0.0f) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(lengthScaleOverride));
            line = "{\"text\":\"" + j_esc(sanitized) + "\",\"length_scale\":" + buf + "}\n";
        } else {
            line = sanitized + "\n";
        }

        if (!writeAll(stdinFd_, line.data(), line.size())) {
            std::cerr << "[TTS] Failed to write to worker stdin; respawning..." << std::endl;
            if (!respawnWorker()) return {};
            if (!writeAll(stdinFd_, line.data(), line.size())) {
                std::cerr << "[TTS] Still failing after respawn, giving up" << std::endl;
                return {};
            }
        }

        std::string outPath;
        if (!readLine(stdoutFile_, outPath)) {
            std::cerr << "[TTS] Worker produced no output (likely crashed); respawning..." << std::endl;
            if (!respawnWorker()) return {};
            // The request that triggered the crash is lost; caller sees an
            // empty result for this one utterance and can retry.
            return {};
        }

        uint32_t fileSampleRate = 0;
        auto pcm = readWavAsInt16(outPath, &fileSampleRate);
        ::unlink(outPath.c_str());
        // mms_worker.py's model runs at 16kHz, quite different from
        // Piper's 22050Hz -- rather than hardcoding either value (and
        // silently mis-playing audio at the wrong pitch/speed next time
        // the engine/model changes again), pick up whatever rate the WAV
        // file's own "fmt " chunk actually declares and remember it for
        // getSampleRate() (used by callers to configure the AudioPlayer).
        if (fileSampleRate > 0) {
            sampleRate_.store(static_cast<int>(fileSampleRate));
        }
        std::cerr << "[TTS] Total samples: " << pcm.size() << " @ " << sampleRate_.load() << "Hz" << std::endl;
        return pcm;
    }

    // Reflects whatever sample rate the most recently synthesized WAV
    // actually declared (see synthesize()); defaults to 22050 (Piper's
    // rate) before the first synthesis, purely as a harmless placeholder.
    int getSampleRate() const { return sampleRate_.load(); }

private:
    static bool writeAll(int fd, const char* data, size_t size) {
        if (fd < 0) return false;
        size_t off = 0;
        while (off < size) {
            ssize_t n = ::write(fd, data + off, size - off);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }

    static bool readLine(FILE* f, std::string& out) {
        if (!f) return false;
        char buf[4096];
        if (!std::fgets(buf, sizeof(buf), f)) return false;
        out = buf;
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        return true;
    }

    // Launches the persistent worker script and wires up
    // stdinFd_/stdoutFile_. Assumes mutex_ is already held by the caller.
    // Blocks until the worker prints "READY" (model loaded + warmed up)
    // before returning success.
    bool spawnWorker() {
        std::vector<std::string> args;
        args.push_back(pythonExe_);
        args.push_back(workerScriptPath_);
        args.push_back("--model"); args.push_back(voicePath_);
        if (!espeakDataPath_.empty()) {
            args.push_back("--espeak-data"); args.push_back(espeakDataPath_);
        }
        args.push_back("--work-dir"); args.push_back(workDir_);
        args.push_back("--length-scale");
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(lengthScale_));
            args.push_back(buf);
        }
        // mms_worker.py-specific tuning knobs (see its own docstring) --
        // harmless to always pass; piper_worker.py never sees these since
        // we no longer spawn it, but if a future worker script doesn't
        // recognize a flag, argparse would error, so keep this list in
        // sync with whatever worker script is actually configured.
        args.push_back("--noise-scale");
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(noiseScale_));
            args.push_back(buf);
        }
        args.push_back("--noise-scale-duration");
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(noiseScaleDuration_));
            args.push_back(buf);
        }
        if (useCuda_) args.push_back("--cuda");

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        int inPipe[2];  // parent writes -> child stdin
        int outPipe[2]; // child stdout -> parent reads
        if (::pipe(inPipe) != 0) {
            std::cerr << "[TTS] pipe() failed for stdin" << std::endl;
            return false;
        }
        if (::pipe(outPipe) != 0) {
            std::cerr << "[TTS] pipe() failed for stdout" << std::endl;
            ::close(inPipe[0]); ::close(inPipe[1]);
            return false;
        }

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, inPipe[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, inPipe[1]);
        posix_spawn_file_actions_addclose(&actions, inPipe[0]);
        posix_spawn_file_actions_addclose(&actions, outPipe[0]);
        posix_spawn_file_actions_addclose(&actions, outPipe[1]);

        // posix_spawnp searches PATH for pythonExe_ (usually just
        // "python3"), and we pass the parent's own environ through
        // unmodified -- the worker only needs HOME (for its ~/.local
        // site-packages) and PATH, both of which it already inherits.
        pid_t pid = 0;
        int rc = ::posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(inPipe[0]);
        ::close(outPipe[1]);

        if (rc != 0) {
            std::cerr << "[TTS] posix_spawnp(worker) failed: " << std::strerror(rc) << std::endl;
            ::close(inPipe[1]);
            ::close(outPipe[0]);
            return false;
        }

        workerPid_ = pid;
        stdinFd_ = inPipe[1];
        stdoutFile_ = ::fdopen(outPipe[0], "r");
        if (!stdoutFile_) {
            std::cerr << "[TTS] fdopen failed on worker stdout" << std::endl;
            ::close(outPipe[0]);
            stopWorker();
            return false;
        }

        std::string ready;
        if (!readLine(stdoutFile_, ready) || ready != "READY") {
            std::cerr << "[TTS] worker did not report READY (got: \"" << ready
                       << "\"); check that " << pythonExe_ << " can `import transformers, torch` "
                       << "and reach the model path given" << std::endl;
            stopWorker();
            return false;
        }
        return true;
    }

    // Closes pipes and reaps the worker process. Assumes mutex_ is held.
    void stopWorker() {
        if (stdinFd_ >= 0) { ::close(stdinFd_); stdinFd_ = -1; }
        if (stdoutFile_) { std::fclose(stdoutFile_); stdoutFile_ = nullptr; }
        if (workerPid_ > 0) {
            int status = 0;
            ::waitpid(workerPid_, &status, 0);
            workerPid_ = -1;
        }
    }

    // Assumes mutex_ is held.
    bool respawnWorker() {
        stopWorker();
        return spawnWorker();
    }

    // Minimal RIFF/WAVE reader, originally written for piper_exe's output,
    // now shared by whatever worker script is configured (mms_worker.py
    // writes standard-compliant WAVs via Python's `wave` module, unlike
    // piper_exe -- but reading "data" to EOF regardless of the declared
    // size still works fine for those too, since data remains the last
    // chunk either way).
    //
    // HISTORICAL NOTE (kept for piper_exe compatibility, not currently
    // exercised): piper_exe wrote placeholder/sentinel values into both the
    // RIFF chunk size (bytes 4-7) and the "data" subchunk size fields
    // instead of patching in the real sizes after writing (confirmed via
    // hexdump: both read back as ~0x7ffff000, nowhere near the real file
    // size). So those size fields CANNOT be trusted -- for the "data"
    // chunk we ignore the declared size entirely and just read everything
    // remaining in the file up to EOF instead. "data" is assumed to be the
    // last chunk.
    //
    // outSampleRate (optional): if non-null, receives the sample rate
    // declared in the "fmt " chunk -- needed because different engines/
    // models run at different native rates (Piper: 22050Hz, MMS-TTS's
    // pl_PL model: 16000Hz) and hardcoding either one would silently
    // mis-play audio (wrong pitch/speed) the next time the engine changes.
    static std::vector<int16_t> readWavAsInt16(const std::string& path, uint32_t* outSampleRate = nullptr) {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            std::cerr << "[TTS] Failed to open WAV: " << path << std::endl;
            return {};
        }

        char riff[4];
        f.read(riff, 4);
        if (f.gcount() != 4 || std::strncmp(riff, "RIFF", 4) != 0) {
            std::cerr << "[TTS] Not a RIFF file: " << path << std::endl;
            return {};
        }
        f.seekg(4, std::ios::cur); // overall chunk size -- unreliable, ignored
        char wave[4];
        f.read(wave, 4);
        if (std::strncmp(wave, "WAVE", 4) != 0) {
            std::cerr << "[TTS] Not a WAVE file: " << path << std::endl;
            return {};
        }

        uint16_t audioFormat = 1, bitsPerSample = 16;
        uint32_t sampleRateFromFile = 0;
        std::vector<uint8_t> dataBytes;
        bool foundData = false;

        while (f && !foundData) {
            char chunkId[4];
            f.read(chunkId, 4);
            if (f.gcount() != 4) break;
            uint32_t chunkSize = 0;
            f.read(reinterpret_cast<char*>(&chunkSize), 4);
            if (!f) break;

            if (std::strncmp(chunkId, "fmt ", 4) == 0) {
                std::vector<char> fmt(chunkSize);
                f.read(fmt.data(), chunkSize);
                if (chunkSize >= 16) {
                    std::memcpy(&audioFormat, fmt.data() + 0, 2);
                    std::memcpy(&sampleRateFromFile, fmt.data() + 4, 4);
                    std::memcpy(&bitsPerSample, fmt.data() + 14, 2);
                }
            } else if (std::strncmp(chunkId, "data", 4) == 0) {
                // Declared chunkSize is bogus -- read to EOF instead.
                std::streampos dataStart = f.tellg();
                f.seekg(0, std::ios::end);
                std::streampos fileEnd = f.tellg();
                std::streamoff realDataSize = fileEnd - dataStart;
                f.seekg(dataStart);
                if (realDataSize > 0) {
                    dataBytes.resize(static_cast<size_t>(realDataSize));
                    f.read(reinterpret_cast<char*>(dataBytes.data()), realDataSize);
                }
                foundData = true; // data is the last chunk; stop parsing
            } else {
                // Unknown chunk before "data" (e.g. "fact"); chunkSize IS
                // reliable for these, only "data" itself is bogus.
                f.seekg(chunkSize, std::ios::cur);
                if (chunkSize % 2 == 1) f.seekg(1, std::ios::cur); // WAV padding
            }
        }

        if (outSampleRate) *outSampleRate = sampleRateFromFile;

        std::vector<int16_t> pcm;
        if (dataBytes.empty()) return pcm;

        if (audioFormat == 3 && bitsPerSample == 32) { // IEEE float
            size_t n = dataBytes.size() / 4;
            pcm.resize(n);
            const float* samples = reinterpret_cast<const float*>(dataBytes.data());
            for (size_t i = 0; i < n; i++) {
                float s = samples[i];
                if (s > 1.0f) s = 1.0f;
                if (s < -1.0f) s = -1.0f;
                pcm[i] = static_cast<int16_t>(s * 32767.0f);
            }
        } else if (audioFormat == 1 && bitsPerSample == 16) { // integer PCM
            size_t n = dataBytes.size() / 2;
            pcm.resize(n);
            std::memcpy(pcm.data(), dataBytes.data(), n * 2);
        } else {
            std::cerr << "[TTS] Unsupported WAV format: audioFormat=" << audioFormat
                      << " bitsPerSample=" << bitsPerSample << std::endl;
        }
        return pcm;
    }

    std::string workerScriptPath_;
    std::string voicePath_;
    std::string espeakDataPath_;
    float lengthScale_ = 1.0f;
    std::string pythonExe_ = "python3";
    bool useCuda_ = true;
    float noiseScale_ = 0.667f;         // VITS: higher = more variation in voice timbre/intonation
    float noiseScaleDuration_ = 0.8f;   // VITS: higher = more variation in rhythm/syllable timing
    std::atomic<int> sampleRate_{22050}; // updated from the actual WAV after each synthesize() call
    // NOT /tmp: cogito-server.service runs with ProtectSystem=strict and
    // ReadWritePaths=/home/booster/projects/booster-CogitoNexus only -- that
    // makes the ENTIRE filesystem read-only for this service except paths
    // explicitly listed there, and /tmp is not one of them. Writing the
    // warm-up/per-utterance WAV files to /tmp/cogito_tts_work therefore
    // failed under systemd (worked fine when run manually from a normal SSH
    // shell, where /tmp is writable) with wave.open() raising a read-only-
    // filesystem error inside piper_worker.py -- which Python then reported
    // as a confusing secondary "'Wave_write' object has no attribute
    // '_file'" AttributeError from wave.py's close(), since the exception
    // happened before Wave_write finished initializing. Using a directory
    // under the project path keeps this working under the existing
    // ReadWritePaths without having to also edit the .service file.
    std::string workDir_ = "/home/booster/projects/booster-CogitoNexus/tts_work";
    bool initialized_ = false;

    std::mutex mutex_;
    pid_t workerPid_ = -1;
    int stdinFd_ = -1;
    FILE* stdoutFile_ = nullptr;
};

// ============================================================
// Audio Stream
// ============================================================

class AudioStream {
public:
    void push(const std::vector<int16_t>& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(data);
        cv_.notify_one();
    }
    bool pop(std::vector<int16_t>& out, int timeout_ms = 1000) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                         [this] { return !queue_.empty() || stopped_; })) {
            if (stopped_ && queue_.empty()) return false;
            out = std::move(queue_.front());
            queue_.pop();
            return true;
        }
        return false;
    }
    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        cv_.notify_all();
    }
private:
    std::queue<std::vector<int16_t>> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopped_ = false;
};

static AudioStream g_audioStream;

// ============================================================
// TCP Server
// ============================================================

static void sendAll(SOCKET sock, const void* data, size_t size) {
    size_t total = 0;
    while (total < size) {
        int sent = ::send(sock, (const char*)data + total, size - total, 0);
        if (sent <= 0) break;
        total += sent;
    }
}

static void sendStr(SOCKET sock, const std::string& s) {
    sendAll(sock, s.data(), s.size());
}

static void broadcastStr(const std::string& s) {
    std::lock_guard<std::mutex> lock(g_clientsMutex);
    for (auto& c : g_clients) sendAll(c, s.data(), s.size());
}

static void executeDance(booster::robot::b1::B1LocoClient& loco, const std::string& name) {
    auto it = DANCE_MAP.find(name);
    if (it == DANCE_MAP.end()) {
        std::cerr << "[Dance] Unknown: " << name << std::endl;
        return;
    }
    int id = it->second;

    // Hold this for the entire dance, including its completion sleep, so a
    // second command arriving on another connection can't interleave a
    // ChangeMode/Dance/WholeBodyDance call mid-execution -- it just blocks
    // here until this one is fully done. See the comment on the mutex's
    // declaration for why this matters (whole-body dances and kicks must
    // never be interrupted).
    std::lock_guard<std::mutex> danceLock(g_locoDanceMutex);

    // Dance()/WholeBodyDance() need the robot already in the Walking body
    // control state (kHumanlikeGait) -- Prepare mode alone just means
    // "standing", not "ready to dance". Check the robot's actual current
    // mode instead of blindly re-running ChangeMode(kPrepare)+GetUp() every
    // single time (that was slow and, per the user, unnecessary when it's
    // already sitting in Prepare):
    //   - already Walking  -> go straight to the dance, no transition needed.
    //   - Prepare          -> just switch straight to Walking (already
    //                         standing, no need to GetUp() again).
    //   - anything else (Damping, Custom, Soccer, get-up in progress, ...)
    //     -> refuse. We don't know a safe transition from an arbitrary mode
    //        into a dance, so don't attempt it -- log and bail out instead
    //        of letting the robot silently do nothing (or worse).
    booster::robot::b1::GetModeResponse modeResp;
    int retGetMode = loco.GetMode(modeResp);
    if (retGetMode != 0) {
        std::cerr << "[Dance] GetMode() failed, ret=" << retGetMode
                   << " -- refusing to dance (unknown current mode)" << std::endl;
        return;
    }
    booster::robot::RobotMode currentMode = modeResp.mode_;
    std::cout << "[Dance] Current mode=" << static_cast<int>(currentMode) << std::endl;

    if (currentMode == booster::robot::RobotMode::kWalking) {
        // Already good to go.
    } else if (currentMode == booster::robot::RobotMode::kPrepare) {
        int retWalking = loco.ChangeMode(booster::robot::RobotMode::kWalking);
        std::cout << "[Dance] ChangeMode(kWalking) ret=" << retWalking << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    } else {
        std::cerr << "[Dance] Refusing to dance -- robot is in mode "
                   << static_cast<int>(currentMode)
                   << " (only kPrepare/kWalking are handled)" << std::endl;
        return;
    }

    // Retries an SDK call a few times with a short backoff if it fails.
    // We saw Handshake() come back with ret=501 immediately after a
    // WaveHand() finished (during a dance_before='wave' + dance_during=
    // 'handshake' sequence) even though GetMode() already reported
    // kWalking and our own settle delay had elapsed -- the robot's
    // hand/arm actuator can reject a new command with a transient "busy"
    // error for a brief moment after the previous hand action, on a
    // timescale our fixed sleep can't reliably predict. Rather than guess
    // an ever-bigger fixed delay, retry the actual RPC -- it succeeds the
    // moment the controller is actually ready, and only gives up (logging
    // every attempt) if it truly never recovers.
    auto callWithRetry = [](const std::function<int()>& fn, const char* label) -> int {
        const int maxAttempts = 6;
        const auto backoff = std::chrono::milliseconds(400);
        for (int attempt = 1; attempt <= maxAttempts; attempt++) {
            int ret = fn();
            std::cout << "[Dance] " << label << " attempt " << attempt
                       << "/" << maxAttempts << " ret=" << ret << std::endl;
            if (ret == 0) return ret;
            if (attempt < maxAttempts) {
                std::this_thread::sleep_for(backoff);
            }
        }
        std::cerr << "[Dance] " << label << " never succeeded after "
                   << maxAttempts << " attempts -- giving up" << std::endl;
        return -1;
    };

    if (id == -1) {
        callWithRetry([&loco]() {
            return loco.WaveHand(booster::robot::b1::HandAction::kHandOpen);
        }, "WaveHand(open)");
        std::this_thread::sleep_for(std::chrono::seconds(3));
        loco.WaveHand(booster::robot::b1::HandAction::kHandClose);
    } else if (id == -2) {
        callWithRetry([&loco]() {
            return loco.Handshake(booster::robot::b1::HandAction::kHandOpen);
        }, "Handshake(open)");
        std::this_thread::sleep_for(std::chrono::seconds(3));
        loco.Handshake(booster::robot::b1::HandAction::kHandClose);
    } else if (id == -3) {
        callWithRetry([&loco]() {
            return loco.VisualKick(true, booster::robot::b1::VisualKickVersion::kV1);
        }, "VisualKick(start)");
        std::this_thread::sleep_for(std::chrono::seconds(2));
        loco.VisualKick(false, booster::robot::b1::VisualKickVersion::kV1);
    } else if (id >= 0 && id < 100) {
        int danceId = id;
        callWithRetry([&loco, danceId]() {
            return loco.Dance(static_cast<booster::robot::b1::DanceId>(danceId));
        }, "Dance(start)");
        std::this_thread::sleep_for(std::chrono::seconds(10));
        loco.Dance(booster::robot::b1::DanceId::kStop);
    } else {
        // Undo the +100 offset applied in DANCE_MAP (see its comment) to
        // get back the real WholeBodyDanceId value.
        int danceId = id - 100;
        callWithRetry([&loco, danceId]() {
            return loco.WholeBodyDance(static_cast<booster::robot::b1::WholeBodyDanceId>(danceId));
        }, "WholeBodyDance(start)");
        std::this_thread::sleep_for(std::chrono::seconds(15));
    }

    // Settle time: the completion sleeps above are how long the SDK call is
    // *expected* to take, but the robot doesn't necessarily finish
    // physically settling back into a steady stance the instant that timer
    // elapses. Since we're still holding g_locoDanceMutex here, this delay
    // is "free" from the caller's point of view -- it just means the next
    // queued dance (whether that's dance_before -> dance_during -> dance in
    // the same command, or a completely different connection) waits an
    // extra beat before its own ChangeMode/Dance call goes out, instead of
    // firing the instant this one's nominal duration is up and potentially
    // landing on a robot that's still mid-transition from the last move.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
}

static void clientHandler(SOCKET sock, PiperTTS& piper,
                          booster::robot::audio::AudioManager& audioMgr,
                          booster::robot::b1::B1LocoClient& loco) {
    std::cout << "[TCP] Client connected" << std::endl;
    {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        g_clients.push_back(sock);
    }
    sendStr(sock, j_obj({j_str("type","welcome"), j_str("status","ready")}) + "\n");

    char buf[65536];
    std::string buffer;
    while (g_running) {
        int ret = ::recv(sock, buf, sizeof(buf), 0);
        if (ret <= 0) break;
        buffer.append(buf, ret);
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (line.empty()) continue;
            try {
                // set_volume / set_length_scale: change the server's RUNTIME
                // default for volume/speed, in place, for as long as this
                // process keeps running -- nothing is written to disk, so a
                // restart goes back to the command-line/systemd-service
                // defaults. This is separate from the per-request "volume"/
                // "length_scale" fields below (those affect ONE utterance
                // only); this changes what every future request falls back
                // to when it doesn't specify its own override. Can be sent
                // alone (no "text"/"dance" needed) or combined with them in
                // the same command.
                double newDefaultVolume = -1.0, newDefaultLengthScale = -1.0;
                bool hasSetVolume = j_get_num(line, "set_volume", newDefaultVolume);
                bool hasSetLengthScale = j_get_num(line, "set_length_scale", newDefaultLengthScale);
                if (hasSetVolume || hasSetLengthScale) {
                    std::vector<std::string> fields = {j_str("type", "settings"), j_str("status", "updated")};
                    if (hasSetVolume) {
                        g_ttsVolume.store(static_cast<float>(newDefaultVolume));
                        std::cout << "[Cmd] set_volume -> new runtime default volume=" << newDefaultVolume << std::endl;
                        fields.push_back(j_num("volume", newDefaultVolume));
                    }
                    if (hasSetLengthScale) {
                        g_ttsLengthScale.store(static_cast<float>(newDefaultLengthScale));
                        std::cout << "[Cmd] set_length_scale -> new runtime default length_scale=" << newDefaultLengthScale << std::endl;
                        fields.push_back(j_num("length_scale", newDefaultLengthScale));
                    }
                    sendStr(sock, j_obj(fields) + "\n");
                }

                std::string text = j_get(line, "text");
                std::string dance = j_get(line, "dance"); // runs AFTER the message (unchanged, back-compat)
                std::string danceBefore = j_get(line, "dance_before"); // runs BEFORE the message starts
                std::string danceDuring = j_get(line, "dance_during"); // runs WHILE the message plays
                if (text.empty() && dance.empty() && danceBefore.empty() && danceDuring.empty()) continue;

                // Only WaveHand/Handshake are safe to run concurrently with a
                // TTS utterance: they're layered "actions" on top of whatever
                // gait/body-control state the robot is already in, not an
                // exclusive body-control transition the way Dance()/
                // WholeBodyDance() are. Anything else requested via
                // dance_during is refused rather than risking a movement
                // command firing mid-speech.
                if (!danceDuring.empty() && danceDuring != "wave" && danceDuring != "handshake") {
                    std::cerr << "[Cmd] dance_during='" << danceDuring
                              << "' refused -- only 'wave'/'handshake' may run during speech" << std::endl;
                    danceDuring.clear();
                }

                // Optional per-request overrides -- if absent, fall back to
                // the server's startup defaults (length_scale baked into the
                // worker at spawn time; volume from g_ttsVolume).
                double reqLengthScale = -1.0, reqVolume = -1.0;
                bool hasLengthScale = j_get_num(line, "length_scale", reqLengthScale);
                bool hasVolume = j_get_num(line, "volume", reqVolume);

                std::cout << "[Cmd] text='" << text << "' dance_before='" << danceBefore
                          << "' dance_during='" << danceDuring << "' dance='" << dance << "'"
                          << (hasLengthScale ? (" length_scale=" + std::to_string(reqLengthScale)) : "")
                          << (hasVolume ? (" volume=" + std::to_string(reqVolume)) : "")
                          << std::endl;

                // Dance BEFORE the message -- fully sequential, blocks until
                // done (executeDance() itself waits out the dance's
                // completion time before returning).
                if (!danceBefore.empty()) {
                    sendStr(sock, j_obj({j_str("type","dance"), j_str("status","started"),
                                         j_str("dance",danceBefore), j_str("phase","before")}) + "\n");
                    executeDance(loco, danceBefore);
                    sendStr(sock, j_obj({j_str("type","dance"), j_str("status","completed"),
                                         j_str("phase","before")}) + "\n");
                }

                // TTS -- synthesize via piper_worker.py (subprocess), then play the
                // PCM through the Booster SDK's AudioPlayer. Playback must go
                // through the SDK, not raw ALSA/aplay: the robot's speaker is
                // owned by the `booster-audio` system service, and a second
                // process opening the ALSA device directly just blocks.
                if (!text.empty()) {
                    // dance_during runs on its own thread for the duration of
                    // this whole TTS block -- it touches only `loco` (via
                    // executeDance(), still serialized by g_locoDanceMutex
                    // against any other dance in flight), while everything
                    // below here touches only `audioMgr`/the player, so the
                    // two genuinely run side by side rather than one blocking
                    // the other.
                    std::thread concurrentGestureThread;
                    if (!danceDuring.empty()) {
                        sendStr(sock, j_obj({j_str("type","dance"), j_str("status","started"),
                                             j_str("dance",danceDuring), j_str("phase","during")}) + "\n");
                        concurrentGestureThread = std::thread([&loco, danceDuring]() {
                            executeDance(loco, danceDuring);
                        });
                    }

                    sendStr(sock, j_obj({j_str("type","tts"), j_str("status","started"), j_str("text",text)}) + "\n");
                    // Falls back to the current runtime default (g_ttsLengthScale
                    // -- settable in place via {"set_length_scale": ...},
                    // see above) rather than a sentinel meaning "let the
                    // worker use whatever it was spawned with" -- that way a
                    // set_length_scale command actually takes effect on the
                    // very next request instead of requiring a worker respawn.
                    float lengthScaleOverride = hasLengthScale ? static_cast<float>(reqLengthScale) : g_ttsLengthScale.load();
                    auto pcm = piper.synthesize(text, lengthScaleOverride);
                    float vol = hasVolume ? static_cast<float>(reqVolume) : g_ttsVolume.load();
                    applyVolume(pcm, vol);
                    std::cout << "[TTS] Synthesized " << pcm.size() << " samples" << std::endl;
                    if (!pcm.empty()) {
                        auto player = audioMgr.CreatePlayer();
                        if (player) {
                            booster::robot::audio::PlayerInitOptions opts;
                            opts.source_type = booster::robot::audio::AudioSourceType::kPcmStream;
                            opts.sample_rate_hz = piper.getSampleRate();
                            opts.channels = 1;
                            opts.bits_per_sample = 16;
                            opts.priority = booster::robot::audio::PlayerPriority::kMedium;
                            int ret = player->Init(opts);
                            std::cout << "[TTS] Player init ret=" << ret << std::endl;
                            if (ret == 0) {
                                player->Start();
                                int sr = piper.getSampleRate();
                                size_t chunkFrames = static_cast<size_t>(sr) / 10, off = 0;
                                auto playStart = std::chrono::steady_clock::now();
                                while (off < pcm.size() && g_running) {
                                    size_t n = (chunkFrames < pcm.size() - off) ? chunkFrames : (pcm.size() - off);
                                    int push_ret = player->PushPcmStream(
                                        reinterpret_cast<const uint8_t*>(pcm.data() + off),
                                        n * sizeof(int16_t));
                                    if (push_ret != 0) {
                                        std::cout << "[TTS] PushPcmStream ret=" << push_ret << " off=" << off << std::endl;
                                    }
                                    off += n;
                                }
                                // The SDK's reported PlayerState (kCompleted
                                // etc.) turned out to be an unreliable clock:
                                // waiting on it (even combined with a real-time
                                // floor) made the "completed" event fire ~3s
                                // after the audio actually finished. We know
                                // the PCM's exact real-time duration already
                                // (pcm.size() / sample_rate), so just wait
                                // that long since playback started, plus one
                                // small fixed pad for downstream/hardware
                                // buffering latency -- no SDK-state polling.
                                double expectedMs = (sr > 0)
                                    ? (1000.0 * static_cast<double>(pcm.size()) / sr)
                                    : 0.0;
                                double elapsedMs = std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - playStart).count();
                                double remainingMs = expectedMs - elapsedMs;
                                if (remainingMs > 0.0) {
                                    std::this_thread::sleep_for(
                                        std::chrono::milliseconds(static_cast<long long>(remainingMs)));
                                }
                                // Fixed safety pad for residual hardware
                                // buffering latency (found empirically: too
                                // small clips the last word, e.g. 350ms
                                // wasn't quite enough).
                                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                                player->Stop();
                            }
                            player->Release();
                        } else {
                            std::cerr << "[TTS] CreatePlayer failed" << std::endl;
                        }
                    } else {
                        std::cerr << "[TTS] Synthesize returned empty pcm" << std::endl;
                    }
                    sendStr(sock, j_obj({j_str("type","tts"), j_str("status","completed")}) + "\n");

                    if (concurrentGestureThread.joinable()) {
                        concurrentGestureThread.join();
                        sendStr(sock, j_obj({j_str("type","dance"), j_str("status","completed"),
                                             j_str("phase","during")}) + "\n");
                    }
                }
                // dance_during only makes sense alongside text (there's
                // nothing to run it concurrently with otherwise) -- if text
                // was empty, just run it as a plain sequential dance so the
                // request isn't silently dropped.
                if (text.empty() && !danceDuring.empty()) {
                    sendStr(sock, j_obj({j_str("type","dance"), j_str("status","started"),
                                         j_str("dance",danceDuring), j_str("phase","during")}) + "\n");
                    executeDance(loco, danceDuring);
                    sendStr(sock, j_obj({j_str("type","dance"), j_str("status","completed"),
                                         j_str("phase","during")}) + "\n");
                }
                // Dance AFTER the message (unchanged, back-compat field name "dance")
                if (!dance.empty()) {
                    sendStr(sock, j_obj({j_str("type","dance"), j_str("status","started"),
                                         j_str("dance",dance), j_str("phase","after")}) + "\n");
                    executeDance(loco, dance);
                    sendStr(sock, j_obj({j_str("type","dance"), j_str("status","completed"),
                                         j_str("phase","after")}) + "\n");
                }
            } catch (const std::exception& e) {
                sendStr(sock, j_obj({j_str("type","error"), j_str("message",e.what())}) + "\n");
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_clientsMutex);
        g_clients.erase(std::remove(g_clients.begin(), g_clients.end(), sock), g_clients.end());
    }
#ifdef _WIN32
    closesocket(sock);
#else
    ::close(sock);
#endif
    std::cout << "[TCP] Client disconnected" << std::endl;
}

static void runTcpServer(int port, PiperTTS& piper,
                         booster::robot::audio::AudioManager& audioMgr,
                         booster::robot::b1::B1LocoClient& loco) {
    SOCKET ss = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ss == INVALID_SOCKET) { std::cerr << "[TCP] socket() failed" << std::endl; return; }
    int opt = 1;
    setsockopt(ss, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (::bind(ss, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "[TCP] bind() failed" << std::endl; return;
    }
    if (::listen(ss, 5) == SOCKET_ERROR) {
        std::cerr << "[TCP] listen() failed" << std::endl; return;
    }
    std::cout << "[TCP] Listening on port " << port << std::endl;
    while (g_running) {
        fd_set fds; FD_ZERO(&fds); FD_SET(ss, &fds);
        struct timeval tv; tv.tv_sec = 1; tv.tv_usec = 0;
        int ret = ::select(ss + 1, &fds, nullptr, nullptr, &tv);
        if (ret > 0 && FD_ISSET(ss, &fds)) {
            struct sockaddr_in ca; socklen_t cl = sizeof(ca);
            SOCKET cs = ::accept(ss, (struct sockaddr*)&ca, &cl);
            if (cs != INVALID_SOCKET)
                std::thread(clientHandler, cs, std::ref(piper), std::ref(audioMgr), std::ref(loco)).detach();
        }
    }
#ifdef _WIN32
    closesocket(ss);
#else
    ::close(ss);
#endif
}

// ============================================================
// Audio Capture
// ============================================================

// Faktycznie negocjowany/używany format strumienia -- ustawiane na bieżąco
// w onAudioFrame (na podstawie tego, czy akurat wysyłamy NAEC czy raw) oraz
// raz na starcie w main() po capture->Init(). Wartości domyślne to stary
// hardkodowany fallback, gdyby capture nigdy się nie zainicjalizował --
// wtedy zdarzenie "audio" po prostu nigdy nie zostanie wysłane.
static std::atomic<int> g_captureSampleRate(16000);
static std::atomic<int> g_captureChannels(1);
static std::atomic<int> g_captureBitsPerSample(16);

static void onAudioFrame(const booster::robot::audio::AudioCaptureFrame& frame) {
    static std::atomic<long long> s_frameCount(0);
    long long n = ++s_frameCount;
    if (n <= 3 || n % 200 == 0) {
        std::cout << "[Audio] onAudioFrame #" << n
                   << " raw_valid=" << frame.raw_valid
                   << " raw_pcm.size()=" << frame.raw_pcm.size()
                   << " naec_valid=" << frame.naec_valid
                   << " naec_pcm.size()=" << frame.naec_pcm.size() << std::endl;
    }

    // UWAGA -- WRÓCILIŚMY na "raw" (rezygnacja z NAEC), mimo że NAEC dawał
    // technicznie czystszy sygnał (patrz komentarz historyczny niżej). W
    // testach na żywo redukcja szumu w NAEC okazała się działać jak
    // agresywna bramka szumów: gdy mówca stał dalej niż ok. 0.5m od robota,
    // NAEC nie tylko czyścił szum, ale też ucinał/tłumił samą mowę --
    // Whisper dostawał prawie ciszę. Nie ma w SDK parametru, żeby to
    // złagodzić, więc korzystamy z "raw" i wybieramy z niego JEDEN kanał
    // (patrz BoosterBridge::handleRawMessage w main_app), zamiast uśredniać
    // wszystkie 3 -- uśrednianie fizycznie rozsuniętych mikrofonów bez
    // wyrównania fazy powodowało zniekształcenia grzebieniowe (comb
    // filtering), które i tak psuły jakość rozpoznawania. Wybór
    // pojedynczego kanału tego nie robi, kosztem nieco gorszego SNR niż
    // dawałby prawidłowy beamforming.
    //
    // Historia: NAEC to gotowy, przetworzony przez SDK (beamforming +
    // redukcja szumu/echa) pojedynczy kanał, przeznaczony właśnie pod
    // rozpoznawanie mowy -- stąd wcześniejsza preferencja dla niego nad
    // "raw". Zostawiamy go jako fallback, gdyby "raw" z jakiegoś powodu nie
    // był dostępny.
    if (frame.raw_valid && !frame.raw_pcm.empty()) {
        g_captureSampleRate = frame.raw_format.sample_rate_hz;
        g_captureChannels = frame.raw_format.channels;
        g_captureBitsPerSample = frame.raw_format.bits_per_sample;
        g_audioStream.push(frame.raw_pcm);
    } else if (frame.naec_valid && !frame.naec_pcm.empty()) {
        g_captureSampleRate = frame.naec_format.sample_rate_hz;
        g_captureChannels = frame.naec_format.channels;
        g_captureBitsPerSample = frame.naec_format.bits_per_sample;
        g_audioStream.push(frame.naec_pcm);
    }
}

// ============================================================
// IP announcement (controller "back" button)
// ============================================================
//
// Looks up the current IPv4 address of a given network interface (e.g.
// "wlP1p1s0", the robot's wireless interface) via the standard POSIX
// getifaddrs() API -- the same mechanism `ip addr` itself uses under the
// hood. Returns "" if the interface doesn't exist or has no IPv4 address
// assigned (e.g. not connected to any network yet).
static std::string getInterfaceIPv4(const std::string& ifaceName) {
    struct ifaddrs* ifaddr = nullptr;
    if (::getifaddrs(&ifaddr) == -1) {
        std::cerr << "[IP] getifaddrs() failed" << std::endl;
        return "";
    }
    std::string result;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || !ifa->ifa_name) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue; // IPv4 only
        if (ifaceName != ifa->ifa_name) continue;
        char buf[INET_ADDRSTRLEN] = {0};
        const void* addrPtr = &reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr)->sin_addr;
        if (::inet_ntop(AF_INET, addrPtr, buf, sizeof(buf))) {
            result = buf;
        }
        break;
    }
    ::freeifaddrs(ifaddr);
    return result;
}

// Synthesizes `text` via `piper` and plays it straight through the SDK's
// AudioPlayer, exactly like the per-request TTS path in clientHandler() --
// but standalone, with no TCP client/socket involved and no JSON events
// sent anywhere. Used for out-of-band announcements the server makes on
// its own (currently: the controller "back" button's IP announcement, see
// announceIpAddress() below). Uses the server's current runtime-default
// volume (g_ttsVolume) and whatever length_scale `piper` was last
// spawned/updated with -- no per-call overrides needed here.
static void speakText(PiperTTS& piper, booster::robot::audio::AudioManager& audioMgr,
                       const std::string& text) {
    auto pcm = piper.synthesize(text);
    applyVolume(pcm, g_ttsVolume.load());
    if (pcm.empty()) {
        std::cerr << "[Speak] Synthesize returned empty pcm for: " << text << std::endl;
        return;
    }
    auto player = audioMgr.CreatePlayer();
    if (!player) {
        std::cerr << "[Speak] CreatePlayer failed" << std::endl;
        return;
    }
    booster::robot::audio::PlayerInitOptions opts;
    opts.source_type = booster::robot::audio::AudioSourceType::kPcmStream;
    opts.sample_rate_hz = piper.getSampleRate();
    opts.channels = 1;
    opts.bits_per_sample = 16;
    opts.priority = booster::robot::audio::PlayerPriority::kMedium;
    int ret = player->Init(opts);
    if (ret == 0) {
        player->Start();
        int sr = piper.getSampleRate();
        size_t chunkFrames = static_cast<size_t>(sr) / 10, off = 0;
        auto playStart = std::chrono::steady_clock::now();
        while (off < pcm.size() && g_running) {
            size_t n = (chunkFrames < pcm.size() - off) ? chunkFrames : (pcm.size() - off);
            player->PushPcmStream(reinterpret_cast<const uint8_t*>(pcm.data() + off), n * sizeof(int16_t));
            off += n;
        }
        double expectedMs = (sr > 0) ? (1000.0 * static_cast<double>(pcm.size()) / sr) : 0.0;
        double elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - playStart).count();
        double remainingMs = expectedMs - elapsedMs;
        if (remainingMs > 0.0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(remainingMs)));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // same hardware-buffering pad as clientHandler()
        player->Stop();
    } else {
        std::cerr << "[Speak] Player init ret=" << ret << std::endl;
    }
    player->Release();
}

// Converts an integer in [0, 999] to its Polish word form (e.g. 192 ->
// "sto dziewięćdziesiąt dwa", 5 -> "pięć", 0 -> "zero"). Needed because
// unlike Piper (which used espeak-ng's number-to-speech normalization),
// MMS-TTS's tokenizer vocabulary is built purely from the characters that
// appeared in its training text -- digits 0-9 are NOT in that vocabulary,
// so raw numerals are silently dropped by the tokenizer instead of being
// pronounced. Every number spoken through MMS-TTS must be spelled out as
// words first. IPv4 octets are always 0-255, well within this function's
// supported range.
static std::string numberToPolishWords(int n) {
    static const char* jednosci[] = {
        "zero", "jeden", "dwa", "trzy", "cztery",
        "pięć", "sześć", "siedem", "osiem", "dziewięć"};
    static const char* nastki[] = {
        "dziesięć", "jedenaście", "dwanaście", "trzynaście", "czternaście",
        "piętnaście", "szesnaście", "siedemnaście", "osiemnaście", "dziewiętnaście"};
    static const char* dziesiatki[] = {
        "", "", "dwadzieścia", "trzydzieści", "czterdzieści",
        "pięćdziesiąt", "sześćdziesiąt", "siedemdziesiąt", "osiemdziesiąt", "dziewięćdziesiąt"};
    static const char* setki[] = {
        "", "sto", "dwieście", "trzysta", "czterysta",
        "pięćset", "sześćset", "siedemset", "osiemset", "dziewięćset"};

    if (n <= 0) return "zero";
    if (n > 999) return std::to_string(n); // out of range for this helper's use case (IP octets); fall back rather than crash

    std::string result;
    int hundreds = n / 100;
    int remainder = n % 100;

    if (hundreds > 0) {
        result += setki[hundreds];
    }
    if (remainder >= 10 && remainder < 20) {
        if (!result.empty()) result += " ";
        result += nastki[remainder - 10];
    } else {
        int tens = remainder / 10;
        int units = remainder % 10;
        if (tens > 0) {
            if (!result.empty()) result += " ";
            result += dziesiatki[tens];
        }
        if (units > 0) {
            if (!result.empty()) result += " ";
            result += jednosci[units];
        }
    }
    return result;
}

// Builds a speech-friendly rendering of an IPv4 address ("192.168.1.5" ->
// "sto dziewięćdziesiąt dwa kropka sto sześćdziesiąt osiem kropka jeden
// kropka pięć") and speaks it via `piper`/`audioMgr`. Each octet is spelled
// out as Polish words via numberToPolishWords() -- MMS-TTS's tokenizer has
// no digit characters in its vocabulary and silently drops raw numerals
// (this is what caused "Mój adres internetowy to: ..." to trail off with no
// numbers spoken at all). "kropka" between octets remains the clearest way
// to say the separator, matching how a person would read an IP aloud.
static void announceIpAddress(PiperTTS& piper, booster::robot::audio::AudioManager& audioMgr,
                               const std::string& ifaceName) {
    std::string ip = getInterfaceIPv4(ifaceName);
    std::string text;
    if (ip.empty()) {
        text = "Cześć! Jestem Booster. Nie mam adresu I P na interfejsie " + ifaceName;
    } else {
        std::string spoken;
        size_t start = 0;
        while (true) {
            size_t dot = ip.find('.', start);
            std::string part = (dot == std::string::npos) ? ip.substr(start) : ip.substr(start, dot - start);
            int octet = std::atoi(part.c_str());
            if (!spoken.empty()) spoken += " kropka ";
            spoken += numberToPolishWords(octet);
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        text = "Cześć! Jestem Booster. Mój adres internetowy to: " + spoken;
    }
    std::cout << "[IP] " << ifaceName << " -> \"" << text << "\"" << std::endl;
    speakText(piper, audioMgr, text);
}

// ============================================================
// Button Events
// ============================================================
//
// The physical controller (a DualShock4 paired to the robot -- see `lsusb`
// on the Jetson) isn't read as raw HID/joystick input: the SDK republishes
// its state as a DDS topic, using the fastddsgen-generated
// booster_interface::msg::RemoteControllerState type (see
// booster/idl/b1/RemoteControllerState.h). There's no Register*/Subscribe*
// helper for it on B1LocoClient and no example anywhere in the SDK --
// it's just another topic, subscribed the same generic way the SDK itself
// subscribes to everything else (ChannelSubscriber<T>, same class used
// internally for audio/joint state -- hence the "ChannelSubscriber::
// InitChannel" log lines already seen at startup for those).
//
// The topic name "rt/remote_controller_state" isn't in any public header
// or example either (booster/robot/b1/b1_api_const.hpp only has
// kTopicLowState, kTopicJointCtrl, etc -- nothing for the controller). It
// was confirmed via an independent from-scratch SDK reimplementation
// (docs.rs/booster_sdk's dds::topics module), whose topic table's naming
// ("rt/<snake_case>") matches every constant already in b1_api_const.hpp.
//
// RemoteControllerState is a continuous state snapshot (every button as a
// plain bool, plus stick axes lx/ly/rx/ry), not a discrete press/release
// event stream -- so we keep the previous snapshot and diff it against
// each new one, emitting one {"type":"button","button":"<name>",
// "pressed":true|false} event per button whose state actually flipped.
// This matches the JSON event shape already documented at the top of this
// file and in README.md.
struct RemoteControllerSnapshot {
    bool a = false, b = false, x = false, y = false;
    bool lb = false, rb = false, lt = false, rt = false;
    bool ls = false, rs = false;
    bool back = false, start = false;
    bool hat_c = false, hat_u = false, hat_d = false, hat_l = false, hat_r = false;
    bool hat_lu = false, hat_ld = false, hat_ru = false, hat_rd = false;
};

class ButtonSub {
public:
    // Registers a callback fired (on its own detached thread, NOT the DDS
    // subscriber's callback thread) every time the "back" button transitions
    // from released to pressed. Call this BEFORE init() (main() does:
    // construct ButtonSub -> setOnBackPressed() -> init()), though it's safe
    // to call any time since onState() only reads onBackPressed_ under
    // prevMutex_.
    void setOnBackPressed(std::function<void()> cb) {
        std::lock_guard<std::mutex> lock(prevMutex_);
        onBackPressed_ = std::move(cb);
    }

    void init() {
        sub_ = std::make_unique<booster::robot::ChannelSubscriber<booster_interface::msg::RemoteControllerState>>(
            "rt/remote_controller_state",
            [this](const void* msg) { onState(msg); },
            false /* reliable=false -- this is a high-rate state stream (like the
                     audio capture channel), best-effort delivery is fine and
                     matches how the SDK subscribes to similar streams itself */);
        sub_->InitChannel();
        std::cout << "[Controller] Subscribed to rt/remote_controller_state" << std::endl;
    }

private:
    void diffAndEmit(const char* name, bool prev, bool cur) {
        if (prev == cur) return;
        broadcastStr(j_obj({j_str("type", "button"), j_str("button", name), j_bool("pressed", cur)}) + "\n");
    }

    void onState(const void* msgPtr) {
        const auto* s = static_cast<const booster_interface::msg::RemoteControllerState*>(msgPtr);
        RemoteControllerSnapshot cur;
        cur.a = s->a();       cur.b = s->b();       cur.x = s->x();       cur.y = s->y();
        cur.lb = s->lb();     cur.rb = s->rb();     cur.lt = s->lt();     cur.rt = s->rt();
        cur.ls = s->ls();     cur.rs = s->rs();
        cur.back = s->back(); cur.start = s->start();
        cur.hat_c = s->hat_c(); cur.hat_u = s->hat_u(); cur.hat_d = s->hat_d();
        cur.hat_l = s->hat_l(); cur.hat_r = s->hat_r();
        cur.hat_lu = s->hat_lu(); cur.hat_ld = s->hat_ld();
        cur.hat_ru = s->hat_ru(); cur.hat_rd = s->hat_rd();

        std::lock_guard<std::mutex> lock(prevMutex_);
        diffAndEmit("a", prev_.a, cur.a);         diffAndEmit("b", prev_.b, cur.b);
        diffAndEmit("x", prev_.x, cur.x);         diffAndEmit("y", prev_.y, cur.y);
        diffAndEmit("lb", prev_.lb, cur.lb);      diffAndEmit("rb", prev_.rb, cur.rb);
        diffAndEmit("lt", prev_.lt, cur.lt);      diffAndEmit("rt", prev_.rt, cur.rt);
        diffAndEmit("ls", prev_.ls, cur.ls);      diffAndEmit("rs", prev_.rs, cur.rs);
        diffAndEmit("back", prev_.back, cur.back);
        diffAndEmit("start", prev_.start, cur.start);
        diffAndEmit("hat_c", prev_.hat_c, cur.hat_c);
        diffAndEmit("hat_u", prev_.hat_u, cur.hat_u);
        diffAndEmit("hat_d", prev_.hat_d, cur.hat_d);
        diffAndEmit("hat_l", prev_.hat_l, cur.hat_l);
        diffAndEmit("hat_r", prev_.hat_r, cur.hat_r);
        diffAndEmit("hat_lu", prev_.hat_lu, cur.hat_lu);
        diffAndEmit("hat_ld", prev_.hat_ld, cur.hat_ld);
        diffAndEmit("hat_ru", prev_.hat_ru, cur.hat_ru);
        diffAndEmit("hat_rd", prev_.hat_rd, cur.hat_rd);

        // "back" pressed (released -> pressed edge only, so holding it down
        // doesn't re-trigger every state update) -> announce the robot's IP.
        // Runs on its own detached thread: onState() fires on the DDS
        // subscriber's callback thread, and speakText() blocks for the
        // whole duration of the announcement (synthesis + playback, a
        // couple of seconds) -- doing that inline here would stall further
        // controller-state updates (and, transitively, every other button's
        // event) for as long as the robot is talking.
        bool backPressedEdge = (!prev_.back && cur.back);
        std::function<void()> cb = onBackPressed_;
        prev_ = cur;
        if (backPressedEdge && cb) {
            std::thread([cb]() { cb(); }).detach();
        }
    }

    std::unique_ptr<booster::robot::ChannelSubscriber<booster_interface::msg::RemoteControllerState>> sub_;
    std::mutex prevMutex_;
    RemoteControllerSnapshot prev_;
    std::function<void()> onBackPressed_;
};

// ============================================================
// Audio Stream Thread
// ============================================================

// Actual negotiated capture format, filled in from AudioCaptureStreamInfo
// after a successful capture->Init() in main() (see the requested_raw_format
// candidate loop there). Defaults match the old hardcoded assumption in case
// capture never initializes -- the "audio" event just won't be sent then.
// (Deklaracja przeniesiona wyżej, przed onAudioFrame -- patrz sekcja
// "Audio Capture" -- bo ta funkcja też ich teraz potrzebuje, żeby zgłaszać
// klientowi format faktycznie wysyłanego strumienia (NAEC albo raw).)

static void runAudioThread() {
    std::vector<int16_t> frame;
    while (g_running) {
        if (g_audioStream.pop(frame, 1000)) {
            // Embed the actual raw PCM as base64 so clients can really
            // record audio, not just observe frame counts. Format reflects
            // whatever the capture stream actually negotiated (see the
            // requested_raw_format candidate loop in main() -- the service
            // may not accept mono and instead deliver raw multi-channel
            // microphone-array data), so clients don't have to guess it.
            std::string b64 = base64Encode(reinterpret_cast<const uint8_t*>(frame.data()),
                                            frame.size() * sizeof(int16_t));
            broadcastStr(j_obj({
                j_str("type","audio"),
                j_int("samples", (int)frame.size()),
                j_int("sample_rate", g_captureSampleRate.load()),
                j_int("channels", g_captureChannels.load()),
                j_int("bits_per_sample", g_captureBitsPerSample.load()),
                j_str("data", b64)
            }) + "\n");
        }
    }
}

// ============================================================
// Main
// ============================================================

void printUsage(const char* p) {
    std::cout << "Usage: " << p << " [port] [model_dir] [unused] [worker.py] [length_scale] [volume]\n"
              << "  model_dir: local directory holding the MMS-TTS model (facebook/mms-tts-pol),\n"
              << "             e.g. downloaded once via huggingface_hub.snapshot_download()\n"
              << "  unused: kept for CLI-position compatibility with the old Piper invocation\n"
              << "          (used to be the espeak-ng data path -- mms_worker.py doesn't need it,\n"
              << "          pass \"\" or any existing path, it's ignored)\n"
              << "  volume: 0.0-1.0+ multiplier on TTS output samples (default 1.0, e.g. 0.5 = half as loud)\n"
              << "CogitoNexus Server - TTS (MMS-TTS Polish), audio, dances, controller\n"
              << "JSON: {\"text\":\"Hello\",\"dance\":\"wave\"}\n"
              << "Dances: newyear,nezha,towardsfuture,dabbing,ultraman,respect,cheering,luckycat\n"
              << "  arabic,michael1,michael2,michael3,boxingkick,roundhousekick\n"
              << "  shanheguren,gaigechunfeng,michael1and2,bowandarrow,charleston\n"
              << "  wave,handshake,kick\n";
}

int main(int argc, char* argv[]) {
    int port = 9000;
    // MMS-TTS (facebook/mms-tts-pol), pre-downloaded to a local directory --
    // see README.md's "Switching the TTS engine" section for the one-time
    // snapshot_download() command that populates this path. Loading from a
    // real local directory (rather than the bare hub id) means transformers
    // never needs network access at runtime, which matters both for the
    // "must be local/offline" requirement and for startup reliability if
    // the robot's operating network has no route to the internet.
    std::string voice = "/home/booster/projects/booster-CogitoNexus/mms-model/pl_PL";
    // No longer used for anything (MMS-TTS's tokenizer doesn't need
    // espeak-ng) -- kept as a CLI-position placeholder only so the argument
    // count/order doesn't have to change from the old Piper invocation.
    // mms_worker.py accepts and ignores --espeak-data. "" is fine here.
    std::string espeak = "";
    std::string piperWorker = "/home/booster/projects/booster-CogitoNexus/mms_worker.py";
    float lengthScale = 1.0f; // >1.0 = slower, <1.0 = faster; 1.0 = the model's own natural speed
    float volume = 1.0f;      // 0.0-1.0+ multiplier on TTS output samples; 1.0 = unchanged

    if (argc > 1) { std::string a = argv[1]; if (a=="--help"||a=="-h") { printUsage(argv[0]); return 0; } port = std::stoi(a); }
    if (argc > 2) voice = argv[2];
    if (argc > 3) espeak = argv[3];
    if (argc > 4) piperWorker = argv[4];
    if (argc > 5) lengthScale = std::stof(argv[5]);
    if (argc > 6) volume = std::stof(argv[6]);
    g_ttsVolume.store(volume);
    g_ttsLengthScale.store(lengthScale);

    std::cout << "=== CogitoNexus Server ===" << std::endl;
    std::cout << "Port: " << port << std::endl;
    std::cout << "TTS volume: " << volume << std::endl;

    signal(SIGINT, [](int){ g_running = false; });
    signal(SIGTERM, [](int){ g_running = false; });

    // pythonExe points at the dedicated venv created for MMS-TTS
    // (transformers/torch/scipy), NOT the system python3 used to run
    // piper_worker.py before -- keeps torch's large/version-sensitive
    // dependency tree fully isolated from whatever numpy/onnxruntime
    // versions anything else on this system needs. See README.md for the
    // one-time `python3 -m venv tts_venv && pip install ...` setup.
    PiperTTS piper(piperWorker, voice, espeak, lengthScale,
                   "/home/booster/projects/booster-CogitoNexus/tts_venv/bin/python3");
    if (!piper.isInitialized()) { std::cerr << "[TTS] Init failed" << std::endl; return 1; }

    {
        // Synthesis-only self-test here (no SDK AudioPlayer yet at this
        // point in startup) -- just confirms the worker + WAV parsing
        // work. Actual playback is exercised for real once a client sends
        // text.
        auto t = piper.synthesize("Test jeden dwa trzy");
        std::cout << "[TTS] Startup self-test samples=" << t.size()
                   << " @ " << piper.getSampleRate() << "Hz" << std::endl;
        if (t.empty()) {
            std::cerr << "[TTS] WARNING: startup self-test produced 0 samples!" << std::endl;
        }
    }

    std::cout << "[SDK] Initializing ChannelFactory..." << std::endl;
    try {
        booster::robot::ChannelFactory::Instance()->Init(0);
        std::cout << "[SDK] ChannelFactory initialized" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[SDK] ChannelFactory init error: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "[SDK] ChannelFactory init: unknown error" << std::endl;
    }

    std::cout << "[SDK] Initializing AudioManager..." << std::endl;
    booster::robot::audio::AudioManager audioMgr;
    if (audioMgr.Init() != 0) { std::cerr << "[SDK] AudioManager init failed" << std::endl; return 1; }

    auto capture = audioMgr.CreateCaptureStream();
    if (!capture) {
        std::cerr << "[Audio] CreateCaptureStream() returned null -- no microphone capture available" << std::endl;
    } else {
        // The SDK docs note AudioCaptureStreamOptions::requested_raw_format
        // currently defaults to 16000/3/16 (three channels -- the raw
        // microphone-array output before any beamforming/AEC mixdown to
        // mono). We previously only tried mono (1 channel), which the
        // service rejected with kAudioErrNotSupported (7001). Try the
        // array-native 3-channel format first, then fall back to mono and
        // stereo in case the actual negotiated format differs from the
        // documented default on this firmware.
        struct RawFormatCandidate { int32_t sample_rate_hz; int32_t channels; int32_t bits_per_sample; };
        static const RawFormatCandidate kCandidates[] = {
            {16000, 3, 16},
            {16000, 1, 16},
            {16000, 2, 16},
        };
        capture->SetFrameCallback(onAudioFrame);

        int captureInitRet = -1;
        for (const auto& fmt : kCandidates) {
            booster::robot::audio::AudioCaptureStreamOptions opts;
            opts.enable_raw_pcm = true;
            // Główny strumień to teraz "raw" (patrz szeroki komentarz przy
            // onAudioFrame) -- NAEC dawał czystszy sygnał technicznie, ale
            // jego redukcja szumu działała jak bramka ucinająca mowę powyżej
            // ok. 0.5m od robota. Zostawiamy naec_pcm włączone jako fallback
            // w onAudioFrame, gdyby "raw" z jakiegoś powodu nie był dostępny.
            opts.enable_naec_pcm = true;
            opts.requested_raw_format = {fmt.sample_rate_hz, fmt.channels, fmt.bits_per_sample};
            captureInitRet = capture->Init(opts);
            std::cout << "[Audio] capture->Init() with requested_raw_format={"
                      << fmt.sample_rate_hz << "," << fmt.channels << "," << fmt.bits_per_sample
                      << "} -> ret=" << captureInitRet << std::endl;
            if (captureInitRet == 0) {
                break;
            }
        }

        if (captureInitRet == 0) {
            booster::robot::audio::AudioCaptureStreamInfo info;
            if (capture->GetInfo(&info) == 0) {
                std::cout << "[Audio] Negotiated raw format: "
                          << info.actual_raw_format.sample_rate_hz << "Hz, "
                          << info.actual_raw_format.channels << " ch, "
                          << info.actual_raw_format.bits_per_sample << " bit"
                          << " (raw_enabled=" << info.raw_enabled << ")" << std::endl;
                std::cout << "[Audio] Negotiated NAEC format: "
                          << info.actual_naec_format.sample_rate_hz << "Hz, "
                          << info.actual_naec_format.channels << " ch, "
                          << info.actual_naec_format.bits_per_sample << " bit"
                          << " (naec_enabled=" << info.naec_enabled << ")" << std::endl;
                // Wartości startowe -- onAudioFrame i tak nadpisuje je co
                // klatkę w zależności od tego, którego strumienia faktycznie
                // używa (raw czy NAEC), ale to na wypadek gdyby "audio" miało
                // pójść do klienta zanim pierwsza klatka w ogóle nadejdzie.
                if (info.raw_enabled && info.actual_raw_format.sample_rate_hz > 0) {
                    g_captureSampleRate = info.actual_raw_format.sample_rate_hz;
                    g_captureChannels = info.actual_raw_format.channels;
                    g_captureBitsPerSample = info.actual_raw_format.bits_per_sample;
                } else if (info.actual_naec_format.sample_rate_hz > 0) {
                    g_captureSampleRate = info.actual_naec_format.sample_rate_hz;
                    g_captureChannels = info.actual_naec_format.channels;
                    g_captureBitsPerSample = info.actual_naec_format.bits_per_sample;
                }
            }
            int captureStartRet = capture->Start();
            if (captureStartRet == 0) {
                std::cout << "[Audio] Capture started" << std::endl;
            } else {
                std::cerr << "[Audio] capture->Start() failed, ret=" << captureStartRet << std::endl;
            }
        } else {
            std::cerr << "[Audio] capture->Init() failed for all candidate formats, last ret=" << captureInitRet << std::endl;
        }
    }

    std::cout << "[Loco] Initializing B1LocoClient..." << std::endl;
    booster::robot::b1::B1LocoClient loco;
    loco.Init();
    loco.WaitForService(5000);
    std::cout << "[Loco] Ready" << std::endl;

    ButtonSub btn;
    // "back" button -> speak the robot's current IP on wlP1p1s0 (its
    // wireless interface). Registered before init() so the very first
    // state callback already has it wired up.
    btn.setOnBackPressed([&piper, &audioMgr]() {
        announceIpAddress(piper, audioMgr, "wlP1p1s0");
    });
    btn.init();

    std::thread audioTh(runAudioThread);
    runTcpServer(port, piper, audioMgr, loco);

    g_running = false;
    g_audioStream.stop();
    if (audioTh.joinable()) audioTh.join();
    if (capture) { capture->Stop(); capture->Destroy(); }
    audioMgr.Shutdown();
    std::cout << "[Server] Done." << std::endl;
    return 0;
}