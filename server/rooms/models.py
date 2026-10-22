"""Request bodies and the player records hosts and joiners report.

Everything a client sends that other players will see goes through `clean_text`:
control and format characters are dropped and the length is capped, so a room
or character name cannot break another player's list.
"""

import unicodedata
from typing import Literal, Optional

from pydantic import BaseModel, Field, field_validator

NAME_MAX = 32
AREA_MAX = 40

Difficulty = Literal["normal", "nightmare", "hell"]
Mode = Literal["softcore", "hardcore"]


def clean_text(value: str, limit: int) -> str:
    kept = "".join(ch for ch in value if unicodedata.category(ch)[0] != "C")
    return " ".join(kept.split())[:limit]


class Mod(BaseModel):
    # Launcher mod ids: "d2r-vr" (Authentic), "d2r-pcvr" (Unleashed), "none".
    id: str = Field(pattern=r"^[a-z0-9][a-z0-9-]{0,31}$")
    version: str = Field(default="", pattern=r"^[0-9A-Za-z.+-]{0,24}$")


class Player(BaseModel):
    name: str
    # Lower-case class name ("paladin"); not a fixed list, so a class the game
    # adds later does not make the whole roster invalid.
    char_class: str = Field(default="unknown", alias="class", pattern=r"^[a-z]{1,16}$")
    level: Optional[int] = Field(default=None, ge=1, le=99)
    act: Optional[int] = Field(default=None, ge=1, le=5)
    area: str = ""
    vr: Optional[bool] = None
    # The roster entry of the player whose game wrote it (the host's own char).
    self_: bool = Field(default=False, alias="self")

    model_config = {"populate_by_name": True}

    @field_validator("name")
    @classmethod
    def _name(cls, v: str) -> str:
        v = clean_text(v, NAME_MAX)
        if not v:
            raise ValueError("empty name")
        return v

    @field_validator("area")
    @classmethod
    def _area(cls, v: str) -> str:
        return clean_text(v, AREA_MAX)


class CreateRoom(BaseModel):
    v: int = 1
    name: str
    mod: Mod
    difficulty: Difficulty = "normal"
    mode: Mode = "softcore"
    act: Optional[int] = Field(default=None, ge=1, le=5)
    max_players: int = Field(default=8, ge=2, le=8)
    public: bool = True
    password: str = Field(default="", max_length=64)
    # The host's own character when the helper knows it without a plugin.
    player: Optional[Player] = None

    @field_validator("name")
    @classmethod
    def _name(cls, v: str) -> str:
        v = clean_text(v, NAME_MAX)
        if not v:
            raise ValueError("empty room name")
        return v


class JoinRoom(BaseModel):
    v: int = 1
    room_id: Optional[str] = Field(default=None, max_length=32)
    code: Optional[str] = Field(default=None, max_length=8)
    password: str = Field(default="", max_length=64)
    mod: Mod
    player: Optional[Player] = None


class HostState(BaseModel):
    """A `state` message on the host's control socket (every ~5 s)."""

    players: list[Player] = Field(default_factory=list, max_length=8)
    difficulty: Optional[Difficulty] = None
    mode: Optional[Mode] = None
    act: Optional[int] = Field(default=None, ge=1, le=5)
    vr: Optional[bool] = None
