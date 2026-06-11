#!/usr/bin/env python3
"""Local Xiaozhi proxy helper for the ornament bridge.

The Rust bridge owns ESP32-facing PCM queues. This helper owns the service-side
WebSocket session. Raw Opus encode/decode is intentionally delegated to a local
Opus backend; if none is present the helper reports a clear bridge status error.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from dataclasses import dataclass
from typing import Optional

import websocket


SAMPLE_RATE = 16000
CHANNELS = 1
FRAME_MS = 60
PCM_FRAME_BYTES = SAMPLE_RATE * FRAME_MS // 1000 * 2


@dataclass
class Args:
    bridge: str
    ws_url: str
    token: Optional[str]


def post_json(base: str, path: str, payload: dict) -> None:
    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(
        base.rstrip("/") + path,
        data=body,
        method="POST",
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=2) as response:
        response.read()


def post_pcm(base: str, path: str, pcm: bytes) -> None:
    request = urllib.request.Request(
        base.rstrip("/") + path,
        data=pcm,
        method="POST",
        headers={"Content-Type": "audio/L16; rate=16000; channels=1"},
    )
    with urllib.request.urlopen(request, timeout=2) as response:
        response.read()


def get_pcm(base: str, path: str) -> bytes:
    try:
        with urllib.request.urlopen(base.rstrip("/") + path, timeout=2) as response:
            if response.status == 204:
                return b""
            return response.read()
    except urllib.error.HTTPError as error:
        if error.code == 204:
            return b""
        raise


def get_json(base: str, path: str) -> dict:
    with urllib.request.urlopen(base.rstrip("/") + path, timeout=2) as response:
        return json.loads(response.read().decode("utf-8"))


def update_status(base: str, **payload: object) -> None:
    try:
        post_json(base, "/v1/xiaozhi/proxy/status", payload)
    except Exception as error:
        print(f"status update failed: {error}", file=sys.stderr)


class MissingOpusBackend:
    available = False

    def encode(self, _pcm: bytes) -> bytes:
        raise RuntimeError(
            "raw Opus backend missing; install opuslib or provide a libopus-backed encoder"
        )

    def decode(self, _packet: bytes) -> bytes:
        raise RuntimeError(
            "raw Opus backend missing; install opuslib or provide a libopus-backed decoder"
        )


class CtypesOpusBackend:
    available = True
    OPUS_APPLICATION_VOIP = 2048
    OPUS_OK = 0
    MAX_PACKET_BYTES = 1500

    def __init__(self, lib) -> None:
        self.lib = lib
        self._bind()
        err = ctypes.c_int()
        self.encoder = self.lib.opus_encoder_create(
            SAMPLE_RATE, CHANNELS, self.OPUS_APPLICATION_VOIP, ctypes.byref(err)
        )
        if err.value != self.OPUS_OK or not self.encoder:
            raise RuntimeError(f"opus_encoder_create failed: {err.value}")
        self.decoder = self.lib.opus_decoder_create(
            SAMPLE_RATE, CHANNELS, ctypes.byref(err)
        )
        if err.value != self.OPUS_OK or not self.decoder:
            self.lib.opus_encoder_destroy(self.encoder)
            raise RuntimeError(f"opus_decoder_create failed: {err.value}")

    def _bind(self) -> None:
        self.lib.opus_encoder_create.argtypes = [
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_int),
        ]
        self.lib.opus_encoder_create.restype = ctypes.c_void_p
        self.lib.opus_encoder_destroy.argtypes = [ctypes.c_void_p]
        self.lib.opus_encoder_destroy.restype = None
        self.lib.opus_decoder_create.argtypes = [
            ctypes.c_int,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_int),
        ]
        self.lib.opus_decoder_create.restype = ctypes.c_void_p
        self.lib.opus_decoder_destroy.argtypes = [ctypes.c_void_p]
        self.lib.opus_decoder_destroy.restype = None
        self.lib.opus_encode.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_int16),
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_ubyte),
            ctypes.c_int32,
        ]
        self.lib.opus_encode.restype = ctypes.c_int32
        self.lib.opus_decode.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_ubyte),
            ctypes.c_int32,
            ctypes.POINTER(ctypes.c_int16),
            ctypes.c_int,
            ctypes.c_int,
        ]
        self.lib.opus_decode.restype = ctypes.c_int

    def encode(self, pcm: bytes) -> bytes:
        frame_size = len(pcm) // 2
        pcm_buffer = (ctypes.c_int16 * frame_size).from_buffer_copy(pcm)
        packet = (ctypes.c_ubyte * self.MAX_PACKET_BYTES)()
        encoded = self.lib.opus_encode(
            self.encoder, pcm_buffer, frame_size, packet, self.MAX_PACKET_BYTES
        )
        if encoded < 0:
            raise RuntimeError(f"opus_encode failed: {encoded}")
        return bytes(packet[:encoded])

    def decode(self, packet: bytes) -> bytes:
        frame_size = SAMPLE_RATE * FRAME_MS // 1000
        packet_buffer = (ctypes.c_ubyte * len(packet)).from_buffer_copy(packet)
        pcm = (ctypes.c_int16 * frame_size)()
        decoded = self.lib.opus_decode(
            self.decoder, packet_buffer, len(packet), pcm, frame_size, 0
        )
        if decoded < 0:
            raise RuntimeError(f"opus_decode failed: {decoded}")
        return bytes(pcm)[: decoded * 2]

    def __del__(self) -> None:
        encoder = getattr(self, "encoder", None)
        decoder = getattr(self, "decoder", None)
        lib = getattr(self, "lib", None)
        if lib is not None and encoder:
            lib.opus_encoder_destroy(encoder)
        if lib is not None and decoder:
            lib.opus_decoder_destroy(decoder)


def load_opus_backend():
    lib = load_opus_dll()
    if lib is not None:
        try:
            return CtypesOpusBackend(lib)
        except Exception as error:
            print(f"ctypes Opus backend failed: {error}", file=sys.stderr)
    try:
        import opuslib  # type: ignore
    except Exception:
        return MissingOpusBackend()

    class OpusLibBackend:
        available = True

        def __init__(self) -> None:
            self.encoder = opuslib.Encoder(SAMPLE_RATE, CHANNELS, opuslib.APPLICATION_VOIP)
            self.decoder = opuslib.Decoder(SAMPLE_RATE, CHANNELS)

        def encode(self, pcm: bytes) -> bytes:
            return self.encoder.encode(pcm, FRAME_MS)

        def decode(self, packet: bytes) -> bytes:
            return self.decoder.decode(packet, SAMPLE_RATE * FRAME_MS // 1000)

    return OpusLibBackend()


def load_opus_dll():
    candidates = []
    env_path = os.environ.get("CODEX_ORNAMENT_OPUS_DLL")
    if env_path:
        candidates.append(Path(env_path))
    home = Path.home()
    candidates.extend(
        [
            home
            / "Miniconda3"
            / "envs"
            / "chatgpt"
            / "Lib"
            / "site-packages"
            / "discord"
            / "bin"
            / "libopus-0.x64.dll",
            home
            / "AppData"
            / "Roaming"
            / "Python"
            / f"Python{sys.version_info.major}{sys.version_info.minor}"
            / "site-packages"
            / "discord"
            / "bin"
            / "libopus-0.x64.dll",
        ]
    )
    for path in candidates:
        if path.is_file():
            return ctypes.CDLL(str(path))
    return None


def websocket_headers(args: Args) -> list[str]:
    headers = [
        "Protocol-Version: 1",
        "Device-Id: codex-ornament-bridge",
        "Client-Id: codex-ornament-bridge",
    ]
    if args.token:
        headers.append(f"Authorization: Bearer {args.token}")
    return headers


def send_hello(ws) -> None:
    ws.send(
        json.dumps(
            {
                "type": "hello",
                "version": 1,
                "transport": "websocket",
                "audio_params": {
                    "format": "opus",
                    "sample_rate": SAMPLE_RATE,
                    "channels": CHANNELS,
                    "frame_duration": FRAME_MS,
                },
            },
            separators=(",", ":"),
        )
    )


def send_listen(ws, session_id: str, state: str) -> None:
    ws.send(
        json.dumps(
            {"session_id": session_id, "type": "listen", "state": state, "mode": "auto"},
            separators=(",", ":"),
        )
    )


def handle_text_message(base: str, text: str, state: dict) -> None:
    try:
        message = json.loads(text)
    except json.JSONDecodeError:
        return

    msg_type = message.get("type")
    if msg_type == "hello" and message.get("transport") == "websocket":
        session_id = message.get("session_id") or state.get("session_id") or "bridge"
        state["session_id"] = session_id
        update_status(
            base,
            state="listening",
            connected=True,
            configured=True,
            upstreamRunning=True,
            sessionId=session_id,
            lastError="",
        )
    elif msg_type == "stt":
        update_status(base, lastStt=str(message.get("text") or ""))
    elif msg_type == "tts":
        tts_state = message.get("state")
        if tts_state == "start":
            update_status(base, state="speaking", connected=True, upstreamRunning=True)
        elif tts_state == "stop":
            update_status(base, state="listening", connected=True, upstreamRunning=True)
        elif tts_state == "sentence_start":
            update_status(base, lastTts=str(message.get("text") or ""))
    elif msg_type == "alert":
        update_status(base, state="error", lastError=str(message.get("message") or "alert"))


def run_proxy(args: Args) -> int:
    opus = load_opus_backend()
    if not opus.available:
        update_status(
            args.bridge,
            state="error",
            configured=True,
            connected=False,
            upstreamRunning=False,
            lastError="raw Opus backend missing on PC bridge",
        )
        print("raw Opus backend missing; install opuslib or provide libopus", file=sys.stderr)
        return 2

    update_status(args.bridge, state="connecting", configured=True, upstreamRunning=True)
    ws = websocket.create_connection(args.ws_url, header=websocket_headers(args), timeout=10)
    ws.settimeout(0.05)
    state = {"session_id": "bridge"}
    send_hello(ws)
    last_listen = 0.0

    try:
        while True:
            now = time.monotonic()
            if state.get("session_id") and now - last_listen > 3.0:
                send_listen(ws, str(state["session_id"]), "start")
                last_listen = now

            try:
                message = ws.recv()
            except websocket.WebSocketTimeoutException:
                message = None
            if isinstance(message, str):
                handle_text_message(args.bridge, message, state)
            elif isinstance(message, (bytes, bytearray)):
                pcm = opus.decode(bytes(message))
                if pcm:
                    post_pcm(args.bridge, "/v1/xiaozhi/audio/inject", pcm)

            pcm = get_pcm(
                args.bridge,
                f"/v1/xiaozhi/proxy/uplink?max={PCM_FRAME_BYTES}&wait_ms=20",
            )
            if len(pcm) >= PCM_FRAME_BYTES:
                packet = opus.encode(pcm[:PCM_FRAME_BYTES])
                if packet:
                    ws.send_binary(packet)
            if now - float(state.get("last_status_check") or 0.0) > 1.0:
                status = get_json(args.bridge, "/v1/xiaozhi/session/status")
                state["last_status_check"] = now
                if not status.get("sessionRequested", False):
                    break
    finally:
        try:
            send_listen(ws, str(state.get("session_id") or "bridge"), "stop")
            ws.close()
        except Exception:
            pass
        update_status(
            args.bridge,
            state="idle",
            connected=False,
            upstreamRunning=False,
            lastError="",
        )


def parse_args() -> Args:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bridge", required=True)
    parser.add_argument("--ws-url", required=True)
    parser.add_argument("--token")
    parsed = parser.parse_args()
    return Args(bridge=parsed.bridge, ws_url=parsed.ws_url, token=parsed.token)


if __name__ == "__main__":
    try:
        raise SystemExit(run_proxy(parse_args()))
    except KeyboardInterrupt:
        raise SystemExit(130)
    except Exception as error:
        print(f"Xiaozhi proxy error: {error}", file=sys.stderr)
        raise SystemExit(1)
