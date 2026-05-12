"""HTTP routes — the API the firmware (or any client) talks to."""
from __future__ import annotations

import logging
from typing import Any

from fastapi import APIRouter, HTTPException, Query
from fastapi.responses import FileResponse, JSONResponse
import httpx

from .auth import auth, AuthError
from .cache import favourites, folders, get_thumb_path, library, prewarm_thumbs, derive_series
from .config import settings
from .yoto import yoto

router = APIRouter()
log = logging.getLogger("yoto.routes")


def _safe(action: str, **fields: Any) -> dict[str, Any]:
    """Log a would-be device command and return a stub response."""
    log.warning("[safe-mode] suppressed %s %s", action, fields)
    return {"ok": True, "safe_mode": True, "action": action, **fields}


# ---------- auth ----------

@router.get("/auth/start")
async def auth_start():
    """Kick off the device flow. Show user_code + URL to the user once."""
    return await auth.start_device_flow()


@router.get("/auth/poll")
async def auth_poll():
    try:
        return await auth.poll_device_flow()
    except AuthError as e:
        raise HTTPException(400, str(e))


@router.get("/auth/status")
async def auth_status():
    return {"signed_in": auth.signed_in, "safe_mode": settings.safe_mode}


# ---------- library ----------

@router.get("/cards")
async def list_cards(
    page: int = Query(0, ge=0),
    size: int = Query(24, ge=1, le=200),
    favourites_only: bool = False,
    folder: str | None = None,
    author: str | None = None,
    series: str | None = None,
    sort: str = Query("title"),
    refresh: bool = False,
):
    cards = await library.cards(force=refresh)
    if favourites_only:
        favs = set(favourites.list())
        cards = [c for c in cards if c["cardId"] in favs]
    if folder:
        ids = set(folders.cards_in(folder))
        cards = [c for c in cards if c["cardId"] in ids]
    if author:
        cards = [c for c in cards if (c.get("author") or "") == author]
    if series:
        ids = set(derive_series(await library.cards()).get(series, []))
        cards = [c for c in cards if c["cardId"] in ids]
    if sort == "author":
        cards = sorted(cards, key=lambda c: ((c.get("author") or "~").lower(), (c.get("title") or "").lower()))
    elif sort == "title_desc":
        cards = sorted(cards, key=lambda c: (c.get("title") or "").lower(), reverse=True)
    elif sort == "duration":
        cards = sorted(cards, key=lambda c: c.get("duration") or 0, reverse=True)
    else:  # title (default)
        cards = sorted(cards, key=lambda c: (c.get("title") or "").lower())
    total = len(cards)
    start = page * size
    page_items = [_strip(c) for c in cards[start : start + size]]
    return {"total": total, "page": page, "size": size, "cards": page_items}


@router.get("/authors")
async def list_authors():
    """Distinct authors with card counts, most common first."""
    from collections import Counter
    cards = await library.cards()
    cnt: Counter[str] = Counter((c.get("author") or "(Unknown)") for c in cards)
    items = [{"name": n, "count": k} for n, k in cnt.most_common()]
    return {"authors": items, "total": len(cards)}


@router.get("/series")
async def list_series(expand: bool = False):
    """Auto-detected series (common title prefix per author), most cards first.

    With ?expand=true, each series includes its card list so the firmware can
    render a series-browser in a single round-trip.
    """
    cards = await library.cards()
    s = derive_series(cards)
    by_id = {c["cardId"]: c for c in cards}
    items: list[dict[str, Any]] = []
    for name, ids in s.items():
        entry: dict[str, Any] = {"name": name, "count": len(ids)}
        if expand:
            entry["cards"] = [
                {
                    "cardId": cid,
                    "title": by_id[cid]["title"],
                    "sequence_number": by_id[cid].get("sequence_number"),
                }
                for cid in ids
                if cid in by_id
            ]
            # Preserve metadata-driven order (sequence_number) when present;
            # fall back to alphabetical for heuristic series.
            entry["cards"].sort(key=lambda c: (
                c.get("sequence_number") if c.get("sequence_number") is not None else 1_000_000,
                (c["title"] or "").lower(),
            ))
        items.append(entry)
    items.sort(key=lambda x: (-x["count"], x["name"].lower()))
    return {"series": items, "total": len(cards)}


