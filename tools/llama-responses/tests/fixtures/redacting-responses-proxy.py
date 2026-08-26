#!/usr/bin/env python3
"""Development-only, streaming-safe structural recorder for the Responses API.

This proxy is intentionally narrow.  It accepts loopback requests from an
isolated Codex profile, replaces an ephemeral client bearer token with an
upstream API key read from the environment, and forwards only Models and
Responses API routes.  Its JSONL output records protocol shape and timing; it
never records authorization, instructions, input text, tool names,
descriptions, schemas, arguments, outputs, or model-generated text.
"""

from __future__ import annotations

import argparse
import collections
import hmac
import http.client
import ipaddress
import json
import os
import re
import ssl
import sys
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, TextIO
from urllib.parse import SplitResult, parse_qsl, urlsplit


SUMMARY_SCHEMA = "llama.cpp.responses_redacting_proxy.v1"
DEFAULT_BIND = "127.0.0.1"
DEFAULT_PORT = 8787
DEFAULT_UPSTREAM_KEY_ENV = "OPENAI_API_KEY"
DEFAULT_CLIENT_TOKEN_ENV = "CODEX_PROXY_CLIENT_TOKEN"
DEFAULT_INSPECTION_LIMIT = 8 * 1024 * 1024
READ_SIZE = 64 * 1024
MAX_SEQUENCE_RUNS = 512
SAFE_TOKEN_RE = re.compile(r"^[A-Za-z0-9_.:-]{1,80}$")
SAFE_REQUEST_ID_RE = re.compile(r"^[A-Za-z0-9_.:/-]{1,256}$")

HOP_BY_HOP_HEADERS = {
    "connection",
    "keep-alive",
    "proxy-authenticate",
    "proxy-authorization",
    "te",
    "trailer",
    "transfer-encoding",
    "upgrade",
}
SENSITIVE_REQUEST_HEADERS = {
    "authorization",
    "cookie",
    "proxy-authorization",
    "x-api-key",
}
SENSITIVE_RESPONSE_HEADERS = {"set-cookie"}
SAFE_EXECUTION_MODES = {"client", "server"}
SAFE_STATUSES = {
    "cancelled",
    "completed",
    "failed",
    "in_progress",
    "incomplete",
    "queued",
}
SAFE_ROLES = {"assistant", "developer", "system", "tool", "user"}
SAFE_TOOL_CHOICES = {"auto", "none", "required"}
SAFE_REASONING_EFFORTS = {"none", "low", "medium", "high", "xhigh"}
SAFE_REASONING_SUMMARIES = {"auto", "concise", "detailed", "none"}
SAFE_VERBOSITIES = {"low", "medium", "high"}
SAFE_TRUNCATION_MODES = {"auto", "disabled"}
SAFE_SERVICE_TIERS = {"auto", "default", "flex", "priority"}
SAFE_QUERY_PARAMETER_NAMES = {
    "after",
    "before",
    "client_version",
    "include",
    "limit",
    "order",
    "starting_after",
    "stream",
}
USAGE_KEYS = {
    "cached_tokens",
    "input_tokens",
    "input_tokens_details",
    "output_tokens",
    "output_tokens_details",
    "reasoning_tokens",
    "total_tokens",
}


def _safe_protocol_token(value: Any) -> str:
    if isinstance(value, str) and SAFE_TOKEN_RE.fullmatch(value):
        return value
    return "<other>"


def _safe_enum(value: Any, allowed: set[str]) -> str:
    if isinstance(value, str) and value in allowed:
        return value
    return "<other>"


def _safe_model(value: Any) -> str | None:
    if not isinstance(value, str):
        return None
    if len(value) > 128 or any(ord(character) < 0x20 for character in value):
        return "<other>"
    return value


def _safe_request_id(value: str | None) -> str | None:
    if value is None:
        return None
    if SAFE_REQUEST_ID_RE.fullmatch(value):
        return value
    return "<redacted>"


def _safe_content_type(value: str) -> str:
    media_type = value.split(";", 1)[0].strip().lower()
    if re.fullmatch(r"[a-z0-9!#$&^_.+-]+/[a-z0-9!#$&^_.+-]+", media_type):
        return media_type
    return "<other>" if media_type else ""


