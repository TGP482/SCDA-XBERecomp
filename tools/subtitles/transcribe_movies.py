"""Transcribe the English dialogue of the game's Bink movies into SRT subtitles.

The story movies carry no subtitle text, so this runs faster-whisper over the
English audio and writes speech-only cues to subtitles/<movie>.srt, which the
exe loads at runtime. The output is a first draft: review it by hand before
committing it.

Story movies have 8 audio tracks; track id 1 is the mono centre channel, which
holds the English dialogue on its own. The other movies have 5 tracks and
track id 0 is the English mix.

Setup (Python 3.12, since ctranslate2 has no wheels for newer versions):
    uv venv --python 3.12 "%LOCALAPPDATA%\\scda-subtitles\\venv"
    uv pip install --python "%LOCALAPPDATA%\\scda-subtitles\\venv\\Scripts\\python.exe" ^
        faster-whisper nvidia-cublas-cu12 "nvidia-cudnn-cu12>=9,<10"

Usage:
    python tools/subtitles/transcribe_movies.py --movies iceland_intro
"""

import argparse
import math
import os
import re
import struct
import sys
import unicodedata
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
VIDEOS_SUBDIR = Path("Default/DataXb/videos")
SKIPPED_MOVIES = re.compile(r"^(fv-.*|logos|back_menu)$", re.IGNORECASE)

SAMPLE_RATE = 16000
SILENT_DB = -50.0  # a track quieter than this (RMS) holds no dialogue

# Cast and terms, from the speaker prefixes and lines of the in-game speech text.
INITIAL_PROMPT = (
    "Sam Fisher, Lambert, Assistant Director Williams, Hisham Hamza, Jamie Washington, "
    "Emile Dufraisne, Enrica Villablanca, Carson Moss, Massoud, Takfir, Raheem Kadir, "
    "Cole Yeagher, Barnham, Sykes, Sarah Fisher, JBA, John Brown's Army, NSA, "
    "Third Echelon, Splinter Cell, Red Mercury, Ellsworth, Iceland, Kinshasa, Okhotsk, "
    "Cozumel, Los Angeles, New Orleans."
)

NO_SPEECH_MAX = 0.6
AVG_LOGPROB_MIN = -1.0
WORD_PROB_MIN = 0.7  # mean word probability that overrules a high no_speech_prob
ONSET_DB = -55.0  # a word starts at the first 10 ms frame louder than this

# Lines Whisper invents over silence or music.
FILLER = re.compile(
    r"thanks? (you )?for watching|subtitles? by|subtitled by|captions? by|"
    r"captioning by|transcri(bed|ption) by|translated by|amara\.org|"
    r"please subscribe|like and subscribe|see you (in the )?next video",
    re.IGNORECASE,
)

# Mishearings Whisper keeps making despite the prompt, and the game's spellings: (pattern, fix).
CORRECTIONS = [
    (re.compile(r"\bThird Esch?ell?on\b", re.IGNORECASE), "Third Echelon"),
    # The last dot of "J.B.A." also ends the sentence when the text or a sentence ends there.
    (re.compile(r"\bJ\.B\.A\.(?=$|\s+[A-Z])"), "JBA."),
    (re.compile(r"\bJ\.B\.A\.?(?=\W|$)"), "JBA"),
    (re.compile(r"\bN\.S\.A\.(?=$|\s+[A-Z])"), "NSA."),
    (re.compile(r"\bN\.S\.A\.?(?=\W|$)"), "NSA"),
    (re.compile(r"\bDuf(resne|rane|raine)\b"), "Dufraisne"),
    (re.compile(r"\bEmil\b"), "Emile"),
    (re.compile(r"\bYeager\b"), "Yeagher"),
    (re.compile(r"\bBJ Sykes\b"), "B.J. Sykes"),
    (re.compile(r"\bred mercury\b"), "Red Mercury"),
]

MAX_LINE = 42
PAUSE_SPLIT = 0.6
MIN_DURATION = 1.0
MAX_DURATION = 6.0
TAIL = 0.3
MIN_GAP = 0.07

