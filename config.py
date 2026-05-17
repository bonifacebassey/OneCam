from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_file=".env", env_file_encoding="utf-8", extra="ignore")

    host: str = "0.0.0.0"
    port: int = 8000
    cameras_file: str = "cameras.json"
    snapshots_dir: str = "snapshots"
    proxy_timeout: float = 10.0
    stream_chunk_size: int = 8192
    motion_poll_interval: float = 2.0
    motion_threshold: float = 0.1       # fraction of pixels that must change to trigger alert
    motion_pixel_threshold: float = 0.15  # per-pixel diff (0–1) to count as "changed"
    motion_bg_alpha: float = 0.05       # background model learning rate (lower = slower adaptation)
    motion_blur_radius: int = 3         # gaussian blur radius to suppress noise
    motion_cooldown_seconds: float = 10.0
    debug: bool = False


settings = Settings()
