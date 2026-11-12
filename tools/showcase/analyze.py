#!/usr/bin/env python3
"""Music analysis for the showcase: the beat grid and audio-reactive envelopes.

  analyze.py music/<track>.mp3   ->  music/analysis.json

The grid is fitted to the attacks rather than taken from a beat tracker (librosa's tracker locked
onto a 4:3 cross-rhythm of this track): the period in 90-140 bpm and the phase that put the most
attack energy on the beats, then the bar (4 beats) and phrase (8 beats) phases the same way.
`hit` (attack strength) and `low` (bass level) are sampled at the video frame rate and normalised
at their 98th percentile; the intro and the compositor drive their pulses from them.
"""
import json, sys
from pathlib import Path
import numpy as np
import librosa

FPS = 60
path = Path(sys.argv[1] if len(sys.argv) > 1 else "music/Discovery.mp3")
y, sr = librosa.load(path, sr=22050, mono=True)
duration = len(y) / sr
hop = 256
onset = librosa.onset.onset_strength(y=y, sr=sr, hop_length=hop)
times = librosa.frames_to_time(np.arange(len(onset)), sr=sr, hop_length=hop)
S = np.abs(librosa.stft(y, n_fft=2048, hop_length=hop))
freqs = librosa.fft_frequencies(sr=sr, n_fft=2048)
low = S[(freqs >= 30) & (freqs <= 140)].mean(axis=0)

def norm(v):
    return np.clip(v / (np.percentile(v, 98) + 1e-9), 0, 1.5)

def energy_at(t):
    return np.interp(t, times, onset, left=0, right=0)

# Period: the lag in 90-140 bpm whose first 32 multiples the attack envelope repeats at best
# (its autocorrelation); the phase that then puts the most attack energy on the beats.
ac = librosa.autocorrelate(onset - onset.mean())
lag_t = times - times[0]
def repeats(period):
    return np.interp(period * np.arange(1, 33), lag_t, ac).mean()
period = max(np.linspace(60 / 140, 60 / 90, 4000), key=repeats)
period = max(np.linspace(period * 0.998, period * 1.002, 2000), key=repeats)
phase = max(np.linspace(0, period, 400, endpoint=False), key=lambda p: energy_at(np.arange(p, duration, period)).mean())
beats = np.arange(phase, duration, period)
# Bars and phrases: the offset whose first beats carry the most attack energy.
bar = max(range(4), key=lambda k: energy_at(beats[k::4]).mean())
phrase = max(range(2), key=lambda k: energy_at(beats[bar::4][k::2]).mean())
downbeats = beats[bar::4]
phrases = downbeats[phrase::2]

t_video = np.arange(0, duration, 1 / FPS)
result = {
    "duration": duration, "tempo": 60 / period, "fps": FPS,
    "beats": [round(float(t), 3) for t in beats],
    "downbeats": [round(float(t), 3) for t in downbeats],
    "phrases": [round(float(t), 3) for t in phrases],
    "hit": [round(float(v), 3) for v in np.interp(t_video, times, norm(onset))],
    "low": [round(float(v), 3) for v in np.interp(t_video, times, norm(low))],
}
json.dump(result, open(path.parent / "analysis.json", "w"))
print(f"duration {duration:.2f}s tempo {result['tempo']:.2f} bpm, {len(beats)} beats, first beat {beats[0]:.3f}, "
      f"first bar {downbeats[0]:.3f}, first phrase {phrases[0]:.3f}")