class ObservedSequence:
    """A counted, run-length encoded sequence with bounded log growth."""

    def __init__(self, *, max_runs: int = MAX_SEQUENCE_RUNS) -> None:
        self.counts: collections.Counter[str] = collections.Counter()
        self.runs: list[dict[str, Any]] = []
        self.total = 0
        self.max_runs = max_runs
        self.truncated = False

    def add(self, value: str) -> None:
        self.counts[value] += 1
        self.total += 1
        if self.runs and self.runs[-1]["value"] == value:
            self.runs[-1]["count"] += 1
            return
        if len(self.runs) >= self.max_runs:
            self.truncated = True
            return
        self.runs.append({"value": value, "count": 1})

    def as_json(self) -> dict[str, Any]:
        return {
            "total": self.total,
            "counts": dict(sorted(self.counts.items())),
            "sequence_runs": self.runs,
            "sequence_truncated": self.truncated,
        }


class StructuralObservation:
    """Collect response structure without retaining payload-bearing fields."""

    def __init__(self) -> None:
        self.event_types = ObservedSequence()
        self.item_types = ObservedSequence()
        self.content_types = ObservedSequence()
        self.execution_modes = ObservedSequence()
        self.call_id_nullness = ObservedSequence()
        self.statuses = ObservedSequence()
        self.usage: dict[str, Any] | None = None
        self.json_parse_errors = 0

    def observe_event(self, value: Any, *, event_name: str | None = None) -> None:
        if isinstance(value, dict):
            event_type = value.get("type")
            if isinstance(event_type, str):
                self.event_types.add(_safe_protocol_token(event_type))
            elif event_name is not None:
                self.event_types.add(_safe_protocol_token(event_name))
            self._observe_container(value)
        elif event_name is not None:
            self.event_types.add(_safe_protocol_token(event_name))

    def observe_response(self, value: Any) -> None:
        if isinstance(value, dict):
            self._observe_container(value)

    def _observe_container(self, value: dict[str, Any]) -> None:
        self._observe_status(value.get("status"))
        self._observe_execution(value.get("execution"))
        if "call_id" in value:
            self.call_id_nullness.add(
                "null" if value["call_id"] is None else "non_null"
            )
        self._observe_usage(value.get("usage"))
        self._observe_item(value.get("item"))

        response = value.get("response")
        if isinstance(response, dict):
            self._observe_status(response.get("status"))
            self._observe_usage(response.get("usage"))
            self._observe_output(response.get("output"))

        self._observe_output(value.get("output"))

    def _observe_output(self, output: Any) -> None:
        if isinstance(output, list):
            for item in output:
                self._observe_item(item)

    def _observe_item(self, item: Any) -> None:
        if not isinstance(item, dict):
            return
        self.item_types.add(_safe_protocol_token(item.get("type")))
        self._observe_status(item.get("status"))
        self._observe_execution(item.get("execution"))
        if "call_id" in item:
            self.call_id_nullness.add("null" if item["call_id"] is None else "non_null")
        content = item.get("content")
        if isinstance(content, list):
            for part in content:
                if isinstance(part, dict):
                    self.content_types.add(_safe_protocol_token(part.get("type")))

    def _observe_status(self, value: Any) -> None:
        if value is not None:
            self.statuses.add(_safe_enum(value, SAFE_STATUSES))

    def _observe_execution(self, value: Any) -> None:
        if value is not None:
            self.execution_modes.add(_safe_enum(value, SAFE_EXECUTION_MODES))

    def _observe_usage(self, value: Any) -> None:
        sanitized = _usage_numbers(value)
        if sanitized:
            self.usage = sanitized

    def as_json(self) -> dict[str, Any]:
        result: dict[str, Any] = {
            "event_types": self.event_types.as_json(),
            "item_types": self.item_types.as_json(),
            "content_types": self.content_types.as_json(),
            "execution_modes": self.execution_modes.as_json(),
            "call_id_nullness": self.call_id_nullness.as_json(),
            "statuses": self.statuses.as_json(),
            "json_parse_errors": self.json_parse_errors,
        }
        if self.usage is not None:
            result["usage"] = self.usage
        return result


def _usage_numbers(value: Any) -> dict[str, Any] | None:
    if not isinstance(value, dict):
        return None
    result: dict[str, Any] = {}
    for key in USAGE_KEYS:
        item = value.get(key)
        if isinstance(item, bool):
            continue
        if isinstance(item, (int, float)):
            result[key] = item
        elif isinstance(item, dict):
            nested = _usage_numbers(item)
            if nested:
                result[key] = nested
    return result or None


def _sequence_from_values(values: list[str]) -> dict[str, Any]:
    sequence = ObservedSequence()
    for value in values:
        sequence.add(value)
    return sequence.as_json()


