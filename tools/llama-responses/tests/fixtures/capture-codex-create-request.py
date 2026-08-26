#!/usr/bin/env python3
"""Capture and normalize one Codex CLI POST /v1/responses request.

The listener serves a deterministic private model catalog and terminates the
first Responses request with a non-retryable 400.  It never contacts a model or
an external service.  Run Codex separately with the loopback provider described
in codex-create-request-0.149.1.md.
"""

from __future__ import annotations

import argparse
import copy
import gzip
import hashlib
import json
import os
from datetime import date
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from typing import Any


FIXTURES_DIR = Path(__file__).resolve().parent
PROMPT_PATH = FIXTURES_DIR.parents[1] / "prompts" / "codex-base-instructions.md"
PROMPT_REFERENCE = "../../prompts/codex-base-instructions.md"
CAPTURE_PROMPT = "Reply with exactly CAPTURE_OK and do not call tools."


def text_fingerprint(value: str) -> dict[str, Any]:
    encoded = value.encode("utf-8")
    return {
        "characters": len(value),
        "utf8_bytes": len(encoded),
        "sha256": hashlib.sha256(encoded).hexdigest(),
    }


def json_shape(value: Any) -> Any:
    if isinstance(value, dict):
        return {
            "type": "object",
            "properties": {key: json_shape(item) for key, item in sorted(value.items())},
        }
    if isinstance(value, list):
        return {
            "type": "array",
            "length": len(value),
            "items": [json_shape(item) for item in value],
        }
    if value is None:
        return {"type": "null"}
    if isinstance(value, bool):
        return {"type": "boolean"}
    if isinstance(value, str):
        return {"type": "string"}
    if isinstance(value, int):
        return {"type": "integer"}
    if isinstance(value, float):
        return {"type": "number"}
    return {"type": type(value).__name__}


def private_catalog(base_instructions: str) -> dict[str, Any]:
    return {
        "models": [
            {
                "slug": "capture-model",
                "display_name": "Deterministic Capture Model",
                "description": "Local request-capture fixture.",
                "supported_reasoning_levels": [
                    {"effort": "low", "description": "Capture effort"},
                    {"effort": "medium", "description": "Capture effort"},
                    {"effort": "xhigh", "description": "Capture effort"},
                ],
                "default_reasoning_level": "low",
                "shell_type": "unified_exec",
                "visibility": "list",
                "supported_in_api": True,
                "priority": 1,
                "availability_nux": None,
                "upgrade": None,
                "base_instructions": base_instructions,
                "include_skills_usage_instructions": False,
                "include_plugin_usage_instructions": False,
                "include_apps_usage_instructions": False,
                "supports_reasoning_summary_parameter": False,
                "default_reasoning_summary": "none",
                "support_verbosity": False,
                "default_verbosity": None,
                "apply_patch_tool_type": "freeform",
                "truncation_policy": {"mode": "tokens", "limit": 10_000},
                "supports_image_detail_original": False,
                "effective_context_window_percent": 95,
                "experimental_supported_tools": [],
                "input_modalities": ["text", "image"],
                "supports_search_tool": False,
                "use_responses_lite": False,
                "context_window": 32_768,
                "max_context_window": 32_768,
            }
        ]
    }


