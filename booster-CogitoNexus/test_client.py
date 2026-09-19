#!/usr/bin/env python3
"""
CogitoNexus Test Client

Tests:
1. Actually record 10 seconds of microphone audio to recording.wav (real
   PCM captured from the server's "audio" events, not a fake sleep)
2. TTS: "Witaj świecie!"
3. Dance: "wave"
4. Combined TTS + dance (after)
5. TTS with per-request length_scale / volume overrides
6. dance_before + dance_during + dance (all three slots in one command --
   wave before, handshake layered during speech, dabbing after)
7. Refused dance_during (server should refuse a whole-body dance/kick as
   "during" and just skip it -- see README.md "Dance safety")

Every step here WAITS on the actual server event that marks it done
(the server's own JSON events: {"type":"welcome",...}, {"type":"tts",
"status":"completed"}, {"type":"dance","status":"completed","phase":...})
instead of sleeping a guessed number of seconds -- that's the whole point
of the server pushing these events over the socket in the first place. The
one exception is genuinely time-based: record_audio() waits `duration`
seconds because that's an actual recording length, not a completion signal.

Usage:
    python test_client.py [robot_ip] [port]
"""

import base64
import socket
import json
import time
import sys
import threading
import wave

class CogitoClient:
    def __init__(self, host, port=9000):
        self.host = host
        self.port = port
        self.sock = None
        self._running = False

        # Registered (predicate, threading.Event, result-holder) waiters --
        # see wait_for(). Guarded by _waiters_lock since events arrive on
        # the listener thread but waiters are registered from the caller's
        # thread.
        self._waiters = []
        self._waiters_lock = threading.Lock()

        # Registered (fn) sniffers -- called on EVERY event, unlike waiters
        # which fire once and unregister. Used by record_audio() to
        # continuously accumulate "audio" events for the requested duration
        # instead of a single one-shot wait.
        self._sniffers = []
        self._sniffers_lock = threading.Lock()

    def connect(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((self.host, self.port))
        self._running = True

        # Start event listener BEFORE waiting for anything, so we don't
        # race the server's welcome event.
        self.listener = threading.Thread(target=self._listen, daemon=True)
        self.listener.start()

        print("[Client] Connected, waiting for welcome...")
        welcome = self.wait_for(lambda e: e.get('type') == 'welcome',
                                 timeout=10, description="welcome event")
        if welcome is None:
            raise RuntimeError("Server never sent a welcome event")

    def _listen(self):
        buffer = ""
        while self._running:
            try:
                data = self.sock.recv(4096)
                if not data:
                    break
                buffer += data.decode('utf-8', errors='ignore')

                while '\n' in buffer:
                    line, buffer = buffer.split('\n', 1)
                    if line.strip():
                        event = json.loads(line)
                        self._on_event(event)
            except Exception as e:
                print(f"[Client] Listen error: {e}")
                break

    def _on_event(self, event):
        etype = event.get('type', 'unknown')
        status = event.get('status', '')

        if etype == 'welcome':
            print(f"[Event] Server ready: {status}")
        elif etype == 'button':
            print(f"[Event] Button '{event['button']}' pressed={event['pressed']}")
        elif etype == 'tts':
            print(f"[Event] TTS {status}: {event.get('text', '')}")
        elif etype == 'dance':
            phase = event.get('phase')
            phase_str = f" (phase={phase})" if phase else ""
            print(f"[Event] Dance {status}{phase_str}: {event.get('dance', '')}")
        elif etype == 'audio':
            print(f"[Event] Audio frame: {event['samples']} samples")
        elif etype == 'settings':
            parts = []
            if 'volume' in event:
                parts.append(f"volume={event['volume']}")
            if 'length_scale' in event:
                parts.append(f"length_scale={event['length_scale']}")
            print(f"[Event] Settings {status}: {', '.join(parts)}")
        elif etype == 'error':
            print(f"[Event] Error: {event.get('message', '')}")

        # Feed every sniffer (continuous, e.g. record_audio()) with this event.
        with self._sniffers_lock:
            for fn in list(self._sniffers):
                try:
                    fn(event)
                except Exception as e:
                    print(f"[Client] Sniffer error: {e}")

        # Wake up any wait_for() calls whose predicate matches this event.
        with self._waiters_lock:
            for w in self._waiters:
                if not w["event"].is_set() and w["predicate"](event):
                    w["result"][0] = event
                    w["event"].set()

    def add_sniffer(self, fn):
        with self._sniffers_lock:
            self._sniffers.append(fn)

    def remove_sniffer(self, fn):
        with self._sniffers_lock:
            if fn in self._sniffers:
                self._sniffers.remove(fn)

    def wait_for(self, predicate, timeout=30, description="event"):
        """Block the calling thread until an event matching `predicate`
        arrives (checked against every event the listener thread parses),
        or until `timeout` seconds pass. Returns the matching event dict,
        or None on timeout."""
        ev = threading.Event()
        result = [None]
        waiter = {"predicate": predicate, "event": ev, "result": result}
        with self._waiters_lock:
            self._waiters.append(waiter)
        try:
            got = ev.wait(timeout)
        finally:
            with self._waiters_lock:
                if waiter in self._waiters:
                    self._waiters.remove(waiter)
        if not got:
            print(f"[Client] TIMEOUT after {timeout}s waiting for {description}")
            return None
        return result[0]

    def send_command(self, text=None, dance=None, dance_before=None,
                      dance_during=None, length_scale=None, volume=None,
                      set_volume=None, set_length_scale=None):
        """Send a raw command without waiting for anything. length_scale
        and volume are optional per-request overrides; dance_before /
        dance_during / dance place a gesture before, during (concurrent
        with speech), or after the message -- see README.md's JSON
        Protocol section:
          - dance_before: runs fully BEFORE the message starts (sequential)
          - dance_during: runs CONCURRENTLY while the message is spoken --
                          only "wave"/"handshake" are accepted by the
                          server, anything else is refused server-side
          - dance:        runs fully AFTER the message finishes (unchanged,
                           back-compat field name)
          - length_scale: speaking speed for just this utterance
                           (>1.0 slower, <1.0 faster; server default 1.0)
          - volume: loudness for just this utterance, 0.0-1.0+ multiplier
                    on the output samples (server default 1.0)
        Omit any of these to fall back to the server's startup defaults /
        skip that slot entirely.

        set_volume / set_length_scale are a different thing entirely: they
        change the server's RUNTIME DEFAULT (in memory only, not persisted
        to disk -- lost on restart) for every FUTURE request that doesn't
        specify its own length_scale/volume override. Can be sent alone
        (no text/dance needed) or combined with the fields above.
        """
        cmd = {}
        if text:
            cmd['text'] = text
        if dance:
            cmd['dance'] = dance
        if dance_before:
            cmd['dance_before'] = dance_before
        if dance_during:
            cmd['dance_during'] = dance_during
        if length_scale is not None:
            cmd['length_scale'] = length_scale
        if volume is not None:
            cmd['volume'] = volume
        if set_volume is not None:
            cmd['set_volume'] = set_volume
        if set_length_scale is not None:
            cmd['set_length_scale'] = set_length_scale

        if cmd:
            # ensure_ascii=False is required here: the default True would
            # turn e.g. "ę" into the escape sequence ę in the JSON
            # text. The server's j_get() is a minimal hand-rolled parser
            # (not a real JSON parser) that just slices out the raw bytes
            # between quotes -- it does NOT decode \uXXXX escapes, so with
            # the default you'd get the literal characters "ę" spoken
            # and echoed back instead of "ę". Sending raw UTF-8 bytes
            # (what ensure_ascii=False does) is what the server expects.
            msg = json.dumps(cmd, ensure_ascii=False) + '\n'
            self.sock.sendall(msg.encode('utf-8'))
            print(f"[Client] Sent: {cmd}")

    def say_and_wait(self, text, length_scale=None, volume=None, timeout=30):
        """Send TTS text and block until the server reports {"type":"tts",
        "status":"completed"} (or timeout). Returns True on completion."""
        self.send_command(text=text, length_scale=length_scale, volume=volume)
        ev = self.wait_for(
            lambda e: e.get('type') == 'tts' and e.get('status') == 'completed',
            timeout=timeout, description=f"tts completed ({text!r})")
        return ev is not None

    def dance_and_wait(self, dance, timeout=30):
        """Send a dance command (runs in the "after" slot) and block until
        the server reports {"type":"dance","status":"completed"} (or
        timeout)."""
        self.send_command(dance=dance)
        ev = self.wait_for(
            lambda e: e.get('type') == 'dance' and e.get('status') == 'completed',
            timeout=timeout, description=f"dance completed ({dance!r})")
        return ev is not None

    def set_defaults_and_wait(self, set_volume=None, set_length_scale=None, timeout=10):
        """Send set_volume/set_length_scale (the server's in-memory runtime
        defaults for every future request that doesn't override them
        itself -- NOT persisted to disk, lost on restart) and block until
        the server confirms with a {"type":"settings","status":"updated"}
        event. Returns that event, or None on timeout."""
        self.send_command(set_volume=set_volume, set_length_scale=set_length_scale)
        return self.wait_for(
            lambda e: e.get('type') == 'settings' and e.get('status') == 'updated',
            timeout=timeout, description="settings updated")

    def say_and_dance_and_wait(self, text, dance, length_scale=None, volume=None, timeout=30):
        """Send combined TTS + dance (after) and block until BOTH
        completion events have arrived (order not assumed)."""
        self.send_command(text=text, dance=dance, length_scale=length_scale, volume=volume)
        tts_done = threading.Event()
        dance_done = threading.Event()

        def make_predicate(flag_event, etype):
            def predicate(e):
                if e.get('type') == etype and e.get('status') == 'completed':
                    flag_event.set()
                return flag_event.is_set()
            return predicate

        self.wait_for(make_predicate(tts_done, 'tts'), timeout=timeout,
                       description=f"tts completed ({text!r})")
        # tts_done may already be set from the call above; if not, the
        # predicate above only fires once per wait_for call, so check dance
        # separately regardless of order of arrival.
        if not dance_done.is_set():
            self.wait_for(lambda e: e.get('type') == 'dance' and e.get('status') == 'completed',
                           timeout=timeout, description=f"dance completed ({dance!r})")
        return True

    def full_sequence_and_wait(self, text, dance_before=None, dance_during=None,
                                dance=None, length_scale=None, volume=None, timeout=30):
        """Send a command using all three dance slots at once
        (dance_before / dance_during / dance) plus text, and block until
        every completion event for whichever slots were actually set has
        arrived. Matches events by "phase" so it doesn't get confused if,
        say, "before" and "after" use the same dance name.

        Returns a dict {phase: event_or_None} for each slot that was sent
        (plus "tts": event_or_None when text was sent), so the caller can
        tell exactly what finished and what timed out.
        """
        self.send_command(text=text, dance=dance, dance_before=dance_before,
                           dance_during=dance_during, length_scale=length_scale,
                           volume=volume)
        results = {}
        if dance_before:
            results['before'] = self.wait_for(
                lambda e: e.get('type') == 'dance' and e.get('status') == 'completed'
                          and e.get('phase') == 'before',
                timeout=timeout, description=f"dance_before completed ({dance_before!r})")
        if text:
            results['tts'] = self.wait_for(
                lambda e: e.get('type') == 'tts' and e.get('status') == 'completed',
                timeout=timeout, description=f"tts completed ({text!r})")
        if dance_during:
            results['during'] = self.wait_for(
                lambda e: e.get('type') == 'dance' and e.get('status') == 'completed'
                          and e.get('phase') == 'during',
                timeout=timeout, description=f"dance_during completed ({dance_during!r})")
        if dance:
            results['after'] = self.wait_for(
                lambda e: e.get('type') == 'dance' and e.get('status') == 'completed'
                          and e.get('phase') == 'after',
                timeout=timeout, description=f"dance (after) completed ({dance!r})")
        return results

    def record_audio(self, duration=10.0, out_path="recording.wav", startup_timeout=5.0):
        """Actually records `duration` seconds of the robot's microphone by
        collecting real "audio" events from the server (each carries the
        raw PCM as base64 in its "data" field -- see runAudioThread() in
        booster-CogitoNexus.cpp) and writing them to a WAV file. Blocks for
        (roughly) `duration` seconds. Returns the output path, or None if
        no audio was captured at all."""
        frames = []
        meta = {"sample_rate": 16000, "channels": 1, "bits_per_sample": 16}
        got_first = threading.Event()

        def on_audio(event):
            if event.get('type') != 'audio':
                return
            data_b64 = event.get('data')
            if not data_b64:
                return
            frames.append(base64.b64decode(data_b64))
            meta["sample_rate"] = event.get('sample_rate', meta["sample_rate"])
            meta["channels"] = event.get('channels', meta["channels"])
            meta["bits_per_sample"] = event.get('bits_per_sample', meta["bits_per_sample"])
            got_first.set()

        self.add_sniffer(on_audio)
        try:
            print(f"[Client] Recording {duration}s of microphone audio...")
            if not got_first.wait(timeout=startup_timeout):
                print(f"[Client] WARNING: no audio frames received within "
                      f"{startup_timeout}s -- is the capture stream running "
                      f"on the server, and does it include a 'data' field?")
            time.sleep(max(0.0, duration - (0 if got_first.is_set() else startup_timeout)))
        finally:
            self.remove_sniffer(on_audio)

        if not frames:
            print("[Client] No audio captured -- nothing written")
            return None

        raw = b"".join(frames)
        sampwidth = max(1, meta["bits_per_sample"] // 8)
        with wave.open(out_path, "wb") as wf:
            wf.setnchannels(meta["channels"])
            wf.setsampwidth(sampwidth)
            wf.setframerate(meta["sample_rate"])
            wf.writeframes(raw)

        recorded_s = len(raw) / (meta["sample_rate"] * sampwidth * meta["channels"])
        print(f"[Client] Saved {len(raw)} bytes (~{recorded_s:.1f}s) to {out_path}")
        return out_path

    def test_sequence(self):
        """Run test sequence: TTS, dance, combined, TTS overrides,
        dance_before/dance_during/dance, refused dance_during -- each step
        waits for the server's own completion event rather than a guessed
        sleep()."""
        print("\n=== Test Sequence ===\n")

        print("[Test] Step 1: Record 10 seconds of microphone audio")
        self.say_and_wait("Nagrywanie rozpoczęte")
        self.record_audio(duration=10.0, out_path="recording.wav")

        print("\n[Test] Step 2: TTS 'Witaj świecie!'")
        self.say_and_wait("Witaj świecie!")

        print("\n[Test] Step 3: Dance 'wave'")
        self.dance_and_wait("wave")

        print("\n[Test] Step 4: Combined TTS + dance (after)")
        self.say_and_dance_and_wait("Tańczę i mówię!", "dabbing")

        print("\n[Test] Step 5: TTS with length_scale=1.3 (slower), volume=0.5 (quieter)")
        self.say_and_wait("To zdanie jest wolniejsze i cichsze.",
                           length_scale=1.3, volume=0.5)

        print("\n[Test] Step 6: TTS with length_scale=0.85 (faster), volume=1.0 (normal)")
        self.say_and_wait("To zdanie jest szybsze.",
                           length_scale=0.85, volume=1.0)

        print("\n[Test] Step 7: dance_before='wave' + dance_during='handshake' "
              "+ text + dance(after)='dabbing' -- all three slots at once")
        results = self.full_sequence_and_wait(
            "Macham przed, ściskam dłoń w trakcie, i tańczę po.",
            dance_before="wave", dance_during="handshake", dance="dabbing")
        for phase, ev in results.items():
            status = "OK" if ev is not None else "TIMEOUT"
            print(f"  -> {phase}: {status}")

        print("\n[Test] Step 8: dance_during='roundhousekick' should be REFUSED "
              "(only wave/handshake are allowed during speech) -- the server "
              "logs a refusal and just runs the TTS normally, with no "
              "dance_during completed/phase=during event at all")
        self.send_command(text="To zdanie nie powinno mieć kopnięcia w trakcie.",
                           dance_during="roundhousekick")
        tts_ev = self.wait_for(
            lambda e: e.get('type') == 'tts' and e.get('status') == 'completed',
            timeout=30, description="tts completed (step 8)")
        during_ev = self.wait_for(
            lambda e: e.get('type') == 'dance' and e.get('phase') == 'during',
            timeout=2, description="(should NOT appear) dance phase=during")
        if tts_ev is not None and during_ev is None:
            print("  -> OK: TTS completed normally, no dance_during event seen")
        else:
            print("  -> UNEXPECTED: check server logs for the refusal message")

        print("\n[Test] Step 9: set_volume/set_length_scale -- change the "
              "server's RUNTIME DEFAULTS (in-memory only, not saved to disk) "
              "and confirm the next plain say_and_wait() picks them up "
              "without specifying its own length_scale/volume")
        settings_ev = self.set_defaults_and_wait(set_volume=0.5, set_length_scale=1.3)
        print(f"  -> settings confirmation: {'OK' if settings_ev else 'TIMEOUT'}")
        self.say_and_wait("To zdanie powinno być teraz ciszej i wolniej z domyślnych ustawień.")
        print("  -> restoring defaults to volume=1.0, length_scale=1.0")
        self.set_defaults_and_wait(set_volume=1.0, set_length_scale=1.0)

        print("\n=== Test Complete ===\n")

    def close(self):
        self._running = False
        if self.sock:
            self.sock.close()

def main():
    host = sys.argv[1] if len(sys.argv) > 1 else '127.0.0.1'
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 9000

    print(f"=== CogitoNexus Test Client ===")
    print(f"Connecting to {host}:{port}")

    client = CogitoClient(host, port)

    try:
        client.connect()
        client.test_sequence()

        # Keep listening for events (e.g. controller button presses)
        print("[Client] Listening for events (Ctrl+C to exit)...")
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n[Client] Exiting...")
    except Exception as e:
        print(f"[Client] Error: {e}")
    finally:
        client.close()

if __name__ == '__main__':
    main()