MUSIC_SYMBOLS = "\u2669\u266a\u266b\u266c\u266d\u266e\u266f"
LATIN1_MAP = str.maketrans({
    "\u2018": "'", "\u2019": "'", "\u201a": "'", "\u201b": "'", "\u2032": "'",
    "\u201c": '"', "\u201d": '"', "\u201e": '"', "\u201f": '"', "\u2033": '"',
    "\u2010": "-", "\u2011": "-", "\u2012": "-", "\u2013": "-", "\u2014": "-", "\u2015": "-",
    "\u2026": "...", "\u00a0": " ",
})
NOT_SENTENCE_END = {"mr.", "mrs.", "ms.", "dr.", "st.", "lt.", "col.", "gen.", "sgt."}
# Words that start a clause or phrase: lines and cues break before them, never after.
CONJUNCTIONS = {
    "and", "or", "but", "nor", "so", "as", "than", "that", "if", "since", "because", "when",
    "while", "where", "who", "which", "though", "although", "unless", "until",
}
PREPOSITIONS = {
    "of", "to", "in", "on", "at", "by", "for", "from", "with", "into", "about", "through",
    "over", "under", "after", "before", "out", "across", "against", "without", "between",
    "toward", "towards", "onto", "upon", "during", "like",
}
WEAK_LINE_ENDS = CONJUNCTIONS | PREPOSITIONS | {
    "a", "an", "the", "my", "your", "his", "her", "its", "our", "their",
    "is", "are", "was", "were", "i",
}


def add_cuda_dll_dirs():
    """Lets ctranslate2 find the cuBLAS/cuDNN DLLs of the nvidia-* wheels."""
    if sys.platform != "win32":
        return
    for base in map(Path, sys.path):
        nvidia = base / "nvidia"
        if not nvidia.is_dir():
            continue
        for bin_dir in nvidia.glob("*/bin"):
            os.add_dll_directory(str(bin_dir))
            # cuDNN loads its sub-libraries through the plain PATH search.
            os.environ["PATH"] = str(bin_dir) + os.pathsep + os.environ.get("PATH", "")


# --- Bink audio ---------------------------------------------------------------

@dataclass
class AudioTrack:
    index: int  # position in the file, which is also PyAV's audio stream index
    track_id: int
    rate: int
    stereo: bool


def read_bink_tracks(path):
    with open(path, "rb") as f:
        header = f.read(0x2C)
        if header[:3] != b"BIK":
            raise ValueError(f"{path.name}: not a Bink file")
        (count,) = struct.unpack_from("<I", header, 0x28)
        rest = f.read(count * 12)
    formats = [struct.unpack_from("<HH", rest, count * 4 + i * 4) for i in range(count)]
    ids = struct.unpack_from(f"<{count}I", rest, count * 8)
    return [AudioTrack(i, track_id, rate, bool(flags & 0x2000))
            for i, ((rate, flags), track_id) in enumerate(zip(formats, ids))]


def decode_track(path, track):
    import av
    import numpy as np

    chunks = []
    with av.open(str(path)) as container:
        stream = container.streams.audio[track.index]
        resampler = av.AudioResampler(format="flt", layout="mono", rate=SAMPLE_RATE)
        for packet in container.demux(stream):
            for frame in packet.decode():
                chunks.extend(f.to_ndarray().reshape(-1) for f in resampler.resample(frame))
        chunks.extend(f.to_ndarray().reshape(-1) for f in resampler.resample(None))
    return np.concatenate(chunks).astype(np.float32) if chunks else np.zeros(0, np.float32)


def rms_db(audio):
    import numpy as np

    if audio.size == 0:
        return -math.inf
    return 20 * math.log10(float(np.sqrt(np.mean(np.square(audio, dtype=np.float64)))) + 1e-12)


