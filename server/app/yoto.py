"""Thin wrapper around the Yoto REST API.

All endpoint paths verified against https://yoto.dev/api/ (May 2026).

Note: GET /content/mine returns only MYO (Make Your Own) cards. Commercial
Yoto cards bought from the shop are not currently exposed by a list endpoint;
they can still be played by ID via the card/start command if you know the
card ID (we keep a user-curated catalogue in data/extra_cards.json).
"""
from __future__ import annotations

from typing import Any

import httpx

from .auth import auth
from .config import settings


class YotoClient:
    def __init__(self) -> None:
        self._client: httpx.AsyncClient | None = None

    async def _http(self) -> httpx.AsyncClient:
        if self._client is None:
            self._client = httpx.AsyncClient(base_url=settings.yoto_api_base, timeout=20)
        return self._client

    async def _headers(self) -> dict[str, str]:
        return {"Authorization": f"Bearer {await auth.access_token()}"}

    async def aclose(self) -> None:
        if self._client is not None:
            await self._client.aclose()
            self._client = None

    # ---------- library ----------
    async def list_myo(self, show_deleted: bool = False) -> list[dict[str, Any]]:
        c = await self._http()
        r = await c.get(
            "/content/mine",
            headers=await self._headers(),
            params={"showdeleted": "true" if show_deleted else "false"},
        )
        r.raise_for_status()
        data = r.json()
        return data.get("cards", []) if isinstance(data, dict) else data

    async def get_card(self, card_id: str) -> dict[str, Any]:
        c = await self._http()
        r = await c.get(f"/content/{card_id}", headers=await self._headers())
        r.raise_for_status()
        data = r.json()
        # API wraps single card as {"card": {...}} on some endpoints; tolerate both.
        return data.get("card", data) if isinstance(data, dict) else data

    # ---------- devices ----------
    async def list_devices(self) -> list[dict[str, Any]]:
        c = await self._http()
        r = await c.get("/device-v2/devices/mine", headers=await self._headers())
        r.raise_for_status()
        data = r.json()
        return data.get("devices", []) if isinstance(data, dict) else data

    async def device_status(self, device_id: str) -> dict[str, Any]:
        c = await self._http()
        r = await c.get(f"/device-v2/{device_id}/status", headers=await self._headers())
        r.raise_for_status()
        return r.json()

    # ---------- playback (POST commands; bodies mirror MQTT payloads) ----------
    async def _command(self, device_id: str, resource: str, payload: dict | None = None) -> None:
        c = await self._http()
        r = await c.post(
            f"/device-v2/{device_id}/command/{resource}",
            headers=await self._headers(),
            json=payload or {},
        )
        r.raise_for_status()

    async def play_card(
        self,
        device_id: str,
        card_id: str,
        *,
        chapter_key: str | None = None,
        track_key: str | None = None,
        seconds_in: int = 0,
    ) -> None:
        payload: dict[str, Any] = {"uri": f"https://yoto.io/{card_id}", "secondsIn": seconds_in}
        if chapter_key:
            payload["chapterKey"] = chapter_key
        if track_key:
            payload["trackKey"] = track_key
        await self._command(device_id, "card/start", payload)

    async def pause(self, device_id: str) -> None:
        await self._command(device_id, "card/pause")

    async def resume(self, device_id: str) -> None:
        await self._command(device_id, "card/resume")

    async def stop(self, device_id: str) -> None:
        await self._command(device_id, "card/stop")

    async def set_volume(self, device_id: str, volume: int) -> None:
        await self._command(device_id, "volume/set", {"volume": max(0, min(100, int(volume)))})

    async def sleep_timer(self, device_id: str, seconds: int) -> None:
        await self._command(device_id, "sleep-timer/set", {"seconds": max(0, int(seconds))})

    async def request_status(self, device_id: str) -> None:
        """Ask the player to publish a fresh status report (over MQTT)."""
        await self._command(device_id, "status")


yoto = YotoClient()