def _request_json_summary(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict):
        return {"kind": "non_object"}

    summary: dict[str, Any] = {"kind": "object"}
    model = _safe_model(value.get("model"))
    if model is not None:
        summary["model"] = model

    modes: dict[str, Any] = {}
    for key in ("background", "parallel_tool_calls", "store", "stream"):
        if isinstance(value.get(key), bool):
            modes[key] = value[key]
    scalar_modes = (
        ("service_tier", SAFE_SERVICE_TIERS),
        ("truncation", SAFE_TRUNCATION_MODES),
    )
    for key, allowed in scalar_modes:
        if key in value:
            modes[key] = _safe_enum(value[key], allowed)

    tool_choice = value.get("tool_choice")
    if isinstance(tool_choice, str):
        modes["tool_choice"] = _safe_enum(tool_choice, SAFE_TOOL_CHOICES)
    elif isinstance(tool_choice, dict):
        modes["tool_choice_type"] = _safe_protocol_token(tool_choice.get("type"))
        if "mode" in tool_choice:
            modes["tool_choice_mode"] = _safe_enum(
                tool_choice["mode"], SAFE_TOOL_CHOICES
            )

    reasoning = value.get("reasoning")
    if isinstance(reasoning, dict):
        if "effort" in reasoning:
            modes["reasoning_effort"] = _safe_enum(
                reasoning["effort"], SAFE_REASONING_EFFORTS
            )
        if "summary" in reasoning:
            modes["reasoning_summary"] = _safe_enum(
                reasoning["summary"], SAFE_REASONING_SUMMARIES
            )
    text = value.get("text")
    if isinstance(text, dict) and "verbosity" in text:
        modes["verbosity"] = _safe_enum(text["verbosity"], SAFE_VERBOSITIES)
    summary["modes"] = modes

    input_items = value.get("input")
    if isinstance(input_items, str):
        summary["input"] = {
            "kind": "string",
            "utf8_bytes": len(input_items.encode("utf-8")),
        }
    elif isinstance(input_items, list):
        input_types: list[str] = []
        content_types: list[str] = []
        roles: list[str] = []
        executions: list[str] = []
        call_id_nullness: list[str] = []
        declared_tool_types: list[str] = []
        declared_nested_tool_types: list[str] = []
        declared_executions: list[str] = []
        declared_deferred_true = 0
        declared_deferred_false = 0
        selected_tool_types: list[str] = []
        statuses: list[str] = []
        for item in input_items:
            if not isinstance(item, dict):
                input_types.append("<non_object>")
                continue
            item_type = _safe_protocol_token(item.get("type", "message"))
            input_types.append(item_type)
            if "role" in item:
                roles.append(_safe_enum(item["role"], SAFE_ROLES))
            if "execution" in item:
                executions.append(_safe_enum(item["execution"], SAFE_EXECUTION_MODES))
            if "call_id" in item:
                call_id_nullness.append(
                    "null" if item["call_id"] is None else "non_null"
                )
            if "status" in item:
                statuses.append(_safe_enum(item["status"], SAFE_STATUSES))
            content = item.get("content")
            if isinstance(content, list):
                for part in content:
                    if isinstance(part, dict):
                        content_types.append(_safe_protocol_token(part.get("type")))
            if item_type == "tool_search_output" and isinstance(
                item.get("tools"), list
            ):
                for tool in item["tools"]:
                    if isinstance(tool, dict):
                        selected_tool_types.append(
                            _safe_protocol_token(tool.get("type"))
                        )
            if item_type == "additional_tools" and isinstance(item.get("tools"), list):
                for tool in item["tools"]:
                    if not isinstance(tool, dict):
                        declared_tool_types.append("<non_object>")
                        continue
                    declared_tool_types.append(_safe_protocol_token(tool.get("type")))
                    if "execution" in tool:
                        declared_executions.append(
                            _safe_enum(tool["execution"], SAFE_EXECUTION_MODES)
                        )
                    if tool.get("defer_loading") is True:
                        declared_deferred_true += 1
                    elif tool.get("defer_loading") is False:
                        declared_deferred_false += 1
                    nested = tool.get("tools")
                    if isinstance(nested, list):
                        for nested_tool in nested:
                            if isinstance(nested_tool, dict):
                                declared_nested_tool_types.append(
                                    _safe_protocol_token(nested_tool.get("type"))
                                )
        summary["input"] = {
            "kind": "array",
            "count": len(input_items),
            "item_types": _sequence_from_values(input_types),
            "content_types": _sequence_from_values(content_types),
            "roles": _sequence_from_values(roles),
            "execution_modes": _sequence_from_values(executions),
            "call_id_nullness": _sequence_from_values(call_id_nullness),
            "statuses": _sequence_from_values(statuses),
            "declared_tool_types": _sequence_from_values(declared_tool_types),
            "declared_nested_tool_types": _sequence_from_values(
                declared_nested_tool_types
            ),
            "declared_execution_modes": _sequence_from_values(declared_executions),
            "declared_defer_loading": {
                "true": declared_deferred_true,
                "false": declared_deferred_false,
            },
            "selected_tool_types": _sequence_from_values(selected_tool_types),
        }

    tools = value.get("tools")
    if isinstance(tools, list):
        tool_types: list[str] = []
        nested_tool_types: list[str] = []
        executions: list[str] = []
        deferred_true = 0
        deferred_false = 0
        for tool in tools:
            if not isinstance(tool, dict):
                tool_types.append("<non_object>")
                continue
            tool_types.append(_safe_protocol_token(tool.get("type")))
            if "execution" in tool:
                executions.append(_safe_enum(tool["execution"], SAFE_EXECUTION_MODES))
            if tool.get("defer_loading") is True:
                deferred_true += 1
            elif tool.get("defer_loading") is False:
                deferred_false += 1
            nested = tool.get("tools")
            if isinstance(nested, list):
                for nested_tool in nested:
                    if isinstance(nested_tool, dict):
                        nested_tool_types.append(
                            _safe_protocol_token(nested_tool.get("type"))
                        )
        summary["tools"] = {
            "count": len(tools),
            "types": _sequence_from_values(tool_types),
            "nested_types": _sequence_from_values(nested_tool_types),
            "execution_modes": _sequence_from_values(executions),
            "defer_loading": {"true": deferred_true, "false": deferred_false},
        }
    return summary


