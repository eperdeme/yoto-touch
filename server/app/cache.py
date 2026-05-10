"""In-memory + on-disk library cache.

The Yoto library doesn't change often; we hit the API at most every
LIBRARY_TTL seconds. Thumbnails are downloaded once and resized to a
firmware-friendly size (200x200 JPEG ~10-15 KB), saved by cardId.
"""
from __future__ import annotations

import asyncio
import io
import json
import time
from typing import Any

import httpx
from PIL import Image

from .config import settings
from .yoto import yoto

LIBRARY_TTL = 300  # seconds
THUMB_SIZE = 160


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
        return {
            "cardId": card.get("cardId"),
            "title": card.get("title") or meta.get("title") or "Untitled",
            "author": meta.get("author"),
            "category": meta.get("category"),
            "duration": meta.get("duration"),
            "cover_url": cover,
            "raw": card,  # kept for /cards/{id}/full
        }

    def find(self, card_id: str) -> dict[str, Any] | None:
        for c in self._cards:
            if c["cardId"] == card_id:
                return c
        return None


library = LibraryCache()


# ---------- thumbnails ----------

_thumb_lock = asyncio.Lock()


async def get_thumb_path(card_id: str, fmt: str = "jpg") -> str | None:
    """Return path to a 200x200 thumbnail (JPEG or RGB565), downloading + resizing if needed."""
    if fmt == "565":
        out = settings.thumbs_dir / f"{card_id}.565"
    else:
        out = settings.thumbs_dir / f"{card_id}.jpg"

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
                data = bytearray()
                for y in range(THUMB_SIZE):
                    for x in range(THUMB_SIZE):
                        r, g, b = img.getpixel((x, y))
                        # RGB565 Little Endian: (G[2:0] | B[4:0]), (R[4:0] | G[5:3])
                        val = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                        data.append(val & 0xFF)
                        data.append((val >> 8) & 0xFF)
                out.write_bytes(data)
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
