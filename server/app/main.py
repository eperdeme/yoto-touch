from contextlib import asynccontextmanager

import asyncio
import time

from fastapi import FastAPI, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from httpx import HTTPStatusError

from .auth import AuthError
from .cache import prewarm_thumbs
from .routes import router
from .yoto import yoto


@asynccontextmanager
async def lifespan(app: FastAPI):
    task = asyncio.create_task(prewarm_thumbs())
    try:
        yield
    finally:
        task.cancel()
        await yoto.aclose()


app = FastAPI(title="yoto-touch server", version="0.1.0", lifespan=lifespan)
app.include_router(router)


@app.middleware("http")
async def _timing(request: Request, call_next):
    t0 = time.perf_counter()
    response = await call_next(request)
    ms = (time.perf_counter() - t0) * 1000
    ts = time.strftime("%H:%M:%S")
    size = response.headers.get("content-length", "?")
    print(f"[{ts}] {ms:7.1f}ms {response.status_code} {request.method:5s} {request.url.path}{('?' + request.url.query) if request.url.query else ''}  {size}B")
    response.headers["X-Response-Time-ms"] = f"{ms:.1f}"
    return response


@app.exception_handler(AuthError)
async def _auth_err(_, exc: AuthError):
    return JSONResponse({"error": str(exc)}, status_code=401)


@app.exception_handler(HTTPStatusError)
async def _httpx_err(_, exc: HTTPStatusError):
    return JSONResponse(
        {"error": "upstream Yoto API error", "status": exc.response.status_code, "body": exc.response.text},
        status_code=502,
    )


@app.get("/")
async def root():
    return {"ok": True, "service": "yoto-touch"}


def run() -> None:
    """Entrypoint for `uv run yoto-touch-server` / `uvx`."""
    import uvicorn
    from .config import settings
    uvicorn.run("app.main:app", host=settings.host, port=settings.port, reload=False)