def normalize_request(
    request: dict[str, Any],
    *,
    args: argparse.Namespace,
    raw_bytes: int,
    decoded_bytes: int,
    content_encoding: str | None,
    request_headers: dict[str, str],
    model_gets: list[str],
) -> dict[str, Any]:
    normalized = copy.deepcopy(request)
    replacements: list[dict[str, Any]] = []

    instructions = normalized.get("instructions")
    if not isinstance(instructions, str):
        raise ValueError("captured request has no string instructions field")
    normalized["instructions"] = "$CODEX_BASE_INSTRUCTIONS"

    client_metadata = normalized.get("client_metadata")
    if not isinstance(client_metadata, dict):
        raise ValueError("captured request has no client_metadata object")
    metadata_replacements = {
        "root_turn_id": "$TURN_ID",
        "session_id": "$SESSION_ID",
        "thread_id": "$SESSION_ID",
        "turn_id": "$TURN_ID",
        "x-codex-installation-id": "$INSTALLATION_ID",
        "x-codex-turn-metadata": "$CODEX_TURN_METADATA",
        "x-codex-window-id": "$WINDOW_ID",
    }
    for key, replacement in metadata_replacements.items():
        value = client_metadata.get(key)
        if not isinstance(value, str):
            raise ValueError(f"captured client_metadata.{key} is not a string")
        detail: dict[str, Any] = {
            "pointer": f"/request/client_metadata/{key}",
            "replacement": replacement,
            **text_fingerprint(value),
        }
        if key == "x-codex-turn-metadata":
            try:
                detail["json_shape"] = json_shape(json.loads(value))
            except json.JSONDecodeError:
                detail["json_shape"] = {"type": "invalid_json_string"}
        replacements.append(detail)
        client_metadata[key] = replacement

    prompt_cache_key = normalized.get("prompt_cache_key")
    if not isinstance(prompt_cache_key, str):
        raise ValueError("captured request has no string prompt_cache_key")
    replacements.append(
        {
            "pointer": "/request/prompt_cache_key",
            "replacement": "$SESSION_ID",
            "relation": "same captured value as client_metadata.session_id and client_metadata.thread_id",
            **text_fingerprint(prompt_cache_key),
        }
    )
    normalized["prompt_cache_key"] = "$SESSION_ID"

    input_items = normalized.get("input")
    if not isinstance(input_items, list):
        raise ValueError("captured request has no input array")
    input_text_replacements: list[dict[str, Any]] = []
    for item_index, item in enumerate(input_items):
        if not isinstance(item, dict):
            raise ValueError(f"captured input[{item_index}] is not an object")
        item_id = item.get("id")
        if not isinstance(item_id, str):
            raise ValueError(f"captured input[{item_index}].id is not a string")
        stable_id = f"msg_fixture_{item_index}"
        replacements.append(
            {
                "pointer": f"/request/input/{item_index}/id",
                "replacement": stable_id,
                **text_fingerprint(item_id),
            }
        )
        item["id"] = stable_id
        content = item.get("content")
        if not isinstance(content, list):
            raise ValueError(f"captured input[{item_index}].content is not an array")
        for content_index, part in enumerate(content):
            if not isinstance(part, dict) or not isinstance(part.get("text"), str):
                continue
            text = part["text"]
            if item_index == len(input_items) - 1 and text == CAPTURE_PROMPT:
                continue
            placeholder = f"$INPUT_TEXT_{item_index}_{content_index}"
            input_text_replacements.append(
                {
                    "pointer": f"/request/input/{item_index}/content/{content_index}/text",
                    "replacement": placeholder,
                    "role": item.get("role"),
                    **text_fingerprint(text),
                }
            )
            part["text"] = placeholder

    turn_metadata = request["client_metadata"]["x-codex-turn-metadata"]
    safe_header_values = {
        key.lower(): value
        for key, value in request_headers.items()
        if key.lower() in {"accept", "content-type", "content-encoding"}
    }
    return {
        "fixture_schema": "llama.cpp.codex.responses_create_request.v1",
        "capture": {
            "date": args.capture_date,
            "codex_reported_version": args.codex_version,
            "codex_binary_kind": args.binary_kind,
            "model_catalog_requests": model_gets,
            "transport": {
                "method": "POST",
                "path": "/v1/responses",
                "raw_body_bytes": raw_bytes,
                "decoded_body_bytes": decoded_bytes,
                "content_encoding": content_encoding,
                "header_names": sorted(key.lower() for key in request_headers),
                "stable_header_values": safe_header_values,
                "dynamic_or_redacted_headers": [
                    "authorization",
                    "originator",
                    "session-id",
                    "thread-id",
                    "user-agent",
                    "x-client-request-id",
                    "x-codex-beta-features",
                    "x-codex-turn-metadata",
                    "x-codex-window-id",
                ],
            },
        },
        "normalization": {
            "instructions": {
                "request_placeholder": "$CODEX_BASE_INSTRUCTIONS",
                "source": PROMPT_REFERENCE,
                **text_fingerprint(instructions),
            },
            "dynamic_fields": replacements,
            "input_text_fields": input_text_replacements,
            "turn_metadata_json_shape": json_shape(json.loads(turn_metadata)),
            "comparison_policy": [
                "Materialize or replace the instructions placeholder from the referenced prompt before byte-level replay.",
                "Normalize the listed dynamic JSON pointers before comparing a fresh capture.",
                "Compare tools and all unlisted request fields exactly.",
                "Input-text fingerprints are provenance only; generated developer and environment prose may change with Codex.",
            ],
        },
        "request": normalized,
    }


def write_private_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.chmod(path, 0o600)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18_139)
    parser.add_argument("--raw-output", type=Path, required=True)
    parser.add_argument("--headers-output", type=Path, required=True)
    parser.add_argument("--catalog-output", type=Path, required=True)
    parser.add_argument("--normalized-output", type=Path, required=True)
    parser.add_argument("--codex-version", default="0.149.1")
    parser.add_argument("--binary-kind", default="standalone")
    parser.add_argument("--capture-date", default=date.today().isoformat())
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    base_instructions = PROMPT_PATH.read_text(encoding="utf-8")
    catalog = private_catalog(base_instructions)
    write_private_json(args.catalog_output, catalog)
    model_gets: list[str] = []

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, _format: str, *_values: Any) -> None:
            return

        def send_json(self, status: int, value: Any) -> None:
            payload = json.dumps(value, separators=(",", ":")).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(payload)

        def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
            model_gets.append(self.path)
            self.send_json(200, catalog)

        def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
            raw = self.rfile.read(int(self.headers.get("Content-Length", "0")))
            encoding = (self.headers.get("Content-Encoding") or "").lower()
            decoded = gzip.decompress(raw) if encoding == "gzip" else raw
            request = json.loads(decoded)
            if not isinstance(request, dict):
                raise ValueError("captured request body is not an object")

            write_private_json(args.raw_output, request)
            headers = {
                "header_names": sorted(key.lower() for key in self.headers),
                "authorization": "<redacted>" if self.headers.get("Authorization") else None,
                "safe_values": {
                    key.lower(): value
                    for key, value in self.headers.items()
                    if key.lower() in {"accept", "content-type", "content-encoding", "user-agent"}
                },
            }
            write_private_json(args.headers_output, headers)
            normalized = normalize_request(
                request,
                args=args,
                raw_bytes=len(raw),
                decoded_bytes=len(decoded),
                content_encoding=encoding or None,
                request_headers=dict(self.headers.items()),
                model_gets=model_gets,
            )
            write_private_json(args.normalized_output, normalized)
            self.send_json(
                400,
                {
                    "error": {
                        "message": "deterministic capture complete",
                        "type": "invalid_request_error",
                        "param": None,
                        "code": "capture_complete",
                    }
                },
            )
            server.captured = True

    server = HTTPServer((args.host, args.port), Handler)
    server.captured = False
    server.timeout = 30
    print(f"capture listener ready on http://{args.host}:{args.port}", flush=True)
    while not server.captured:
        server.handle_request()
    server.server_close()
    print(f"raw request: {args.raw_output}", flush=True)
    print(f"normalized request: {args.normalized_output}", flush=True)


if __name__ == "__main__":
    main()
