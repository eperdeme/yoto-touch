"""HTTP routes — the API the firmware (or any client) talks to."""
from __future__ import annotations

import logging
from typing import Any

from fastapi import APIRouter, HTTPException, Query
from fastapi.responses import FileResponse, JSONResponse
import httpx

from .auth import auth, AuthError
from .cache import favourites, folders, get_thumb_path, library
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
    size: int = Query(24, ge=1, le=100),
    favourites_only: bool = False,
    folder: str | None = None,
    refresh: bool = False,
):
    cards = await library.cards(force=refresh)
    if favourites_only:
        favs = set(favourites.list())
        cards = [c for c in cards if c["cardId"] in favs]
    if folder:
        ids = set(folders.cards_in(folder))
        cards = [c for c in cards if c["cardId"] in ids]
    total = len(cards)
    start = page * size
    page_items = [_strip(c) for c in cards[start : start + size]]
    return {"total": total, "page": page, "size": size, "cards": page_items}


def _strip(card: dict[str, Any]) -> dict[str, Any]:
    return {
        "cardId": card["cardId"],
        "title": card["title"],
        "author": card.get("author"),
        "category": card.get("category"),
        "duration": card.get("duration"),
        "thumb": f"/thumb/{card['cardId']}",
        "is_fav": favourites.is_fav(card["cardId"]),
    }


@router.get("/cards/{card_id}")
async def card_detail(card_id: str):
    await library.cards()
    summary = library.find(card_id)
    if not summary:
        raise HTTPException(404, "card not found")
    full = await yoto.get_card(card_id)
    return {**_strip(summary), "chapters": full.get("content", {}).get("chapters", [])}


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
        return await yoto.device_status(did)
    except httpx.HTTPStatusError as e:
        # Yoto requires `family:device-status:view`, which isn't always granted
        # to public clients. Degrade gracefully so the UI keeps working.
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
