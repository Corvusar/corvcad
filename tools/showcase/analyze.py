"""Beat, bar and section map of the showcase music -> music/analysis.json (and a printed summary)."""
import json, sys
import numpy as np
import librosa

path = sys.argv[1] if len(sys.argv) > 1 else "music/Discovery.mp3"
y, sr = librosa.load(path, sr=22050, mono=True)
duration = len(y) / sr
hop = 512

tempo, beats = librosa.beat.beat_track(y=y, sr=sr, hop_length=hop, units="frames")
beat_t = librosa.frames_to_time(beats, sr=sr, hop_length=hop)
onset_env = librosa.onset.onset_strength(y=y, sr=sr, hop_length=hop)
onsets = librosa.onset.onset_detect(onset_envelope=onset_env, sr=sr, hop_length=hop, units="time")

# Loudness (RMS dB) per beat, and low-band energy (kick) per beat.
rms = librosa.feature.rms(y=y, hop_length=hop)[0]
S = np.abs(librosa.stft(y, hop_length=hop, n_fft=2048))
freqs = librosa.fft_frequencies(sr=sr, n_fft=2048)
low = S[freqs < 150].mean(axis=0)
def per_beat(series):
    edges = list(beats) + [len(series)]
    return [float(series[edges[i]:max(edges[i] + 1, edges[i + 1])].mean()) for i in range(len(beats))]
beat_rms = per_beat(rms)
beat_low = per_beat(low)

# Downbeats: phase (0..3) of the 4/4 bar whose beats carry the most low-band energy.
phase = int(np.argmax([np.mean(beat_low[p::4]) for p in range(4)]))
downbeats = beat_t[phase::4]

# Sections: agglomerative segmentation on beat-synchronous chroma + MFCC.
chroma = librosa.feature.chroma_cqt(y=y, sr=sr, hop_length=hop)
mfcc = librosa.feature.mfcc(y=y, sr=sr, hop_length=hop, n_mfcc=13)
feats = librosa.util.sync(np.vstack([librosa.util.normalize(chroma), librosa.util.normalize(mfcc)]), beats)
k = 10
bounds = librosa.segment.agglomerative(feats, k)
section_t = [float(beat_t[b]) if b < len(beat_t) else duration for b in bounds]

# Loudness curve in 1 s windows, for build-ups and the climax.
win = int(sr / hop)
loud = [float(20 * np.log10(rms[i:i + win].mean() + 1e-9)) for i in range(0, len(rms), win)]

result = {
    "duration": duration, "tempo": float(np.atleast_1d(tempo)[0]),
    "beats": [round(float(t), 3) for t in beat_t],
    "downbeats": [round(float(t), 3) for t in downbeats],
    "beat_rms": [round(v, 5) for v in beat_rms], "beat_low": [round(v, 4) for v in beat_low],
    "onsets": [round(float(t), 3) for t in onsets],
    "sections": [round(t, 3) for t in section_t],
    "loudness_db_per_second": [round(v, 2) for v in loud],
}
json.dump(result, open("music/analysis.json", "w"), indent=1)
print(f"duration {duration:.2f}s tempo {result['tempo']:.1f} bpm, {len(beat_t)} beats, downbeat phase {phase}")
print("sections:", [round(t, 1) for t in section_t])
print("loudness per 5 s:", [round(float(np.mean(loud[i:i + 5])), 1) for i in range(0, len(loud), 5)])