class SSEObserver:
    """Incrementally inspect SSE envelopes while discarding payload text."""

    def __init__(
        self, observation: StructuralObservation, max_event_bytes: int
    ) -> None:
        self.observation = observation
        self.max_event_bytes = max_event_bytes
        self.line_buffer = bytearray()
        self.data_lines: list[bytes] = []
        self.event_name: str | None = None
        self.event_bytes = 0
        self.skip_event = False

    def feed(self, chunk: bytes) -> None:
        self.line_buffer.extend(chunk)
        while True:
            newline = self.line_buffer.find(b"\n")
            if newline < 0:
                return
            line = bytes(self.line_buffer[:newline])
            del self.line_buffer[: newline + 1]
            if line.endswith(b"\r"):
                line = line[:-1]
            self._line(line)

    def finish(self) -> None:
        if self.line_buffer:
            line = bytes(self.line_buffer)
            self.line_buffer.clear()
            if line.endswith(b"\r"):
                line = line[:-1]
            self._line(line)
        if self.data_lines or self.event_name is not None:
            self._dispatch()

    def _line(self, line: bytes) -> None:
        if not line:
            self._dispatch()
            return
        if line.startswith(b":"):
            return
        field, separator, raw_value = line.partition(b":")
        if not separator:
            raw_value = b""
        elif raw_value.startswith(b" "):
            raw_value = raw_value[1:]
        if field == b"event":
            self.event_name = _safe_protocol_token(raw_value.decode("utf-8", "replace"))
        elif field == b"data":
            self.event_bytes += len(raw_value)
            if self.event_bytes > self.max_event_bytes:
                self.skip_event = True
                self.data_lines.clear()
            elif not self.skip_event:
                self.data_lines.append(raw_value)

    def _dispatch(self) -> None:
        event_name = self.event_name
        if self.skip_event:
            if event_name is not None:
                self.observation.event_types.add(event_name)
            self.observation.json_parse_errors += 1
        elif self.data_lines:
            data = b"\n".join(self.data_lines)
            if data != b"[DONE]":
                try:
                    value = json.loads(data)
                except (UnicodeDecodeError, json.JSONDecodeError):
                    self.observation.json_parse_errors += 1
                    self.observation.observe_event(None, event_name=event_name)
                else:
                    self.observation.observe_event(value, event_name=event_name)
        elif event_name is not None:
            self.observation.observe_event(None, event_name=event_name)
        self.data_lines.clear()
        self.event_name = None
        self.event_bytes = 0
        self.skip_event = False


