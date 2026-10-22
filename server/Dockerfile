FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY rooms ./rooms

RUN useradd --system --no-create-home rooms
USER rooms

EXPOSE 8080
# One worker: rooms live in this process's memory. No access log and log level
# warning: uvicorn logs every WebSocket at INFO with the player's address and
# the token from the URL. The client address behind Caddy is read by
# the app itself (ROOMS_TRUST_PROXY). --ws-max-size stays a little above
# ROOMS_FRAME_MAX so the app, not uvicorn, answers an oversized frame.
CMD ["uvicorn", "rooms.app:app", "--host", "0.0.0.0", "--port", "8080", "--workers", "1", \
     "--no-access-log", "--log-level", "warning", "--ws", "websockets-sansio", "--ws-max-size", "70000"]
