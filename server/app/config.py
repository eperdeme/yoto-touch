from pathlib import Path
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_file=".env", env_file_encoding="utf-8", extra="ignore")

    yoto_client_id: str
    yoto_auth_base: str = "https://login.yotoplay.com"
    yoto_api_base: str = "https://api.yotoplay.com"
    yoto_audience: str = "https://api.yotoplay.com"
    yoto_scopes: str = "openid profile offline_access"

    data_dir: Path = Path("./data")
    tokens_dir: Path = Path("./tokens")

    default_device_id: str = ""

    # When true, all device-mutating endpoints (play/pause/resume/stop/volume/
    # sleep-timer) become no-ops that just log + return {"ok": true, "safe_mode": true}.
    # Useful while developing the UI without disturbing whoever is actually
    # using the Yoto player.
    safe_mode: bool = False

    host: str = "0.0.0.0"
    port: int = 8010

    @property
    def tokens_file(self) -> Path:
        return self.tokens_dir / "tokens.json"

    @property
    def thumbs_dir(self) -> Path:
        return self.data_dir / "thumbs"

    @property
    def cache_dir(self) -> Path:
        return self.data_dir / "cache"

    @property
    def favourites_file(self) -> Path:
        return self.data_dir / "favourites.json"

    @property
    def folders_file(self) -> Path:
        return self.data_dir / "folders.json"


settings = Settings()
for d in (settings.tokens_dir, settings.data_dir, settings.thumbs_dir, settings.cache_dir):
    d.mkdir(parents=True, exist_ok=True)
