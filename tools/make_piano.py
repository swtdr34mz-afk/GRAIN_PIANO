#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 swtdr34mz
"""CHORD RUN's GRAND PIANO: res/piano.bin (the resource region's payload) from the Salamander Grand Piano V3
samples (Alexander Holm, CC-BY 3.0: https://archive.org/details/SalamanderGrandPianoV3, as remapped in
https://github.com/sfzinstruments/SalamanderGrandPiano). Nine notes a tritone apart (F#2 .. F#6), velocity layer 12 of
16, mono, 22,050 Hz, levelled, cut to a few seconds with a crossfaded loop at the end (the engine lets it fade on), and
4-bit IMA ADPCM. Run: tools/make_piano.py PATH/TO/SalamanderGrandPiano/Samples  (needs ffmpeg and numpy).

The payload (little-endian): "PNO1", the count, the rate, 0; then per note 20 bytes: root (MIDI), the ADPCM step index
at the start and at the loop, 0, the predictor at the start and at the loop (int16), the data's offset in the payload,
its length and the loop's start (samples); then the data (two samples a byte, the first in the low nibble)."""
import struct, subprocess, sys
import numpy as np

SR = 22050
NOTES = [("F#2", 42, 3.4), ("C3", 48, 3.4), ("F#3", 54, 3.2), ("C4", 60, 3.1), ("F#4", 66, 2.9), ("C5", 72, 2.7),
         ("F#5", 78, 2.4), ("C6", 84, 2.0), ("F#6", 90, 1.6)]
LOOP_S, XFADE_S, RMS_TARGET = 0.20, 0.06, 0.20
STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
        5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
        27086, 29794, 32767]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


def load(path):
    raw = subprocess.run(["ffmpeg", "-loglevel", "error", "-i", path, "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"],
                         check=True, capture_output=True).stdout
    return np.frombuffer(raw, dtype=np.float32).astype(np.float64)


def encode(x, pred, idx):
    """IMA ADPCM: the nibbles, and the decoder's state before each sample"""
    nibs, states = [], []
    for v in x:
        states.append((pred, idx))
        step = STEP[idx]
        diff = int(v) - pred
        nib = 8 if diff < 0 else 0
        diff = abs(diff)
        vp = step >> 3
        if diff >= step: nib |= 4; diff -= step; vp += step
        step >>= 1
        if diff >= step: nib |= 2; diff -= step; vp += step
        step >>= 1
        if diff >= step: nib |= 1; vp += step
        pred = max(-32768, min(32767, pred - vp if nib & 8 else pred + vp))
        idx = max(0, min(88, idx + INDEX[nib & 7]))
        nibs.append(nib)
    return nibs, states


def decode(nibs, pred, idx):
    out = []
    for nib in nibs:
        step = STEP[idx]
        vp = step >> 3
        if nib & 4: vp += step
        if nib & 2: vp += step >> 1
        if nib & 1: vp += step >> 2
        pred = max(-32768, min(32767, pred - vp if nib & 8 else pred + vp))
        idx = max(0, min(88, idx + INDEX[nib & 7]))
        out.append(pred)
    return np.array(out, dtype=np.float64)


def main(src):
    entries, blobs, off = [], [], 0
    for name, root, secs in NOTES:
        x = load(f"{src}/{name}v12.flac")
        on = int(np.argmax(np.abs(x) > np.abs(x).max() * 0.02))
        x = x[max(0, on - int(0.002 * SR)):]                  # from the hammer (2 ms before)
        n = int(secs * SR)
        x = x[:n].copy()
        x *= RMS_TARGET / np.sqrt(np.mean(x[:int(0.3 * SR)] ** 2))   # levelled on the first 0.3 s
        pk = np.abs(x).max()
        if pk > 0.95: x *= 0.95 / pk
        # the loop: the last LOOP_S seconds; its end crossfades into what leads to its start (no click at the jump)
        lp, xf = n - int(LOOP_S * SR), int(XFADE_S * SR)
        w = np.linspace(0.0, 1.0, xf)
        x[n - xf:] = x[n - xf:] * (1.0 - w) + x[lp - xf:lp] * w
        q = np.clip(np.round(x * 32767.0), -32768, 32767).astype(np.int64)
        best = min(range(0, 89, 2), key=lambda i: np.abs(decode(encode(q[:256], 0, i)[0], 0, i) - q[:256]).sum())
        nibs, states = encode(q, 0, best)
        dec = decode(nibs, 0, best)
        snr = 10 * np.log10(np.sum(q[:int(SR)] ** 2) / max(1.0, np.sum((dec[:int(SR)] - q[:int(SR)]) ** 2)))
        if len(nibs) & 1: nibs.append(0)
        data = bytes((nibs[i] & 15) | ((nibs[i + 1] & 15) << 4) for i in range(0, len(nibs), 2))
        lpred, lidx = states[lp]
        entries.append(struct.pack("<BBBBhhIII", root, best, lidx, 0, 0, lpred, off, n, lp))
        blobs.append(data)
        off += len(data)
        print(f"{name}: {n / SR:.2f} s, loop at {lp / SR:.2f} s, start index {best}, SNR (1st s) {snr:.1f} dB, {len(data)} B")
    head = struct.pack("<4sIII", b"PNO1", len(NOTES), SR, 0) + b"".join(entries)
    base = len(head)
    entries2 = [struct.pack("<BBBBhhIII", *(struct.unpack("<BBBBhhIII", e)[:6]),
                            struct.unpack("<BBBBhhIII", e)[6] + base, *struct.unpack("<BBBBhhIII", e)[7:]) for e in entries]
    payload = struct.pack("<4sIII", b"PNO1", len(NOTES), SR, 0) + b"".join(entries2) + b"".join(blobs)
    open("res/piano.bin", "wb").write(payload)
    print(f"res/piano.bin: {len(payload)} B")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "Samples")
