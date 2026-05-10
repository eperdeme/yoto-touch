"""OAuth2 device authorization grant against Yoto.

Flow:
  1. POST /oauth/device/code        → user_code, device_code, verification_uri
  2. user signs in at the URL on their phone
  3. poll /oauth/token until tokens come back
  4. persist refresh_token; refresh access_token transparently on demand
"""
from __future__ import annotations

import asyncio
import json
import time
from dataclasses import dataclass
from typing import Any

import httpx

from .config import settings


@dataclass
class Tokens:
    access_token: str
    refresh_token: str
    expires_at: float  # epoch seconds
    token_type: str = "Bearer"

    def to_dict(self) -> dict[str, Any]:
        return self.__dict__

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "Tokens":
        return cls(**d)

    @property
    def expired(self) -> bool:
        return time.time() >= self.expires_at - 30  # 30s safety margin


class AuthError(RuntimeError):
    pass


class AuthManager:
    def __init__(self) -> None:
        self._tokens: Tokens | None = self._load()
        self._lock = asyncio.Lock()
        self._pending: dict[str, Any] | None = None  # active device-flow state

    # ---- persistence ----
    def _load(self) -> Tokens | None:
        p = settings.tokens_file
        if not p.exists():
            return None
        try:
            return Tokens.from_dict(json.loads(p.read_text()))
        except Exception:
            return None

    def _save(self) -> None:
        if self._tokens is None:
            return
        settings.tokens_file.write_text(json.dumps(self._tokens.to_dict(), indent=2))

    # ---- device flow ----
    async def start_device_flow(self) -> dict[str, Any]:
        async with httpx.AsyncClient(timeout=20) as c:
            r = await c.post(
                f"{settings.yoto_auth_base}/oauth/device/code",
                data={
                    "client_id": settings.yoto_client_id,
                    "scope": settings.yoto_scopes,
                    "audience": settings.yoto_audience,
                },
            )
            r.raise_for_status()
            data = r.json()
        self._pending = {
            "device_code": data["device_code"],
            "interval": data.get("interval", 5),
            "expires_at": time.time() + data.get("expires_in", 600),
        }
        return {
            "user_code": data["user_code"],
            "verification_uri": data.get("verification_uri"),
            "verification_uri_complete": data.get("verification_uri_complete"),
            "expires_in": data.get("expires_in", 600),
        }

    async def poll_device_flow(self) -> dict[str, Any]:
        if not self._pending:
            raise AuthError("no device flow in progress; call /auth/start first")
        if time.time() > self._pending["expires_at"]:
            self._pending = None
            raise AuthError("device code expired; call /auth/start again")
        async with httpx.AsyncClient(timeout=20) as c:
            r = await c.post(
                f"{settings.yoto_auth_base}/oauth/token",
                data={
                    "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
                    "device_code": self._pending["device_code"],
                    "client_id": settings.yoto_client_id,
                },
            )
        if r.status_code == 200:
            self._store_token_response(r.json())
            self._pending = None
            return {"status": "authorized"}
        err = r.json().get("error", "unknown")
        if err in ("authorization_pending", "slow_down"):
            return {"status": "pending", "error": err}
        self._pending = None
        raise AuthError(f"device flow failed: {err}")

    # ---- token use ----
    async def access_token(self) -> str:
        async with self._lock:
            if self._tokens is None:
                raise AuthError("not signed in; visit /auth/start")
            if self._tokens.expired:
                await self._refresh()
            return self._tokens.access_token

    async def _refresh(self) -> None:
        assert self._tokens is not None
        async with httpx.AsyncClient(timeout=20) as c:
            r = await c.post(
                f"{settings.yoto_auth_base}/oauth/token",
                data={
                    "grant_type": "refresh_token",
                    "client_id": settings.yoto_client_id,
                    "refresh_token": self._tokens.refresh_token,
                },
            )
        if r.status_code != 200:
            raise AuthError(f"refresh failed: {r.status_code} {r.text}")
        self._store_token_response(r.json())

    def _store_token_response(self, data: dict[str, Any]) -> None:
        self._tokens = Tokens(
            access_token=data["access_token"],
            # Yoto rotates refresh tokens; fall back to existing one if absent.
            refresh_token=data.get("refresh_token")
            or (self._tokens.refresh_token if self._tokens else ""),
            expires_at=time.time() + int(data.get("expires_in", 3600)),
            token_type=data.get("token_type", "Bearer"),
        )
        self._save()

    @property
    def signed_in(self) -> bool:
        return self._tokens is not None


auth = AuthManager()
