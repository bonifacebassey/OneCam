import json

from fastapi import HTTPException
from pydantic import BaseModel, computed_field


class CameraConfig(BaseModel):
    id: str
    label: str
    ip: str
    enabled: bool = True
    stream_port: int = 80  # set to 81 when running the two-port firmware

    @computed_field
    @property
    def stream_url(self) -> str:
        port = f":{self.stream_port}" if self.stream_port != 80 else ""
        return f"http://{self.ip}{port}/stream"

    @computed_field
    @property
    def capture_url(self) -> str:
        return f"http://{self.ip}/capture"

    @computed_field
    @property
    def control_url(self) -> str:
        return f"http://{self.ip}/control"


def load_cameras(path: str) -> dict[str, "CameraConfig"]:
    with open(path) as f:
        data = json.load(f)
    return {cam.id: cam for item in data["cameras"] if (cam := CameraConfig(**item)).enabled}


def get_camera(cameras: dict[str, CameraConfig], cam_id: str) -> CameraConfig:
    if cam_id not in cameras:
        raise HTTPException(status_code=404, detail=f"Camera '{cam_id}' not found")
    return cameras[cam_id]
