"""Transcription worker: takes WAV files from the spool folder (oldest first), transcribes them
with faster-whisper and writes the lines to SQLite. A file is deleted only after its text is saved.

Started by the lifelog service, or by hand:  python -m lifelog_worker [--config PATH] [--once]
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import sqlite3
import sys
import threading
import time
import tomllib
import wave
from pathlib import Path

import numpy as np

log = logging.getLogger("lifelog.worker")
REPO = Path(__file__).resolve().parents[2]

# Phrases Whisper invents on music or noise; dropped when they are the whole line.
HALLUCINATIONS = {
    "thank you.", "thanks for watching.", "thanks for watching!", "thank you for watching.",
    "thank you for watching!", "you", "bye.", ".", "subtitles by the amara.org community",
}


def add_cuda_dlls() -> None:
    """CTranslate2 needs cuBLAS/cuDNN DLLs; pip puts them under site-packages/nvidia/*/bin."""
    for base in map(Path, sys.path):
        for bin_dir in base.glob("nvidia/*/bin"):
            os.add_dll_directory(str(bin_dir))
            os.environ["PATH"] = f"{bin_dir}{os.pathsep}{os.environ['PATH']}"


def load_config(path: Path) -> dict:
    with open(path, "rb") as f:
        return tomllib.load(f)


def load_model(worker_cfg: dict):
    from faster_whisper import WhisperModel

    name = worker_cfg.get("model", "large-v3-turbo")
    device = worker_cfg.get("device", "cuda")
    compute = worker_cfg.get("compute_type", "float16")
    try:
        model = WhisperModel(name, device=device, compute_type=compute)
        log.info("model %s on %s (%s)", name, device, compute)
        return model
    except Exception as e:  # no GPU, missing DLLs, out of memory…
        if device == "cpu":
            raise
        log.warning("could not load on %s (%s); falling back to CPU, which is much slower", device, e)
        return WhisperModel(name, device="cpu", compute_type="int8")


def read_wav(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as w:
        if (w.getframerate(), w.getnchannels(), w.getsampwidth()) != (16000, 1, 2):
            raise ValueError("expected 16 kHz mono 16-bit")
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    return pcm.astype(np.float32) / 32768.0


def transcribe(model, audio: np.ndarray, language: str | None) -> list[tuple[float, float, str]]:
    segments, _ = model.transcribe(
        audio,
        language=language or None,
        vad_filter=True,  # Silero: skips music, game noise and silence
        vad_parameters={"min_silence_duration_ms": 500},
        condition_on_previous_text=False,  # stops one bad line from repeating into the next
        beam_size=5,
    )
    lines = []
    for s in segments:
        text = s.text.strip()
        if not text or text.lower() in HALLUCINATIONS:
            continue
        if s.no_speech_prob > 0.6 and s.avg_logprob < -1.0:
            continue
        lines.append((s.start, s.end, text))
    return lines


def process(model, db: sqlite3.Connection, wav: Path, language: str | None) -> int:
    meta = json.loads(wav.with_suffix(".json").read_text(encoding="utf-8"))
    lines = transcribe(model, read_wav(wav), language)
    speaker = "me" if meta["source"] == "mic" else (meta.get("exe") or "unknown")
    with db:  # one transaction per file
        db.executemany(
            "INSERT INTO segments (start_ts, end_ts, source, speaker, exe, pid, title, device, text)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            [
                (meta["start_ts"] + start, meta["start_ts"] + end, meta["source"], speaker,
                 meta.get("exe"), meta.get("pid"), meta.get("title"), meta.get("device"), text)
                for start, end, text in lines
            ],
        )
    return len(lines)


def remove(wav: Path) -> None:
    wav.unlink(missing_ok=True)
    wav.with_suffix(".json").unlink(missing_ok=True)


def main() -> int:
    default_config = os.environ.get("LIFELOG_CONFIG") or Path(os.environ.get("APPDATA", ".")) / "lifelog" / "config.toml"
    ap = argparse.ArgumentParser(prog="lifelog_worker")
    ap.add_argument("--config", type=Path, default=Path(default_config))
    ap.add_argument("--once", action="store_true", help="empty the queue, then exit")
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s [worker] %(message)s", datefmt="%H:%M:%S")

    cfg = load_config(args.config)
    data_dir = Path(cfg["data_dir"])
    spool = data_dir / "spool"
    failed = spool / "failed"
    spool.mkdir(parents=True, exist_ok=True)
    language = cfg.get("worker", {}).get("language", "en")

    db = sqlite3.connect(data_dir / "lifelog.db", timeout=30)
    db.executescript((REPO / "schema.sql").read_text(encoding="utf-8"))

    stop = threading.Event()
    if not args.once and not sys.stdin.isatty():
        # The service holds our stdin open; when it closes (service stopped or died), finish up.
        def watch_parent() -> None:
            try:
                sys.stdin.buffer.read()
            finally:
                stop.set()

        threading.Thread(target=watch_parent, daemon=True).start()

    add_cuda_dlls()
    model = load_model(cfg.get("worker", {}))

    while not stop.is_set():
        queue = sorted(spool.glob("*.wav"))
        if not queue:
            if args.once:
                break
            stop.wait(1.0)
            continue
        for wav in queue:
            if stop.is_set():
                break
            started = time.monotonic()
            try:
                n = process(model, db, wav, language)
            except Exception:
                log.exception("failed on %s; moved to %s", wav.name, failed)
                failed.mkdir(exist_ok=True)
                for f in (wav, wav.with_suffix(".json")):
                    if f.exists():
                        f.replace(failed / f.name)
                continue
            remove(wav)
            log.info("%s: %d lines in %.1fs (%d queued)", wav.name, n, time.monotonic() - started,
                     len(list(spool.glob("*.wav"))))
    db.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
