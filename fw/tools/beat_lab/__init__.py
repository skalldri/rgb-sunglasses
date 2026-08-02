"""beat_lab — offline analysis tooling for the firmware beat detector (issue #264).

Workflow overview (full doc: fw/docs/beat-detection-debugging.md):

  1. Record on-device:   `sound agc gain 0x28` + `sound mic record_wav 30`
  2. Pull sound.wav + sound.wav.csv off the USB mass-storage disk
  3. Replay on host:     replay.py --wav sound.wav --out host.txt
  4. Validate replica:   compare.py --device sound.wav.csv --host host.txt
  5. Score vs reference: evaluate.py --frames host.txt --ref-librosa beats
  6. Visualize:          report.py --wav sound.wav --frames host.txt --out report.png
  7. Tune:               replay.py --sweep "alpha=2.0:5.0:0.5" --ref-librosa beats

Shared frame codec lives in frames.py; the wire format is produced by
tap_frame_format() in fw/src/sound/sound.cpp and by the replay app in
fw/tests/sound/audio_dsp_replay/src/main.cpp — the three must stay in sync.
"""
