"""Test fixtures: a real uvicorn server on a free port, with test-sized limits."""

import asyncio
import socket
import sys
import threading
import time
from pathlib import Path

import pytest
import uvicorn

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "server"))
sys.path.insert(0, str(ROOT / "tools"))

from rooms.app import create_app  # noqa: E402
from rooms.config import Settings  # noqa: E402


class LiveServer:
    def __init__(self, settings: Settings):
        self.settings = settings
        self.app = create_app(settings)
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        self.port = sock.getsockname()[1]
        sock.close()
        cfg = uvicorn.Config(self.app, host="127.0.0.1", port=self.port, log_level="warning",
                             access_log=False, ws="websockets-sansio", ws_max_size=settings.frame_max + 1024)
        self.server = uvicorn.Server(cfg)
        self.thread = threading.Thread(target=self.server.run, daemon=True)

    @property
    def http(self) -> str:
        return f"http://127.0.0.1:{self.port}"

    @property
    def ws(self) -> str:
        return f"ws://127.0.0.1:{self.port}"

    @property
    def registry(self):
        return self.app.state.registry

    def start(self):
        self.thread.start()
        deadline = time.time() + 10
        while not self.server.started:
            if time.time() > deadline:
                raise RuntimeError("server did not start")
            time.sleep(0.02)

    def stop(self):
        self.server.should_exit = True
        self.thread.join(10)


def make_settings(**over) -> Settings:
    s = Settings(host_grace=1.0, state_timeout=3.0, tunnel_connect_timeout=1.5, sweep_interval=0.2)
    for k, v in over.items():
        setattr(s, k, v)
    return s


@pytest.fixture
def live():
    """Factory: live(**settings) -> a started LiveServer, stopped after the test."""
    started = []

    def make(**over):
        srv = LiveServer(make_settings(**over))
        srv.start()
        started.append(srv)
        return srv

    yield make
    for srv in started:
        srv.stop()


def run(coro, timeout: float = 30):
    return asyncio.run(asyncio.wait_for(coro, timeout))
