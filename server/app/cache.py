"""In-memory + on-disk library cache.

The Yoto library doesn't change often; we hit the API at most every
LIBRARY_TTL seconds. Thumbnails are downloaded once and resized to a
firmware-friendly size (200x200 JPEG ~10-15 KB), saved by cardId.
"""
from __future__ import annotations

import asyncio
import io
import json
import re
import time
from typing import Any

_NOTE_RE = re.compile(r"^\s*(.*?)(?:\s*#\s*(\d+))?\s*$")


def parse_series_note(note: str | None) -> tuple[str | None, int | None]:
    """Parse Yoto `metadata.note` like 'Cross Bones #1' into (series, seq)."""
    if not note:
        return None, None
    m = _NOTE_RE.match(note)
    if not m:
        return None, None
    name = (m.group(1) or "").strip()
    seq = m.group(2)
    return (name or None), (int(seq) if seq else None)

import httpx
from PIL import Image

from .config import settings
from .yoto import yoto

LIBRARY_TTL = 300  # seconds
THUMB_SIZE = 144


class LibraryCache:
    def __init__(self) -> None:
        self._cards: list[dict[str, Any]] = []
        self._fetched_at: float = 0
        self._lock = asyncio.Lock()

    async def cards(self, force: bool = False) -> list[dict[str, Any]]:
        async with self._lock:
            if force or not self._cards or (time.time() - self._fetched_at) > LIBRARY_TTL:
                await self._refresh()
            return list(self._cards)

    async def _refresh(self) -> None:
        myo = await yoto.list_myo()
        extra = self._load_extra()
        merged = {c["cardId"]: self._normalise(c) for c in myo if c.get("cardId")}
        for c in extra:
            if c.get("cardId") and c["cardId"] not in merged:
                merged[c["cardId"]] = self._normalise(c)
        self._cards = sorted(merged.values(), key=lambda c: (c.get("title") or "").lower())
        self._fetched_at = time.time()

    def _load_extra(self) -> list[dict[str, Any]]:
        p = settings.data_dir / "extra_cards.json"
        if not p.exists():
            return []
        try:
            return json.loads(p.read_text())
        except Exception:
            return []

    @staticmethod
    def _normalise(card: dict[str, Any]) -> dict[str, Any]:
        meta = card.get("metadata") or {}
        cover = (meta.get("cover") or {}).get("imageL") or (meta.get("cover") or {}).get("imageS")
        series, seq = parse_series_note(meta.get("note"))
        if series is None:
            series = (meta.get("series") or "").strip() or None
        if seq is None:
            raw_seq = meta.get("sequence_number") or card.get("book_number") or card.get("order")
            try:
                seq = int(raw_seq) if raw_seq is not None and str(raw_seq).strip() != "" else None
            except (TypeError, ValueError):
                seq = None
        return {
            "cardId": card.get("cardId"),
            "title": card.get("title") or meta.get("title") or "Untitled",
            "author": meta.get("author"),
            "category": meta.get("category"),
            "duration": meta.get("duration"),
            "cover_url": cover,
            "series": series,
            "sequence_number": seq,
            "raw": card,  # kept for /cards/{id}/full
        }

    def find(self, card_id: str) -> dict[str, Any] | None:
        for c in self._cards:
            if c["cardId"] == card_id:
                return c
        return None


library = LibraryCache()


# ---------- series detection ----------

_STOPWORDS = {"the", "a", "an", "and", "of", "to", "in", "on", "for", "by", "with"}


def _tokens(title: str) -> list[str]:
    out: list[str] = []
    word = []
    for ch in title:
        if ch.isalnum():
            word.append(ch.lower())
        else:
            if word:
                out.append("".join(word))
                word = []
    if word:
        out.append("".join(word))
    return out


def _series_key(prefix: list[str]) -> str:
    # Title-case for display; preserve original word boundaries.
    return " ".join(w.capitalize() for w in prefix)


