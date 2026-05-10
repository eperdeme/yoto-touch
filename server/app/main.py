from contextlib import asynccontextmanager

from fastapi import FastAPI
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from httpx import HTTPStatusError

from .auth import AuthError
from .routes import router
from .yoto import yoto


@asynccontextmanager
async def lifespan(app: FastAPI):
    yield
    await yoto.aclose()


app = FastAPI(title="yoto-touch server", version="0.1.0", lifespan=lifespan)
app.include_router(router)


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