@router.post("/refresh")
async def refresh_library():
    """Force re-fetch from Yoto API and warm any new thumbnails."""
    import asyncio
    cards = await library.cards(force=True)
    asyncio.create_task(prewarm_thumbs())
    return {"ok": True, "total": len(cards)}


def _strip(card: dict[str, Any]) -> dict[str, Any]:
    return {
        "cardId": card["cardId"],
        "title": card["title"],
        "author": card.get("author"),
        "category": card.get("category"),
        "duration": card.get("duration"),
        "series": card.get("series"),
        "sequence_number": card.get("sequence_number"),
        "thumb": f"/thumb/{card['cardId']}",
        "is_fav": favourites.is_fav(card["cardId"]),
    }


def _detail_extras(card: dict[str, Any], full: dict[str, Any]) -> dict[str, Any]:
    """Extract rich detail fields from a Yoto card payload."""
    raw = card.get("raw") or {}
    meta = raw.get("metadata") or full.get("metadata") or {}
    content = full.get("content") or raw.get("content") or {}
    chapters = content.get("chapters") or []
    media = (content.get("media") or {}) if isinstance(content, dict) else {}
    track_count = sum(len(ch.get("tracks") or []) for ch in chapters)
    return {
        "description": (meta.get("description") or "").strip() or None,
        "chapter_count": len(chapters),
        "track_count": track_count,
        "genre": meta.get("genre") or meta.get("genres"),
        "age_min": meta.get("minAge") or meta.get("ageMin"),
        "age_max": meta.get("maxAge") or meta.get("ageMax"),
        "languages": meta.get("languages"),
        "narrator": meta.get("narrator") or meta.get("readBy"),
        "publisher": meta.get("publisher"),
        "file_size": media.get("fileSize") if isinstance(media, dict) else None,
    }


@router.get("/cards/{card_id}")
async def card_detail(card_id: str):
    await library.cards()
    summary = library.find(card_id)
    if not summary:
        raise HTTPException(404, "card not found")
    full = await yoto.get_card(card_id)
    chapters = (full.get("content") or {}).get("chapters", [])
    return {
        **_strip(summary),
        **_detail_extras(summary, full),
        "chapters": [
            {
                "key": ch.get("key"),
                "title": ch.get("title") or ch.get("name"),
                "duration": ch.get("duration"),
                "track_count": len(ch.get("tracks") or []),
            }
            for ch in chapters
        ],
    }


@router.get("/thumb/{card_id}")
async def thumb(card_id: str):
    await library.cards()
    path = await get_thumb_path(card_id, "jpg")
    if not path:
        raise HTTPException(404, "no thumbnail")
    return FileResponse(path, media_type="image/jpeg")


@router.get("/thumb565/{card_id}")
async def thumb565(card_id: str):
    await library.cards()
    path = await get_thumb_path(card_id, "565")
    if not path:
        raise HTTPException(404, "no thumbnail")
    return FileResponse(path, media_type="application/octet-stream")