def normalise_series_name(name: str) -> str:
    """Normalise a series name for comparison/grouping (per Yoto convention).

    Strip leading articles, drop non-alphanumerics, collapse whitespace, lowercase.
    """
    if not name:
        return ""
    s = name.strip().lower()
    for art in ("the ", "a ", "an "):
        if s.startswith(art):
            s = s[len(art):]
            break
    out = []
    prev_space = False
    for ch in s:
        if ch.isalnum():
            out.append(ch)
            prev_space = False
        elif ch.isspace() or ch in "-_":
            if not prev_space:
                out.append(" ")
                prev_space = True
    return "".join(out).strip()


def derive_series(cards: list[dict[str, Any]]) -> dict[str, list[str]]:
    """Group card IDs by series name.

    Prefers explicit metadata.series (Yoto API) when present; falls back to a
    title-prefix heuristic for cards whose metadata is unset. Heuristic groups
    are skipped if their normalised name collides with a real metadata series.
    """
    series: dict[str, list[str]] = {}
    claimed: set[str] = set()  # cardIds already placed via metadata
    norm_taken: set[str] = set()  # normalised names taken by metadata series

    # 1) Explicit metadata.series, sorted by sequence_number then title.
    by_meta: dict[str, list[dict[str, Any]]] = {}
    for c in cards:
        name = (c.get("series") or "").strip()
        if not name:
            continue
        by_meta.setdefault(name, []).append(c)
    for name, group in by_meta.items():
        group.sort(key=lambda c: (
            c.get("sequence_number") if c.get("sequence_number") is not None else 1_000_000,
            (c.get("title") or "").lower(),
        ))
        series[name] = [c["cardId"] for c in group if c.get("cardId")]
        claimed.update(series[name])
        norm_taken.add(normalise_series_name(name))

    # 2) Heuristic over remaining cards only.
    by_author: dict[str, list[dict[str, Any]]] = {}
    for c in cards:
        if c.get("cardId") in claimed:
            continue
        a = c.get("author") or ""
        by_author.setdefault(a, []).append(c)

    for author, group in by_author.items():
        if len(group) < 2:
            continue
        # Build (tokens, card) pairs with leading stopwords stripped.
        pairs: list[tuple[list[str], dict[str, Any]]] = []
        for c in group:
            toks = _tokens(c.get("title") or "")
            while toks and toks[0] in _STOPWORDS:
                toks.pop(0)
            if toks:
                pairs.append((toks, c))

        # Try prefix lengths from longest down; collect groups of size >= 2.
        used: set[str] = set()
        max_len = max((len(p[0]) for p in pairs), default=0)
        for plen in range(min(max_len, 4), 0, -1):
            buckets: dict[tuple[str, ...], list[str]] = {}
            for toks, c in pairs:
                if c["cardId"] in used or len(toks) < plen:
                    continue
                key = tuple(toks[:plen])
                buckets.setdefault(key, []).append(c["cardId"])
            for key, ids in buckets.items():
                if len(ids) < 2:
                    continue
                # Skip purely-stopword or single-letter prefixes.
                if all(len(w) <= 2 for w in key):
                    continue
                name = _series_key(list(key))
                if author and author.lower() not in name.lower():
                    name = f"{name} ({author})"
                # Avoid clobbering an existing better (longer) series with the same name,
                # or one already provided by real metadata.
                if name in series or normalise_series_name(name) in norm_taken:
                    continue
                series[name] = ids
                used.update(ids)
    return series


async def prewarm_thumbs(concurrency: int = 4) -> None:
    """Fetch & convert every card thumbnail to .jpg + .565 in the background.

    Idempotent: skips cards whose .565 file already exists.
    """
    cards = await library.cards()
    sem = asyncio.Semaphore(concurrency)
    pending = [
        c for c in cards
        if c.get("cardId") and not (settings.thumbs_dir / f"{c['cardId']}.{THUMB_SIZE}.565").exists()
    ]
    if not pending:
        print(f"[prewarm] all {len(cards)} thumbs already cached")
        return
    print(f"[prewarm] warming {len(pending)}/{len(cards)} thumbnails...")

    async def _one(cid: str) -> None:
        async with sem:
            await get_thumb_path(cid, "565")

    t0 = time.time()
    await asyncio.gather(*(_one(c["cardId"]) for c in pending), return_exceptions=True)
    print(f"[prewarm] done in {time.time() - t0:.1f}s")