class ResponseInspector:
    def __init__(
        self, content_type: str, content_encoding: str, max_bytes: int
    ) -> None:
        self.content_type = content_type.lower()
        self.content_encoding = content_encoding.lower()
        self.max_bytes = max_bytes
        self.observation = StructuralObservation()
        self.body_bytes = 0
        self.decoded_bytes = 0
        self._decoded_buffer = bytearray()
        self._inspection_truncated = False
        self._decoder: Any = None
        if self.content_encoding in ("", "identity"):
            self._decoder = None
        elif self.content_encoding == "gzip":
            import zlib

            self._decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
        else:
            self._inspection_truncated = True
        self._sse = (
            SSEObserver(self.observation, max_bytes)
            if "text/event-stream" in self.content_type
            and not self._inspection_truncated
            else None
        )

    def feed(self, chunk: bytes) -> None:
        self.body_bytes += len(chunk)
        if self._inspection_truncated:
            return
        try:
            if self._decoder is not None:
                remaining = self.max_bytes - self.decoded_bytes
                decoded = self._decoder.decompress(chunk, remaining + 1)
                if len(decoded) > remaining or self._decoder.unconsumed_tail:
                    self._inspection_truncated = True
                    self._decoded_buffer.clear()
                    self._sse = None
                    return
            else:
                decoded = chunk
        except Exception:  # The response still belongs to the client; inspection is best effort.
            self._inspection_truncated = True
            self._decoded_buffer.clear()
            self._sse = None
            return
        self._feed_decoded(decoded)

    def finish(self) -> dict[str, Any]:
        if not self._inspection_truncated and self._decoder is not None:
            try:
                remaining = self.max_bytes - self.decoded_bytes
                decoded = self._decoder.flush(remaining + 1)
                if len(decoded) > remaining:
                    self._inspection_truncated = True
                else:
                    self._feed_decoded(decoded)
            except Exception:  # See the best-effort inspection rationale in feed().
                self._inspection_truncated = True
        if self._sse is not None:
            self._sse.finish()
        elif self._decoded_buffer and not self._inspection_truncated:
            try:
                value = json.loads(self._decoded_buffer)
            except (UnicodeDecodeError, json.JSONDecodeError):
                self.observation.json_parse_errors += 1
            else:
                self.observation.observe_response(value)
        result = self.observation.as_json()
        result.update(
            {
                "body_bytes": self.body_bytes,
                "decoded_body_bytes": self.decoded_bytes,
                "inspection_truncated": self._inspection_truncated,
            }
        )
        return result

    def _feed_decoded(self, decoded: bytes) -> None:
        if not decoded:
            return
        self.decoded_bytes += len(decoded)
        if self._sse is not None:
            self._sse.feed(decoded)
            return
        if len(self._decoded_buffer) + len(decoded) > self.max_bytes:
            self._inspection_truncated = True
            self._decoded_buffer.clear()
            return
        self._decoded_buffer.extend(decoded)


class JsonlSink:
    def __init__(self, stream: TextIO, *, close_stream: bool = False) -> None:
        self.stream = stream
        self.close_stream = close_stream
        self.lock = threading.Lock()

    @classmethod
    def from_path(cls, path: str) -> "JsonlSink":
        if path == "-":
            return cls(sys.stdout)
        flags = os.O_WRONLY | os.O_CREAT | os.O_APPEND
        if hasattr(os, "O_NOFOLLOW"):
            flags |= os.O_NOFOLLOW
        descriptor = os.open(path, flags, 0o600)
        stream = os.fdopen(descriptor, "a", encoding="utf-8", buffering=1)
        return cls(stream, close_stream=True)

    def emit(self, value: dict[str, Any]) -> None:
        encoded = json.dumps(value, sort_keys=True, separators=(",", ":"))
        with self.lock:
            self.stream.write(encoded + "\n")
            self.stream.flush()

    def close(self) -> None:
        if self.close_stream:
            self.stream.close()


@dataclass(frozen=True)
class ProxyConfig:
    upstream: SplitResult
    upstream_key: str
    client_token: str
    sink: JsonlSink
    max_inspect_bytes: int = DEFAULT_INSPECTION_LIMIT

    def upstream_path(self, incoming_path: str, query: str) -> str:
        base = self.upstream.path.rstrip("/")
        if incoming_path == "/v1" or incoming_path.startswith("/v1/"):
            relative = incoming_path[3:]
        else:
            relative = incoming_path
        target = f"{base}{relative}" if base else incoming_path
        if not target.startswith("/"):
            target = "/" + target
        return f"{target}?{query}" if query else target


class RedactingProxyServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address: tuple[str, int], config: ProxyConfig) -> None:
        self.proxy_config = config
        super().__init__(address, RedactingProxyHandler)

    def handle_error(self, request: Any, client_address: Any) -> None:
        # Avoid BaseServer's traceback: exception text can contain upstream
        # connection details and does not belong in the structural log.
        print("redacting proxy: request handler failed", file=sys.stderr)


class RedactingProxyHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server: RedactingProxyServer

    def do_DELETE(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self._proxy()

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self._proxy()

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self._proxy()

    def log_message(self, format: str, *args: Any) -> None:
        # The JSONL summary is the only request log.  In particular, do not let
        # framework diagnostics accidentally grow to include headers or bodies.
        return

    def _proxy(self) -> None:
        started_wall = datetime.now(timezone.utc).isoformat()
        started = time.monotonic()
        parsed_path = urlsplit(self.path)
        path = parsed_path.path
        request_summary: dict[str, Any] = {
            "method": self.command,
            "path": path,
            "query_parameter_names": sorted(
                {
                    name if name in SAFE_QUERY_PARAMETER_NAMES else "<other>"
                    for name, _ in parse_qsl(parsed_path.query)
                }
            ),
        }

        if not self._is_loopback_client():
            self._reject(HTTPStatus.FORBIDDEN)
            return
        if not _allowed_path(path):
            self._reject(HTTPStatus.NOT_FOUND)
            return
        if not self._authorized_client():
            self._reject(HTTPStatus.UNAUTHORIZED)
            return

        try:
            body = self._read_body()
        except ValueError:
            self._reject(HTTPStatus.BAD_REQUEST)
            return

        request_summary.update(
            _summarize_request_body(
                body,
                self.headers.get("Content-Encoding"),
                self.server.proxy_config.max_inspect_bytes,
            )
        )
        upstream_started = time.monotonic()
        connection = self._upstream_connection()
        upstream_path = self.server.proxy_config.upstream_path(path, parsed_path.query)
        try:
            connection.request(
                self.command,
                upstream_path,
                body=body,
                headers=self._upstream_headers(len(body)),
            )
            upstream_response = connection.getresponse()
        except Exception:  # Deliberately do not serialize exception text.
            connection.close()
            self._upstream_failure(
                started_wall, started, request_summary, upstream_started
            )
            return

        headers_received = time.monotonic()
        upstream_request_id = _safe_request_id(
            upstream_response.getheader("x-request-id")
            or upstream_response.getheader("request-id")
            or upstream_response.getheader("openai-request-id")
        )
        content_type = upstream_response.getheader("Content-Type", "")
        content_encoding = upstream_response.getheader("Content-Encoding", "")
        inspector = ResponseInspector(
            content_type, content_encoding, self.server.proxy_config.max_inspect_bytes
        )

        self.send_response(upstream_response.status)
        for name, value in upstream_response.getheaders():
            lowered = name.lower()
            if lowered in HOP_BY_HOP_HEADERS or lowered in SENSITIVE_RESPONSE_HEADERS:
                continue
            self.send_header(name, value)
        # A close-delimited body keeps streaming latency and payload bytes intact
        # even when http.client has decoded upstream chunk framing.
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

        first_byte: float | None = None
        client_disconnected = False
        try:
            while True:
                chunk = upstream_response.read1(READ_SIZE)
                if not chunk:
                    break
                if first_byte is None:
                    first_byte = time.monotonic()
                inspector.feed(chunk)
                try:
                    self.wfile.write(chunk)
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    client_disconnected = True
                    break
        finally:
            upstream_response.close()
            connection.close()

        finished = time.monotonic()
        summary = {
            "schema": SUMMARY_SCHEMA,
            "started_at": started_wall,
            "request": request_summary,
            "upstream": {
                "status_code": upstream_response.status,
                "request_id": upstream_request_id,
                "content_type": _safe_content_type(content_type),
            },
            "response": inspector.finish(),
            "client_disconnected": client_disconnected,
            "timing_ms": {
                "upstream_headers": round(
                    (headers_received - upstream_started) * 1000, 3
                ),
                "first_body_byte": (
                    round((first_byte - upstream_started) * 1000, 3)
                    if first_byte is not None
                    else None
                ),
                "total": round((finished - started) * 1000, 3),
            },
        }
        self.server.proxy_config.sink.emit(summary)

    def _is_loopback_client(self) -> bool:
        try:
            return ipaddress.ip_address(self.client_address[0]).is_loopback
        except ValueError:
            return False

    def _authorized_client(self) -> bool:
        expected = f"Bearer {self.server.proxy_config.client_token}"
        actual = self.headers.get("Authorization", "")
        return hmac.compare_digest(actual, expected)

    def _read_body(self) -> bytes:
        transfer_encoding = self.headers.get("Transfer-Encoding", "").lower()
        if transfer_encoding:
            if transfer_encoding != "chunked":
                raise ValueError("unsupported transfer encoding")
            return self._read_chunked_body()
        raw_length = self.headers.get("Content-Length")
        if raw_length is None:
            return b""
        try:
            length = int(raw_length)
        except ValueError as error:
            raise ValueError("invalid content length") from error
        if length < 0:
            raise ValueError("invalid content length")
        return self.rfile.read(length)

    def _read_chunked_body(self) -> bytes:
        body = bytearray()
        while True:
            size_line = self.rfile.readline(4097)
            if len(size_line) > 4096:
                raise ValueError("chunk header too long")
            try:
                size = int(size_line.split(b";", 1)[0].strip(), 16)
            except ValueError as error:
                raise ValueError("invalid chunk size") from error
            if size == 0:
                while self.rfile.readline(4097) not in (b"\r\n", b"\n", b""):
                    pass
                return bytes(body)
            body.extend(self.rfile.read(size))
            if self.rfile.read(2) != b"\r\n":
                raise ValueError("invalid chunk terminator")

    def _upstream_connection(self) -> http.client.HTTPConnection:
        upstream = self.server.proxy_config.upstream
        if upstream.scheme == "https":
            return http.client.HTTPSConnection(
                upstream.hostname,
                upstream.port or 443,
                context=ssl.create_default_context(),
                timeout=300,
            )
        return http.client.HTTPConnection(
            upstream.hostname, upstream.port or 80, timeout=300
        )

    def _upstream_headers(self, body_length: int) -> dict[str, str]:
        headers: dict[str, str] = {}
        for name, value in self.headers.items():
            lowered = name.lower()
            if (
                lowered in HOP_BY_HOP_HEADERS
                or lowered in SENSITIVE_REQUEST_HEADERS
                or lowered in {"accept-encoding", "content-length", "host"}
            ):
                continue
            headers[name] = value
        headers["Authorization"] = f"Bearer {self.server.proxy_config.upstream_key}"
        headers["Accept-Encoding"] = "identity"
        headers["Content-Length"] = str(body_length)
        return headers

    def _reject(self, status: HTTPStatus) -> None:
        body = json.dumps(
            {"error": {"type": "redacting_proxy_rejected"}}, separators=(",", ":")
        ).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def _upstream_failure(
        self,
        started_wall: str,
        started: float,
        request_summary: dict[str, Any],
        upstream_started: float,
    ) -> None:
        body = json.dumps(
            {"error": {"type": "redacting_proxy_upstream_error"}}, separators=(",", ":")
        ).encode()
        self.send_response(HTTPStatus.BAD_GATEWAY)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True
        finished = time.monotonic()
        self.server.proxy_config.sink.emit(
            {
                "schema": SUMMARY_SCHEMA,
                "started_at": started_wall,
                "request": request_summary,
                "upstream": {"status_code": HTTPStatus.BAD_GATEWAY, "request_id": None},
                "response": {"body_bytes": 0, "inspection_truncated": False},
                "client_disconnected": False,
                "timing_ms": {
                    "upstream_headers": round((finished - upstream_started) * 1000, 3),
                    "first_body_byte": None,
                    "total": round((finished - started) * 1000, 3),
                },
            }
        )


def _allowed_path(path: str) -> bool:
    return (
        path == "/v1/models"
        or path.startswith("/v1/models/")
        or path == "/v1/responses"
        or path.startswith("/v1/responses/")
    )


def _summarize_request_body(
    body: bytes, content_encoding: str | None, max_bytes: int
) -> dict[str, Any]:
    encoding = (content_encoding or "identity").lower()
    result: dict[str, Any] = {
        "body_bytes": len(body),
        "content_encoding": encoding
        if encoding in {"", "gzip", "identity"}
        else "<other>",
    }
    if len(body) > max_bytes:
        result["inspection"] = "size_limit"
        return result
    if encoding in ("", "identity"):
        decoded = body
    elif encoding == "gzip":
        try:
            import zlib

            decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
            decoded = decoder.decompress(body, max_bytes + 1)
            if len(decoded) > max_bytes or decoder.unconsumed_tail:
                result["inspection"] = "decoded_size_limit"
                return result
            decoded += decoder.flush(max_bytes + 1 - len(decoded))
        except (EOFError, OSError, zlib.error):
            result["inspection"] = "invalid_encoding"
            return result
    else:
        result["inspection"] = "unsupported_encoding"
        return result
    result["decoded_body_bytes"] = len(decoded)
    if len(decoded) > max_bytes:
        result["inspection"] = "decoded_size_limit"
        return result
    if not decoded:
        result["json"] = {"kind": "empty"}
        return result
    try:
        value = json.loads(decoded)
    except (UnicodeDecodeError, json.JSONDecodeError):
        result["inspection"] = "invalid_json"
        return result
    result["json"] = _request_json_summary(value)
    return result


def _validate_upstream(value: str, *, allow_http: bool) -> SplitResult:
    parsed = urlsplit(value)
    if parsed.scheme not in ({"https", "http"} if allow_http else {"https"}):
        raise ValueError(
            "upstream must use HTTPS (or explicitly allowed loopback HTTP)"
        )
    if (
        not parsed.hostname
        or parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
    ):
        raise ValueError(
            "upstream must be an explicit base URL without credentials, query, or fragment"
        )
    if parsed.scheme == "http":
        try:
            if not ipaddress.ip_address(parsed.hostname).is_loopback:
                raise ValueError("HTTP upstream must be loopback")
        except ValueError as error:
            raise ValueError("HTTP upstream must be a loopback IP address") from error
    return parsed


def _validate_bind(value: str) -> None:
    try:
        is_loopback = ipaddress.ip_address(value).is_loopback
    except ValueError as error:
        raise ValueError("bind must be an explicit IP address") from error
    if not is_loopback:
        raise ValueError("the development proxy only binds to loopback")


def _validate_codex_home(value: str) -> Path:
    path = Path(value).expanduser().resolve()
    default_home = (Path.home() / ".codex").resolve()
    if path == default_home:
        raise ValueError(
            "refusing the default ~/.codex profile; use an isolated CODEX_HOME"
        )
    if not path.is_dir():
        raise ValueError("isolated CODEX_HOME must already exist")
    return path


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--upstream",
        required=True,
        help="explicit upstream API base URL, normally https://api.openai.com/v1",
    )
    parser.add_argument(
        "--codex-home",
        required=True,
        help="existing isolated CODEX_HOME; ~/.codex is refused",
    )
    parser.add_argument(
        "--bind", default=DEFAULT_BIND, help=f"listen address (default: {DEFAULT_BIND})"
    )
    parser.add_argument(
        "--port",
        type=int,
        default=DEFAULT_PORT,
        help=f"listen port (default: {DEFAULT_PORT})",
    )
    parser.add_argument(
        "--summary-file",
        default="-",
        help="append JSONL summaries here, or - for stdout",
    )
    parser.add_argument(
        "--upstream-key-env",
        default=DEFAULT_UPSTREAM_KEY_ENV,
        help="environment variable holding the upstream bearer token",
    )
    parser.add_argument(
        "--client-token-env",
        default=DEFAULT_CLIENT_TOKEN_ENV,
        help="environment variable holding the ephemeral inbound bearer token",
    )
    parser.add_argument(
        "--max-inspect-bytes", type=int, default=DEFAULT_INSPECTION_LIMIT
    )
    parser.add_argument(
        "--allow-http-upstream",
        action="store_true",
        help="allow only a loopback HTTP upstream, intended for tests",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        _validate_bind(args.bind)
        _validate_codex_home(args.codex_home)
        upstream = _validate_upstream(
            args.upstream, allow_http=args.allow_http_upstream
        )
        if args.max_inspect_bytes <= 0:
            raise ValueError("--max-inspect-bytes must be positive")
        upstream_key = os.environ.get(args.upstream_key_env)
        client_token = os.environ.get(args.client_token_env)
        if not upstream_key:
            raise ValueError(
                f"missing upstream key environment variable: {args.upstream_key_env}"
            )
        if not client_token:
            raise ValueError(
                f"missing client token environment variable: {args.client_token_env}"
            )
        if hmac.compare_digest(upstream_key, client_token):
            raise ValueError("client token must differ from the upstream API key")
        sink = JsonlSink.from_path(args.summary_file)
    except (OSError, ValueError) as error:
        print(f"redacting proxy: {error}", file=sys.stderr)
        return 2

    config = ProxyConfig(
        upstream=upstream,
        upstream_key=upstream_key,
        client_token=client_token,
        sink=sink,
        max_inspect_bytes=args.max_inspect_bytes,
    )
    server = RedactingProxyServer((args.bind, args.port), config)
    host, port = server.server_address[:2]
    print(
        f"redacting proxy listening on http://{host}:{port}; upstream={upstream.scheme}://{upstream.hostname}{upstream.path}",
        file=sys.stderr,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        sink.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
