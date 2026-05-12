#!/usr/bin/env python3
"""End-to-end smoke test for the running yoto-touch server.

Walks the full happy path:
    1. /auth/status          - signed in?
    2. /auth/start + /auth/poll - if not, walk device flow
    3. /devices              - list players
    4. /cards                - list library (first page)
    5. (interactive) play a chosen card, then /now-playing, then /stop

Run the server in another terminal first:
    cd server && uvicorn app.main:app --port 8010

Then:
    python tools/smoke_test.py
"""
from __future__ import annotations

import sys
import time
import urllib.parse
import urllib.request
import json

BASE = "http://localhost:8010"


def _req(method: str, path: str) -> dict:
    req = urllib.request.Request(BASE + path, method=method)
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read())


def get(p):  return _req("GET", p)
def post(p): return _req("POST", p)


def ensure_signed_in() -> None:
    if get("/auth/status").get("signed_in"):
        print("auth: already signed in ✓")
        return
    print("auth: starting device flow ...")
    start = get("/auth/start")
    print()
    print("  Open this URL on your phone or laptop:")
    print("    ", start.get("verification_uri_complete") or start["verification_uri"])
    print("  And enter this code if asked:", start["user_code"])
    print()
    print("  Polling every 5s ...")
    deadline = time.time() + start.get("expires_in", 600)
    while time.time() < deadline:
        time.sleep(5)
        try:
            r = get("/auth/poll")
        except urllib.error.HTTPError as e:
            print("  poll error:", e.read().decode())
            sys.exit(1)
        if r.get("status") == "authorized":
            print("auth: signed in ✓")
            return
        print("  ...", r.get("error", "pending"))
    print("auth: timed out")
    sys.exit(1)


def main() -> None:
    print(f"server: {BASE}")
    try:
        get("/")
    except Exception as e:
        print("ERROR: server not reachable:", e)
        sys.exit(1)

    ensure_signed_in()

    print("\ndevices:")
    devs = get("/devices").get("devices", [])
    if not devs:
        print("  none — make sure your player is on the same Yoto account.")
        sys.exit(1)
    for d in devs:
        print(f"  - {d.get('deviceId')}  {d.get('name')!r}  online={d.get('online')}")

    print("\ncards (page 0):")
    cards = get("/cards?page=0&size=10")
    print(f"  total={cards.get('total')}")
    for i, c in enumerate(cards.get("cards", [])):
        print(f"  [{i:2}] {c['cardId']}  {c['title']!r}")

    if not cards.get("cards"):
        print("\n(library is empty — likely no MYO cards on the account.")
        print(" Add a cardId to server/data/extra_cards.json to test playback,")
        print(" then re-run with `?refresh=true` on /cards.)")
        return

    pick = input("\nPlay which index? (blank to skip): ").strip()
    if not pick:
        return
    chosen = cards["cards"][int(pick)]
    print(f"playing {chosen['title']!r} ({chosen['cardId']}) ...")
    print(post(f"/play/{chosen['cardId']}"))

    time.sleep(3)
    print("\nnow-playing:")
    np = get("/now-playing")
    print(json.dumps(np, indent=2)[:800])

    if input("\nStop playback? [y/N]: ").strip().lower() == "y":
        print(post("/stop"))


if __name__ == "__main__":
    main()