# ---------- thumbnails ----------

_thumb_lock = asyncio.Lock()


async def get_thumb_path(card_id: str, fmt: str = "jpg") -> str | None:
    """Return path to a thumbnail (JPEG or raw RGB565), downloading + resizing if needed."""
    if fmt == "565":
        out = settings.thumbs_dir / f"{card_id}.{THUMB_SIZE}.565"
    else:
        out = settings.thumbs_dir / f"{card_id}.{THUMB_SIZE}.jpg"

    if out.exists():
        return str(out)

    if fmt == "565":
        # Ensure JPEG exists first, then convert
        jpg_path = await get_thumb_path(card_id, "jpg")
        if not jpg_path:
            return None
        async with _thumb_lock:
            if out.exists():
                return str(out)
            try:
                img = Image.open(jpg_path).convert("RGB")
                raw = img.tobytes()  # tightly-packed RGB bytes
                buf = bytearray(THUMB_SIZE * THUMB_SIZE * 2)
                for i in range(THUMB_SIZE * THUMB_SIZE):
                    r = raw[i * 3]
                    g = raw[i * 3 + 1]
                    b = raw[i * 3 + 2]
                    # RGB565 little-endian (low byte first)
                    val = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                    buf[i * 2] = val & 0xFF
                    buf[i * 2 + 1] = (val >> 8) & 0xFF
                out.write_bytes(buf)
                return str(out)
            except Exception:
                return None

    card = library.find(card_id)
    if not card or not card.get("cover_url"):
        return None
    async with _thumb_lock:
        if out.exists():
            return str(out)
        try:
            async with httpx.AsyncClient(timeout=15) as c:
                r = await c.get(card["cover_url"])
                r.raise_for_status()
            img = Image.open(io.BytesIO(r.content)).convert("RGB")
            img.thumbnail((THUMB_SIZE, THUMB_SIZE), Image.LANCZOS)
            canvas = Image.new("RGB", (THUMB_SIZE, THUMB_SIZE), (255, 255, 255))
            canvas.paste(img, ((THUMB_SIZE - img.width) // 2, (THUMB_SIZE - img.height) // 2))
            canvas.save(out, "JPEG", quality=82, optimize=True)
            return str(out)
        except Exception:
            return None


# ---------- favourites ----------

class Favourites:
    def __init__(self) -> None:
        self._ids: set[str] = set(self._load())

    def _load(self) -> list[str]:
        p = settings.favourites_file
        if not p.exists():
            return []
        try:
            return json.loads(p.read_text())
        except Exception:
            return []

    def _save(self) -> None:
        settings.favourites_file.write_text(json.dumps(sorted(self._ids), indent=2))

    def list(self) -> list[str]:
        return sorted(self._ids)

    def toggle(self, card_id: str) -> bool:
        if card_id in self._ids:
            self._ids.remove(card_id)
            self._save()
            return False
        self._ids.add(card_id)
        self._save()
        return True

    def is_fav(self, card_id: str) -> bool:
        return card_id in self._ids


favourites = Favourites()


# ---------- folders (kid-friendly categories) ----------

class Folders:
    """Editable JSON file mapping folder name -> list of cardIds.

    Example data/folders.json:
        { "Bedtime": ["abc123", ...], "Music": ["def456", ...] }
    """

    def __init__(self) -> None:
        self._data: dict[str, list[str]] = self._load()

    def _load(self) -> dict[str, list[str]]:
        p = settings.folders_file
        if not p.exists():
            return {}
        try:
            return json.loads(p.read_text())
        except Exception:
            return {}

    def _save(self) -> None:
        settings.folders_file.write_text(json.dumps(self._data, indent=2))

    def all(self) -> dict[str, list[str]]:
        return dict(self._data)

    def cards_in(self, name: str) -> list[str]:
        return list(self._data.get(name, []))

    def add(self, name: str, card_id: str) -> None:
        self._data.setdefault(name, [])
        if card_id not in self._data[name]:
            self._data[name].append(card_id)
            self._save()

    def remove(self, name: str, card_id: str) -> None:
        if name in self._data and card_id in self._data[name]:
            self._data[name].remove(card_id)
            self._save()


folders = Folders()