@router.get("/page565")
async def page565(
    page: int = Query(0, ge=0),
    size: int = Query(8, ge=1, le=16),
    author: str | None = None,
    series: str | None = None,
    sort: str = Query("title"),
):
    """Return all RGB565 thumbnails for a page concatenated as one binary blob.

    Output: `size × THUMB_SIZE²×2` bytes. Missing/failing thumbnails are filled
    with 0xFF so the firmware doesn't have to do bounds-checking. Card order
    matches `/cards?page=...&size=...&author=...&sort=...`.
    """
    from fastapi.responses import Response
    from .cache import THUMB_SIZE
    THUMB_BYTES = THUMB_SIZE * THUMB_SIZE * 2

    cards = await library.cards()
    if author:
        cards = [c for c in cards if (c.get("author") or "") == author]
    if series:
        ids = set(derive_series(await library.cards()).get(series, []))
        cards = [c for c in cards if c["cardId"] in ids]
    if sort == "author":
        cards = sorted(cards, key=lambda c: ((c.get("author") or "~").lower(), (c.get("title") or "").lower()))
    elif sort == "title_desc":
        cards = sorted(cards, key=lambda c: (c.get("title") or "").lower(), reverse=True)
    elif sort == "duration":
        cards = sorted(cards, key=lambda c: c.get("duration") or 0, reverse=True)
    else:
        cards = sorted(cards, key=lambda c: (c.get("title") or "").lower())
    start = page * size
    page_cards = cards[start : start + size]

    out = bytearray()
    for c in page_cards:
        path = await get_thumb_path(c["cardId"], "565")
        if path:
            try:
                blob = open(path, "rb").read()
                if len(blob) == THUMB_BYTES:
                    out.extend(blob)
                    continue
            except Exception:
                pass
        out.extend(b"\xff" * THUMB_BYTES)

    return Response(content=bytes(out), media_type="application/octet-stream")


# ---------- favourites & folders ----------

@router.get("/favourites")
async def get_favs():
    return {"cardIds": favourites.list()}


@router.post("/favourites/{card_id}")
async def toggle_fav(card_id: str):
    return {"is_fav": favourites.toggle(card_id)}


@router.get("/folders")
async def get_folders():
    return folders.all()


@router.post("/folders/{name}/{card_id}")
async def add_to_folder(name: str, card_id: str):
    folders.add(name, card_id)
    return {"ok": True}


@router.delete("/folders/{name}/{card_id}")
async def remove_from_folder(name: str, card_id: str):
    folders.remove(name, card_id)
    return {"ok": True}


# ---------- devices & playback ----------

async def _resolve_device() -> str:
    if settings.default_device_id:
        return settings.default_device_id
    devices = await yoto.list_devices()
    if not devices:
        raise HTTPException(404, "no Yoto players found on this account")
    return devices[0]["deviceId"]


@router.get("/devices")
async def list_devices():
    return {"devices": await yoto.list_devices()}


@router.get("/now-playing")
async def now_playing(device_id: str | None = None):
    did = device_id or await _resolve_device()
    try:
        data = await yoto.device_status(did)
        data["available"] = True
        return data
    except httpx.HTTPStatusError as e:
        if e.response.status_code == 403:
            return {"deviceId": did, "available": False, "reason": "scope_missing"}
        raise


@router.post("/play/{card_id}")
async def play(card_id: str, device_id: str | None = None):
    did = device_id or await _resolve_device()
    if settings.safe_mode:
        return _safe("play", deviceId=did, cardId=card_id)
    await yoto.play_card(did, card_id)
    return {"ok": True, "deviceId": did, "cardId": card_id}


@router.post("/pause")
async def pause(device_id: str | None = None):
    did = device_id or await _resolve_device()
    if settings.safe_mode:
        return _safe("pause", deviceId=did)
    await yoto.pause(did)
    return {"ok": True}


@router.post("/resume")
async def resume(device_id: str | None = None):
    did = device_id or await _resolve_device()
    if settings.safe_mode:
        return _safe("resume", deviceId=did)
    await yoto.resume(did)
    return {"ok": True}


@router.post("/stop")
async def stop(device_id: str | None = None):
    did = device_id or await _resolve_device()
    if settings.safe_mode:
        return _safe("stop", deviceId=did)
    await yoto.stop(did)
    return {"ok": True}


@router.post("/volume/{volume}")
async def volume(volume: int, device_id: str | None = None):
    did = device_id or await _resolve_device()
    if settings.safe_mode:
        return _safe("volume", deviceId=did, volume=volume)
    await yoto.set_volume(did, volume)
    return {"ok": True}


@router.post("/sleep-timer/{seconds}")
async def sleep_timer(seconds: int, device_id: str | None = None):
    did = device_id or await _resolve_device()
    if settings.safe_mode:
        return _safe("sleep-timer", deviceId=did, seconds=seconds)
    await yoto.sleep_timer(did, seconds)
    return {"ok": True}