def load_dialogue(path):
    """Returns (audio, track, level in dBFS) for the movie's English speech."""
    tracks = read_bink_tracks(path)
    by_id = {t.track_id: t for t in tracks}
    wanted = [1, 0] if len(tracks) == 8 else [0]
    for track_id in wanted:
        track = by_id.get(track_id)
        if track is None:
            continue
        audio = decode_track(path, track)
        level = rms_db(audio)
        if level > SILENT_DB or track_id == wanted[-1]:
            return audio, track, level
        print(f"  track id {track_id} is near-silent ({level:.1f} dBFS), falling back")
    raise ValueError(f"{path.name}: no English track among ids {[t.track_id for t in tracks]}")


# --- Text ---------------------------------------------------------------------

def to_latin1(text):
    """Maps text onto U+0020-U+00FF, the range the game font covers."""
    text = text.translate(LATIN1_MAP)
    out = []
    for ch in text:
        if 0x20 <= ord(ch) <= 0x7E or 0xA0 <= ord(ch) <= 0xFF:
            out.append(ch)
        elif ch.isspace():
            out.append(" ")
        else:
            # Keep the base letter of anything outside Latin-1, e.g. "c" for U+010D.
            base = "".join(c for c in unicodedata.normalize("NFKD", ch) if 0x20 <= ord(c) <= 0x7E)
            if not base:
                print(f"    dropped character U+{ord(ch):04X}")
            out.append(base)
    return "".join(out)


def strip_non_speech(texts):
    """Removes [...], (...) and music symbols from a segment's word texts."""
    depth = 0
    cleaned = []
    for text in texts:
        kept = []
        for ch in text:
            if ch in "[(":
                depth += 1
            elif ch in "])":
                depth = max(depth - 1, 0)
            elif depth == 0 and ch not in MUSIC_SYMBOLS:
                kept.append(ch)
        cleaned.append("".join(kept).strip())
    return cleaned


def fix_text(text):
    for pattern, fix in CORRECTIONS:
        text = pattern.sub(fix, text)
    return re.sub(r"\s+", " ", text).strip()


def ends_sentence(text):
    word = text.rstrip("\"')").lower()
    # "B.J. Sykes", "the J.B.A. So...": initials are left to the pause test.
    if word in NOT_SENTENCE_END or re.fullmatch(r"([a-z]\.){2,}", word):
        return False
    return word.endswith((".", "?", "!"))


def break_cost(top, bottom):
    """Prefers breaks after punctuation or before a weak word, then even lines; none inside a name."""
    cost = max(len(top), len(bottom)) / 2
    last = top.rsplit(" ", 1)[-1]
    first = bottom.split(" ", 1)[0]
    if top[-1] in ",;:.?!":
        cost -= 6
    elif last.lower() in WEAK_LINE_ENDS:
        cost += 8
    elif last[0].isupper() and first[0].isupper():
        cost += 8  # e.g. "New York / City"
    if first.lower() in CONJUNCTIONS | PREPOSITIONS:
        cost -= 4  # "... much help / to look like hell"
    return cost


def wrap(text):
    """Balanced one- or two-line layout of text, or None if it does not fit."""
    if len(text) <= MAX_LINE:
        return [text]
    best = None
    for i, ch in enumerate(text):
        if ch != " ":
            continue
        top, bottom = text[:i], text[i + 1:]
        if len(top) > MAX_LINE or len(bottom) > MAX_LINE:
            continue
        cost = break_cost(top, bottom)
        if best is None or cost < best[0]:
            best = (cost, [top, bottom])
    return best[1] if best else None


# --- Cues ---------------------------------------------------------------------

@dataclass
class Word:
    start: float
    end: float
    text: str


@dataclass
class Cue:
    parts: list  # lists of Words; a merged cue puts each part on its own line

    @property
    def start(self):
        return self.parts[0][0].start

    @property
    def last_end(self):
        return self.parts[-1][-1].end

    def lines(self):
        texts = [fix_text(" ".join(w.text for w in part)) for part in self.parts]
        if len(texts) == 1:
            return wrap(texts[0])
        if len(texts) == 2 and all(len(t) <= MAX_LINE for t in texts):
            return texts
        return None


def fits(cue):
    return cue.lines() is not None and cue.last_end - cue.start <= MAX_DURATION


