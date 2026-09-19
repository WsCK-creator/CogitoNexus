#!/usr/bin/env python3
"""
record_channels.py

Łączy się z serwerem CogitoNexus na Boosterze (ten sam protokół co
test_client.py), nagrywa kilka sekund SUROWEGO strumienia audio ("raw",
niezmiksowane próbki wprost z mikrofonu-array -- patrz booster-CogitoNexus.cpp
onAudioFrame) i zapisuje KAŻDY kanał osobno do pliku WAV: kanal1.wav,
kanal2.wav, kanal3.wav itd. -- bez żadnego mieszania/uśredniania kanałów,
żeby można było odsłuchać i porównać je pojedynczo.

Serwer wysyła zdarzenia "audio" ciągłym strumieniem od razu po połączeniu
(patrz runAudioThread() w booster-CogitoNexus.cpp) -- nie trzeba wysyłać
żadnej komendy, żeby zacząć nagrywanie.

Użycie:
    python record_channels.py [robot_ip] [port] [czas_w_sekundach]

Przykład:
    python record_channels.py 192.168.1.50 9000 5
"""

import base64
import json
import socket
import sys
import time
import wave


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 9000
    duration = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0

    print(f"=== Nagrywanie kanałów audio ===")
    print(f"Łączenie z {host}:{port} ...")

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))
    print("Połączono. Czekam na strumień audio...")

    buffer = ""
    frames_raw = []
    meta = {"sample_rate": 16000, "channels": 1, "bits_per_sample": 16}

    start_time = None
    got_first = False

    sock.settimeout(1.0)
    deadline_after_first = None

    try:
        while True:
            try:
                data = sock.recv(65536)
            except socket.timeout:
                if got_first and time.monotonic() >= deadline_after_first:
                    break
                continue

            if not data:
                print("Połączenie zamknięte przez serwer.")
                break

            buffer += data.decode("utf-8", errors="ignore")
            while "\n" in buffer:
                line, buffer = buffer.split("\n", 1)
                if not line.strip():
                    continue
                try:
                    event = json.loads(line)
                except json.JSONDecodeError:
                    continue

                if event.get("type") != "audio":
                    continue

                data_b64 = event.get("data")
                if not data_b64:
                    continue

                if not got_first:
                    got_first = True
                    start_time = time.monotonic()
                    deadline_after_first = start_time + duration
                    meta["sample_rate"] = event.get("sample_rate", meta["sample_rate"])
                    meta["channels"] = event.get("channels", meta["channels"])
                    meta["bits_per_sample"] = event.get("bits_per_sample", meta["bits_per_sample"])
                    print(f"Pierwsza ramka audio: {meta['sample_rate']}Hz, "
                          f"{meta['channels']} kanał(ów), {meta['bits_per_sample']} bit. "
                          f"Nagrywanie {duration}s...")

                frames_raw.append(base64.b64decode(data_b64))

            if got_first and time.monotonic() >= deadline_after_first:
                break
    finally:
        sock.close()

    if not frames_raw:
        print("Nie odebrano żadnych ramek audio -- nic nie zapisano. "
              "Sprawdź, czy serwer cogito-server działa i czy capture "
              "stream wystartował (logi: journalctl -u cogito-server -f).")
        return

    raw = b"".join(frames_raw)
    channels = max(1, meta["channels"])
    sampwidth = max(1, meta["bits_per_sample"] // 8)
    sample_rate = meta["sample_rate"]

    total_samples = len(raw) // sampwidth
    if sampwidth != 2:
        print(f"Uwaga: bits_per_sample={meta['bits_per_sample']} (nietypowe, "
              f"skrypt zakłada 16-bit PCM) -- kontynuuję mimo to.")

    frame_count = total_samples // channels
    print(f"Odebrano {len(raw)} bajtów (~{frame_count / sample_rate:.1f}s), "
          f"{channels} kanał(ów) -- deinterleaving i zapis...")

    # Deinterleaving: bajty w "raw" są przeplecione po kanałach
    # (frame0_ch0, frame0_ch1, ..., frame0_chN-1, frame1_ch0, ...).
    import array
    samples = array.array("h")  # signed short, 16-bit
    samples.frombytes(raw[: frame_count * channels * 2])
    if sys.byteorder == "big":
        samples.byteswap()

    for ch in range(channels):
        channel_samples = samples[ch::channels]
        out_path = f"kanal{ch + 1}.wav"
        with wave.open(out_path, "wb") as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(sample_rate)
            wf.writeframes(channel_samples.tobytes())
        print(f"  -> zapisano {out_path} ({len(channel_samples) / sample_rate:.1f}s)")

    print("Gotowe.")


if __name__ == "__main__":
    main()
