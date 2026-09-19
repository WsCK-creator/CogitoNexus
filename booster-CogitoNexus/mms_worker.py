#!/usr/bin/env python3
"""Persistent MMS-TTS (facebook/mms-tts-pol) worker for CogitoNexus.

Drop-in replacement for piper_worker.py -- speaks the EXACT SAME stdin/
stdout line protocol, so the C++ side (PiperTTS class in
booster-CogitoNexus.cpp) needed zero protocol changes to switch engines,
only different CLI arguments (see main()'s PiperTTS construction).

Protocol (identical to piper_worker.py):
  - reads ONE line from stdin at a time. The line is EITHER:
      * plain text -- synthesized using the --length-scale given at startup
      * a JSON object {"text": "...", "length_scale": 1.23} -- "length_scale"
        is optional and overrides the startup default for this one line only
  - synthesizes it with a VitsModel loaded once at startup (CUDA if
    available and requested)
  - writes a correctly-headered WAV file into --work-dir
  - prints the absolute path to that WAV file to stdout, followed by a
    newline, and flushes immediately
  - on any per-utterance error, prints "ERROR: <message>" instead of a path

"length_scale" semantics MATCH Piper's, not VITS's own native parameter:
here (as in Piper) >1.0 = SLOWER, <1.0 = FASTER, 1.0 = natural pace. VITS's
own `speaking_rate` parameter is the inverse (higher = faster), so this
script converts internally (speaking_rate = 1.0 / length_scale) -- this way
nothing on the C++ side (set_length_scale, per-request "length_scale", the
JSON protocol/docs) needed to change meaning when the engine swapped.

--noise-scale / --noise-scale-duration are VITS-specific "expressiveness"
knobs with no Piper equivalent (Piper's job here didn't expose them) --
higher noise_scale = more variation in timbre/intonation, higher
noise_scale_duration = more variation in rhythm/syllable timing. Piper's
CLI never passed these, but since this script fully replaces
piper_worker.py (not running alongside it), it's fine for the C++ side to
always pass them.

--espeak-data is accepted and IGNORED -- MMS-TTS's tokenizer doesn't use
espeak-ng at all, but the C++ side's spawnWorker() may still be configured
with a leftover Piper espeak-data path (or "") for CLI-position
compatibility with the old invocation, so this script tolerates it rather
than erroring on an path that no longer means anything.
"""
import argparse
import json
import os
import sys
import time
import wave

import numpy as np


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True,
                     help="Local directory holding the MMS-TTS model files "
                          "(config.json, model.safetensors, tokenizer files, ...) "
                          "-- NOT a bare Hugging Face hub id, so this never needs "
                          "network access at runtime. See README.md for the "
                          "one-time snapshot_download() that populates this dir.")
    ap.add_argument("--espeak-data", default=None, help=argparse.SUPPRESS)  # unused, accepted for CLI compat
    ap.add_argument("--work-dir", required=True)
    ap.add_argument("--length-scale", type=float, default=1.0,
                     help="Piper-style semantics: >1.0 slower, <1.0 faster, 1.0 natural")
    ap.add_argument("--noise-scale", type=float, default=0.667,
                     help="VITS: higher = more variation in voice timbre/intonation")
    ap.add_argument("--noise-scale-duration", type=float, default=0.8,
                     help="VITS: higher = more variation in rhythm/syllable timing")
    ap.add_argument("--cuda", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.work_dir, exist_ok=True)

    # We load from a real local directory (args.model), not a hub id, so
    # transformers resolves everything from disk already -- this env var is
    # just defense-in-depth against any incidental hub-connectivity check.
    os.environ.setdefault("HF_HUB_OFFLINE", "1")
    os.environ.setdefault("TRANSFORMERS_OFFLINE", "1")

    # Imported after argparse/env setup so --help doesn't pay torch's
    # multi-second import cost, and so HF_HUB_OFFLINE is set before
    # transformers reads it at import time.
    import torch
    from transformers import VitsModel, AutoTokenizer

    device = "cuda" if (args.cuda and torch.cuda.is_available()) else "cpu"
    if args.cuda and device == "cpu":
        print(f"[MMS] WARNING: --cuda given but torch.cuda.is_available() is False "
              f"-- running on CPU instead (near-realtime may not hold)", file=sys.stderr)

    tokenizer = AutoTokenizer.from_pretrained(args.model)
    model = VitsModel.from_pretrained(args.model)
    model.config.noise_scale = args.noise_scale
    model.config.noise_scale_duration = args.noise_scale_duration
    model = model.to(device)
    model.eval()

    sample_rate = model.config.sampling_rate  # 16000 for facebook/mms-tts-pol

    # Warm-up synthesis (not counted/printed) -- pays the one-time cost of
    # first CUDA kernel launches / lazy init before READY, same idea as
    # piper_worker.py's warmup, so the FIRST real request isn't the slow one.
    warm_inputs = tokenizer("Test.", return_tensors="pt").to(device)
    with torch.no_grad():
        model(**warm_inputs, speaking_rate=1.0)

    print("READY", flush=True)

    for line in sys.stdin:
        raw = line.strip()
        if not raw:
            continue

        text = raw
        length_scale = args.length_scale
        if raw.startswith("{"):
            try:
                obj = json.loads(raw)
                text = obj.get("text", "")
                if obj.get("length_scale") is not None:
                    length_scale = float(obj["length_scale"])
            except (json.JSONDecodeError, TypeError, ValueError):
                # Not valid JSON after all -- treat the whole line as
                # literal text rather than dropping the request.
                text = raw

        if not text:
            continue

        # Piper convention (>1 slower) -> VITS's own convention (>1 faster).
        speaking_rate = (1.0 / length_scale) if length_scale > 0 else 1.0

        out_path = os.path.join(args.work_dir, f"{time.time_ns()}.wav")
        try:
            inputs = tokenizer(text, return_tensors="pt").to(device)
            with torch.no_grad():
                waveform = model(**inputs, speaking_rate=speaking_rate).waveform
            pcm_float = waveform.squeeze().detach().cpu().numpy()
            pcm16 = np.clip(pcm_float * 32767.0, -32768, 32767).astype(np.int16)

            with wave.open(out_path, "wb") as wf:
                wf.setnchannels(1)
                wf.setsampwidth(2)  # 16-bit
                wf.setframerate(sample_rate)
                wf.writeframes(pcm16.tobytes())

            print(out_path, flush=True)
        except Exception as exc:  # noqa: BLE001
            print(f"ERROR: {exc}", flush=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