def cue_break_cost(prev, nxt):
    """Cost of ending a cue on word prev, with nxt starting the next one."""
    if prev.text.endswith((",", ";", ":")):
        cost = 0
    elif prev.text.lower() in WEAK_LINE_ENDS or (prev.text[0].isupper() and nxt.text[0].isupper()):
        cost = 30
    else:
        cost = 10
    if nxt.text.lower() in CONJUNCTIONS:
        cost -= 8
    elif nxt.text.lower() in PREPOSITIONS:
        cost -= 5
    if nxt.start - prev.end > 0.25:
        cost -= 5
    return cost


def split_sentence(words):
    """Cuts a sentence into as few cues as fit, at the best break points, without stubs."""
    best = [0.0] + [math.inf] * len(words)  # best[i]: cost of cutting words[:i]
    cut = [0] * (len(words) + 1)
    for i in range(1, len(words) + 1):
        for j in range(i - 1, -1, -1):
            chunk = Cue([words[j:i]])
            if j < i - 1 and not fits(chunk):
                break
            chars = len(" ".join(w.text for w in words[j:i]))
            cost = best[j] + 100 + max(0, 30 - chars)
            if j:
                cost += cue_break_cost(words[j - 1], words[j])
            if i < len(words) and words[i].start - words[j].start < MIN_DURATION + MIN_GAP:
                cost += 60  # could not stay up for MIN_DURATION

            if cost < best[i]:
                best[i], cut[i] = cost, j
    cues = []
    i = len(words)
    while i:
        cues.insert(0, Cue([words[cut[i]:i]]))
        i = cut[i]
    return cues


def group_words(words):
    """Splits words into sentences at sentence ends and pauses, then those into cues."""
    cues = []
    run = []
    for w in words:
        if run and (w.start - run[-1].end > PAUSE_SPLIT or ends_sentence(run[-1].text)):
            cues += split_sentence(run)
            run = []
        run.append(w)
    if run:
        cues += split_sentence(run)
    return cues


def merge_short(cues):
    """Joins a cue that cannot last MIN_DURATION before the next one starts to a neighbour.

    One sentence per line with the next cue, else with the previous one, else
    wrapped together with the next.
    """
    out = []
    i = 0
    while i < len(cues):
        cue = cues[i]
        nxt = cues[i + 1] if i + 1 < len(cues) else None
        if nxt and len(cue.parts) == 1 and cue.start + MIN_DURATION > nxt.start - MIN_GAP:
            prev = out[-1] if out and len(out[-1].parts) == 1 else None
            if len(nxt.parts) == 1 and fits(merged := Cue(cue.parts + nxt.parts)):
                out.append(merged)
                i += 2
                continue
            if prev and fits(merged := Cue(prev.parts + cue.parts)):
                out[-1] = merged
                i += 1
                continue
            if len(nxt.parts) == 1 and fits(merged := Cue([cue.parts[0] + nxt.parts[0]])):
                out.append(merged)
                i += 2
                continue
        out.append(cue)
        i += 1
    return out


def timed_cues(cues):
    result = []
    for i, cue in enumerate(cues):
        start = cue.start
        end = max(cue.last_end + TAIL, start + MIN_DURATION)
        end = min(end, start + MAX_DURATION)
        if i + 1 < len(cues):
            end = min(end, cues[i + 1].start - MIN_GAP)
        lines = cue.lines() or [fix_text(" ".join(w.text for p in cue.parts for w in p))]
        end = max(end, start + 0.1)
        if end - start < MIN_DURATION:
            print(f"  short cue {start:6.2f}-{end:6.2f}: {' / '.join(lines)}")
        result.append((start, end, lines))
    return result


def srt_time(t):
    ms = max(int(round(t * 1000)), 0)
    return f"{ms // 3600000:02}:{ms // 60000 % 60:02}:{ms // 1000 % 60:02},{ms % 1000:03}"


def write_srt(path, cues):
    blocks = []
    for n, (start, end, lines) in enumerate(cues, 1):
        blocks.append(f"{n}\n{srt_time(start)} --> {srt_time(end)}\n" + "\n".join(lines) + "\n")
    path.write_text("\n".join(blocks), encoding="utf-8", newline="\n")


# --- Transcription ------------------------------------------------------------

def prompt_echo(text):
    """True when Whisper parrots the initial prompt's name list back."""
    rest = text.lower()
    hits = 0
    for term in (t.strip(" .").lower() for t in INITIAL_PROMPT.split(",")):
        if term in rest:
            hits += 1
            rest = rest.replace(term, "")
    left, total = (len(re.sub(r"[^a-z]", "", s)) for s in (rest, text.lower()))
    return hits >= 3 and left < 0.3 * total


def snap_onsets(words, audio):
    """Moves word starts forward to where their sound begins.

    Whisper stretches a word back over the pause before it, or places it wholly
    in that pause, which shows cues up to half a second early. This works on the
    clean centre channel; over music every frame is loud and nothing moves.
    """
    import numpy as np

    hop = SAMPLE_RATE // 100
    frames = audio[:len(audio) // hop * hop].reshape(-1, hop).astype(np.float64)
    loud = 10 * np.log10(np.mean(np.square(frames), axis=1) + 1e-12) > ONSET_DB
    for i, w in enumerate(words):
        first = int(w.start * 100)
        hits = np.flatnonzero(loud[first:int((w.end + 0.15) * 100)])
        if not hits.size or (first + hits[0]) / 100 <= w.start:
            continue
        w.start = (first + hits[0]) / 100
        w.end = max(w.end, w.start + 0.05)
        if i + 1 < len(words):
            nxt = words[i + 1]
            nxt.start = max(nxt.start, w.end)
            nxt.end = max(nxt.end, nxt.start)


def bad_seams(segments):
    """Counts the marks of a window that starts mid-sentence.

    Those are segments of 8+ words without punctuation, and lowercase text right
    after a sentence end, such as "anymore." followed by "more, so he...".
    """
    count = 0
    for prev, seg in zip([None] + segments[:-1], segments):
        text = seg.text.strip()
        if len(seg.words or []) >= 8 and not re.search(r"[.,?!;:]", text):
            count += 1
        elif prev and text[:1].islower() and prev.text.strip().endswith((".", "?", "!")):
            count += 1
    return count


def run_whisper(model, audio):
    import numpy as np

    # Trailing silence: without it, a last window holding almost no audio can make
    # faster-whisper's word alignment fail with an IndexError.
    padded = np.concatenate([audio, np.zeros(SAMPLE_RATE, np.float32)])
    options = dict(language="en", vad_filter=True, word_timestamps=True, beam_size=5,
                   initial_prompt=INITIAL_PROMPT)
    segments = list(model.transcribe(padded, condition_on_previous_text=False, **options)[0])
    if bad_seams(segments):
        # A window that starts mid-sentence can lose its punctuation, and so its
        # sentence splits, or repeat the end of a word; carrying the previous text
        # over fixes that.
        retry = list(model.transcribe(padded, condition_on_previous_text=True, **options)[0])
        if bad_seams(retry) < bad_seams(segments):
            print("  bad window seam, using condition_on_previous_text=True")
            return retry
    return segments


def transcribe(model, audio):
    """Returns (kept words, dropped segment count)."""
    duration = len(audio) / SAMPLE_RATE
    segments = run_whisper(model, audio)
    words = []
    dropped = 0
    for seg in segments:
        span = f"{seg.start:6.2f}-{seg.end:6.2f}"
        seg_words = seg.words or []
        texts = [to_latin1(t).strip() for t in strip_non_speech([w.word for w in seg_words])]
        text = fix_text(" ".join(t for t in texts if t))
        word_prob = sum(w.probability for w in seg_words) / max(len(seg_words), 1)
        # faster-whisper scores no_speech_prob and avg_logprob once per 30 s window,
        # so music at the start of a window flags every line in it; the segment's own
        # word probabilities decide.
        reason = None
        if seg.start >= duration - 0.1:
            reason = "after the audio ends"
        elif seg.avg_logprob < AVG_LOGPROB_MIN:
            reason = f"avg_logprob {seg.avg_logprob:.2f}"
        elif seg.no_speech_prob > NO_SPEECH_MAX and word_prob < WORD_PROB_MIN:
            reason = f"no_speech_prob {seg.no_speech_prob:.2f}, word probability {word_prob:.2f}"
        elif not re.search(r"[A-Za-z0-9]", text):
            reason = "no speech text"
        elif FILLER.search(text):
            reason = "filler"
        elif prompt_echo(text):
            reason = "prompt echo"
        if reason:
            dropped += 1
            print(f"  drop {span} ({reason}): {seg.text.strip()!r}")
            continue
        print(f"  keep {span} (no_speech {seg.no_speech_prob:.2f}, word p {word_prob:.2f}): {text}")
        unsure = [f"{w.word.strip()} ({w.probability:.2f})" for w in seg_words if w.probability < 0.3]
        if unsure:
            print("    unsure: " + ", ".join(unsure))
        for n, (w, t) in enumerate(zip(seg_words, texts)):
            if not t:
                continue
            # Punctuation left after a removed bracket, or a token such as ",000" in "2,000".
            if words and (not re.search(r"[A-Za-z0-9]", t) or (n and not w.word.startswith(" "))):
                words[-1].text += t
                words[-1].end = max(words[-1].end, w.end)
                continue
            start = max(w.start, words[-1].end if words else 0.0)
            words.append(Word(start, max(w.end, start), t))
    snap_onsets(words, audio)
    return words, dropped


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--assets", type=Path, default=REPO / "assets",
                        help="game assets folder (holding Default/DataXb/videos)")
    parser.add_argument("--movies", nargs="+",
                        help="movie names without .bik (default: all but fv-*, logos, back_menu)")
    parser.add_argument("--out", type=Path, default=REPO / "subtitles", help="SRT output folder")
    parser.add_argument("--model", default="large-v3", help="faster-whisper model")
    parser.add_argument("--device", default="cuda", help="cuda or cpu")
    parser.add_argument("--force", action="store_true",
                        help="overwrite existing SRT files, which may hold hand fixes")
    args = parser.parse_args()

    videos = args.assets / VIDEOS_SUBDIR
    if args.movies:
        movies = [videos / (m if m.lower().endswith(".bik") else m + ".bik") for m in args.movies]
    else:
        movies = [p for p in sorted(videos.glob("*.bik")) if not SKIPPED_MOVIES.match(p.stem)]
    missing = [p for p in movies if not p.is_file()]
    if missing:
        sys.exit("missing movies: " + ", ".join(str(p) for p in missing))
    if not args.force:
        for p in [p for p in movies if (args.out / (p.stem + ".srt")).exists()]:
            print(f"{p.stem}: {p.stem}.srt exists, skipping (use --force to overwrite)")
            movies.remove(p)
    if not movies:
        return

    add_cuda_dll_dirs()
    os.environ.setdefault("HF_HUB_DISABLE_SYMLINKS_WARNING", "1")
    from faster_whisper import WhisperModel

    compute_type = "float16" if args.device == "cuda" else "int8"
    model = WhisperModel(args.model, device=args.device, compute_type=compute_type)
    args.out.mkdir(parents=True, exist_ok=True)

    summary = []
    for path in movies:
        print(f"{path.stem}:")
        audio, track, level = load_dialogue(path)
        channels = "stereo" if track.stereo else "mono"
        track_desc = f"id {track.track_id} ({channels} {track.rate} Hz, {level:.1f} dBFS)"
        print(f"  track {track_desc}, {len(audio) / SAMPLE_RATE:.1f} s")
        words, dropped = transcribe(model, audio)
        cues = timed_cues(merge_short(group_words(words)))
        if cues:
            write_srt(args.out / (path.stem + ".srt"), cues)
        else:
            print("  no speech, no SRT written")
        summary.append((path.stem, track_desc, len(cues), dropped))

    print("\nsummary:")
    for name, track_desc, cue_count, dropped in summary:
        cues = f"{cue_count} cues" if cue_count else "no speech"
        print(f"  {name}: track {track_desc}, {cues}, {dropped} segments dropped")


if __name__ == "__main__":
    main()
