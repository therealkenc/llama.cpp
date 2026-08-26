#!/usr/bin/env python3
"""Unit tests for the development Responses redacting proxy."""

from __future__ import annotations

import gzip
import http.client
import importlib.util
import io
import json
import sys
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit


MODULE_PATH = Path(__file__).with_name("redacting-responses-proxy.py")
ORACLE_PATH = Path(__file__).with_name("openai-client-tool-search-2026-08-26.json")
SPEC = importlib.util.spec_from_file_location("redacting_responses_proxy", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot import {MODULE_PATH}")
PROXY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = PROXY
SPEC.loader.exec_module(PROXY)


CLIENT_TOKEN = "ephemeral-client-token-for-tests"
UPSTREAM_KEY = "upstream-api-key-for-tests"
REQUEST_SECRETS = (
    "SECRET_INSTRUCTIONS",
    "SECRET_USER_TEXT",
    "SECRET_TOOL_DESCRIPTION",
    "SECRET_TOOL_NAME",
    "SECRET_SCHEMA_PROPERTY",
    "SECRET_SELECTED_TOOL_OUTPUT",
    "secret_query_value",
    "SECRET_QUERY_NAME",
)
RESPONSE_SECRETS = (
    "SECRET_MODEL_ARGUMENT",
    "SECRET_MODEL_TEXT",
)


class FakeUpstream(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self) -> None:
        self.requests: list[dict[str, Any]] = []
        self.last_response_payload = b""
        self.lock = threading.Lock()
        super().__init__(("127.0.0.1", 0), FakeUpstreamHandler)


class FakeUpstreamHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server: FakeUpstream

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        with self.server.lock:
            self.server.requests.append(
                {
                    "method": self.command,
                    "path": self.path,
                    "authorization": self.headers.get("Authorization"),
                    "accept_encoding": self.headers.get("Accept-Encoding"),
                    "body": body,
                }
            )

        events = [
            {
                "type": "response.created",
                "response": {"status": "in_progress", "output": []},
            },
            {
                "type": "response.output_item.added",
                "item": {
                    "type": "tool_search_call",
                    "execution": "client",
                    "call_id": "call_fake",
                    "status": "in_progress",
                    "arguments": {"query": "SECRET_MODEL_ARGUMENT"},
                },
            },
            {
                "type": "response.output_item.done",
                "item": {
                    "type": "tool_search_call",
                    "execution": "client",
                    "call_id": "call_fake",
                    "status": "completed",
                    "arguments": {"query": "SECRET_MODEL_ARGUMENT"},
                },
            },
            {
                "type": "response.completed",
                "response": {
                    "status": "completed",
                    "output": [
                        {
                            "type": "message",
                            "status": "completed",
                            "content": [
                                {"type": "output_text", "text": "SECRET_MODEL_TEXT"}
                            ],
                        }
                    ],
                    "usage": {
                        "input_tokens": 16000,
                        "output_tokens": 17,
                        "total_tokens": 16017,
                        "input_tokens_details": {"cached_tokens": 1000},
                    },
                },
            },
        ]
        payloads = [
            f"event: {event['type']}\ndata: {json.dumps(event)}\n\n".encode("utf-8")
            for event in events
        ]
        payloads.append(b"data: [DONE]\n\n")
        self.server.last_response_payload = b"".join(payloads)

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("X-Request-ID", "req_fake_123")
        self.end_headers()
        for payload in payloads:
            self.wfile.write(f"{len(payload):X}\r\n".encode("ascii"))
            self.wfile.write(payload)
            self.wfile.write(b"\r\n")
            self.wfile.flush()
            time.sleep(0.002)
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def log_message(self, format: str, *args: Any) -> None:
        return


class ProxyHarness:
    def __init__(self) -> None:
        self.upstream = FakeUpstream()
        self.upstream_thread = threading.Thread(
            target=self.upstream.serve_forever, daemon=True
        )
        self.upstream_thread.start()

        self.log = io.StringIO()
        upstream_url = f"http://127.0.0.1:{self.upstream.server_port}/v1"
        config = PROXY.ProxyConfig(
            upstream=urlsplit(upstream_url),
            upstream_key=UPSTREAM_KEY,
            client_token=CLIENT_TOKEN,
            sink=PROXY.JsonlSink(self.log),
        )
        self.proxy = PROXY.RedactingProxyServer(("127.0.0.1", 0), config)
        self.proxy_thread = threading.Thread(
            target=self.proxy.serve_forever, daemon=True
        )
        self.proxy_thread.start()

    def close(self) -> None:
        self.proxy.shutdown()
        self.proxy.server_close()
        self.proxy_thread.join(timeout=2)
        self.upstream.shutdown()
        self.upstream.server_close()
        self.upstream_thread.join(timeout=2)

    def summaries(self) -> list[dict[str, Any]]:
        for _ in range(100):
            lines = self.log.getvalue().splitlines()
            if lines:
                return [json.loads(line) for line in lines]
            time.sleep(0.005)
        return []


class RedactingProxyTest(unittest.TestCase):
    def setUp(self) -> None:
        self.harness = ProxyHarness()

    def tearDown(self) -> None:
        self.harness.close()

    def test_stream_is_forwarded_and_only_structure_is_logged(self) -> None:
        request = {
            "model": "gpt-5.6-sol",
            "stream": True,
            "background": False,
            "store": False,
            "parallel_tool_calls": True,
            "tool_choice": "auto",
            "reasoning": {"effort": "high", "summary": "auto"},
            "text": {"verbosity": "medium"},
            "instructions": "SECRET_INSTRUCTIONS",
            "input": [
                {
                    "type": "additional_tools",
                    "role": "developer",
                    "tools": [
                        {
                            "type": "namespace",
                            "name": "SECRET_TOOL_NAME",
                            "description": "SECRET_TOOL_DESCRIPTION",
                            "defer_loading": True,
                            "tools": [
                                {
                                    "type": "function",
                                    "name": "SECRET_TOOL_NAME",
                                    "description": "SECRET_TOOL_DESCRIPTION",
                                }
                            ],
                        },
                        {
                            "type": "tool_search",
                            "execution": "client",
                            "description": "SECRET_TOOL_DESCRIPTION",
                        },
                    ],
                },
                {
                    "type": "message",
                    "role": "user",
                    "content": [{"type": "input_text", "text": "SECRET_USER_TEXT"}],
                },
                {
                    "type": "tool_search_output",
                    "execution": "client",
                    "call_id": "call_previous",
                    "status": "completed",
                    "tools": [
                        {
                            "type": "function",
                            "name": "SECRET_TOOL_NAME",
                            "description": "SECRET_TOOL_DESCRIPTION",
                            "parameters": {
                                "type": "object",
                                "properties": {
                                    "SECRET_SCHEMA_PROPERTY": {"type": "string"}
                                },
                            },
                        }
                    ],
                    "output": "SECRET_SELECTED_TOOL_OUTPUT",
                },
            ],
            "tools": [
                {
                    "type": "tool_search",
                    "execution": "client",
                    "description": "SECRET_TOOL_DESCRIPTION",
                },
                {
                    "type": "namespace",
                    "name": "SECRET_TOOL_NAME",
                    "description": "SECRET_TOOL_DESCRIPTION",
                    "defer_loading": True,
                    "tools": [
                        {
                            "type": "function",
                            "name": "SECRET_TOOL_NAME",
                            "description": "SECRET_TOOL_DESCRIPTION",
                        }
                    ],
                },
            ],
        }
        encoded = gzip.compress(json.dumps(request).encode("utf-8"))

        connection = http.client.HTTPConnection(
            "127.0.0.1", self.harness.proxy.server_port, timeout=5
        )
        connection.request(
            "POST",
            "/v1/responses?include=secret_query_value&SECRET_QUERY_NAME=value",
            body=encoded,
            headers={
                "Authorization": f"Bearer {CLIENT_TOKEN}",
                "Content-Type": "application/json",
                "Content-Encoding": "gzip",
            },
        )
        response = connection.getresponse()
        response_body = response.read()
        connection.close()

        self.assertEqual(response.status, 200)
        self.assertEqual(response_body, self.harness.upstream.last_response_payload)

        self.assertEqual(len(self.harness.upstream.requests), 1)
        forwarded = self.harness.upstream.requests[0]
        self.assertEqual(
            forwarded["path"],
            "/v1/responses?include=secret_query_value&SECRET_QUERY_NAME=value",
        )
        self.assertEqual(forwarded["authorization"], f"Bearer {UPSTREAM_KEY}")
        self.assertEqual(forwarded["accept_encoding"], "identity")
        self.assertEqual(forwarded["body"], encoded)

        summaries = self.harness.summaries()
        self.assertEqual(len(summaries), 1)
        summary = summaries[0]
        serialized = json.dumps(summary, sort_keys=True)
        for secret in (*REQUEST_SECRETS, *RESPONSE_SECRETS, CLIENT_TOKEN, UPSTREAM_KEY):
            self.assertNotIn(secret, serialized)

        self.assertEqual(summary["schema"], PROXY.SUMMARY_SCHEMA)
        self.assertEqual(summary["request"]["method"], "POST")
        self.assertEqual(summary["request"]["path"], "/v1/responses")
        self.assertEqual(
            summary["request"]["query_parameter_names"], ["<other>", "include"]
        )
        self.assertEqual(summary["request"]["body_bytes"], len(encoded))
        self.assertEqual(summary["request"]["json"]["model"], "gpt-5.6-sol")
        self.assertTrue(summary["request"]["json"]["modes"]["stream"])
        self.assertEqual(
            summary["request"]["json"]["tools"]["types"]["counts"],
            {"namespace": 1, "tool_search": 1},
        )
        self.assertEqual(
            summary["request"]["json"]["input"]["selected_tool_types"]["counts"],
            {"function": 1},
        )
        self.assertEqual(
            summary["request"]["json"]["input"]["declared_tool_types"]["counts"],
            {"namespace": 1, "tool_search": 1},
        )
        self.assertEqual(
            summary["request"]["json"]["input"]["declared_nested_tool_types"]["counts"],
            {"function": 1},
        )
        self.assertEqual(
            summary["request"]["json"]["input"]["declared_execution_modes"]["counts"],
            {"client": 1},
        )
        self.assertEqual(
            summary["request"]["json"]["input"]["declared_defer_loading"],
            {"false": 0, "true": 1},
        )
        self.assertEqual(summary["upstream"]["request_id"], "req_fake_123")
        self.assertEqual(summary["upstream"]["status_code"], 200)
        self.assertEqual(
            summary["response"]["event_types"]["counts"],
            {
                "response.completed": 1,
                "response.created": 1,
                "response.output_item.added": 1,
                "response.output_item.done": 1,
            },
        )
        self.assertEqual(
            summary["response"]["execution_modes"]["counts"], {"client": 2}
        )
        self.assertEqual(
            summary["response"]["call_id_nullness"]["counts"], {"non_null": 2}
        )
        self.assertEqual(summary["response"]["usage"]["total_tokens"], 16017)
        self.assertIsNotNone(summary["timing_ms"]["first_body_byte"])

    def test_wrong_client_token_is_rejected_without_contacting_upstream(self) -> None:
        connection = http.client.HTTPConnection(
            "127.0.0.1", self.harness.proxy.server_port, timeout=5
        )
        connection.request(
            "POST",
            "/v1/responses",
            body=b"{}",
            headers={
                "Authorization": "Bearer wrong-token",
                "Content-Type": "application/json",
            },
        )
        response = connection.getresponse()
        response.read()
        connection.close()

        self.assertEqual(response.status, 401)
        self.assertEqual(self.harness.upstream.requests, [])
        self.assertEqual(self.harness.summaries(), [])

    def test_non_api_route_is_rejected(self) -> None:
        connection = http.client.HTTPConnection(
            "127.0.0.1", self.harness.proxy.server_port, timeout=5
        )
        connection.request(
            "GET",
            "/not-an-api-route?token=SECRET_USER_TEXT",
            headers={"Authorization": f"Bearer {CLIENT_TOKEN}"},
        )
        response = connection.getresponse()
        response.read()
        connection.close()

        self.assertEqual(response.status, 404)
        self.assertEqual(self.harness.upstream.requests, [])
        self.assertEqual(self.harness.summaries(), [])


class OracleFixtureTest(unittest.TestCase):
    def test_tool_search_oracle_is_structural_and_content_free(self) -> None:
        fixture = json.loads(ORACLE_PATH.read_text(encoding="utf-8"))

        self.assertEqual(
            fixture["fixture_schema"],
            "llama.responses.openai-client-tool-search-structural.v1",
        )
        provenance = fixture["provenance"]
        self.assertFalse(provenance["content_retained"])
        self.assertFalse(provenance["credentials_retained"])
        self.assertFalse(provenance["request_ids_retained"])

        non_stream_output = fixture["non_stream"]["response"]["output"]
        self.assertEqual(len(non_stream_output), 1)
        self.assertEqual(non_stream_output[0]["type"], "tool_search_call")
        self.assertEqual(non_stream_output[0]["execution"], "client")
        self.assertEqual(non_stream_output[0]["arguments_kind"], "object")

        events = fixture["stream"]["events"]
        self.assertEqual(
            [event["type"] for event in events],
            [
                "response.created",
                "response.in_progress",
                "response.output_item.added",
                "response.output_item.done",
                "response.completed",
            ],
        )
        self.assertEqual(
            [event["sequence_number"] for event in events], list(range(len(events)))
        )

        continuation = fixture["client_output_continuation"]
        search_output = continuation["request"]["tool_search_output"]
        self.assertEqual(continuation["http_status"], 200)
        self.assertEqual(search_output["execution"], "client")
        self.assertEqual(search_output["status"], "completed")
        self.assertEqual(search_output["call_id_kind"], "string")
        self.assertTrue(search_output["call_id_matches_tool_search_call"])
        self.assertFalse(search_output["id_present"])
        self.assertEqual(search_output["tools_kind"], "array")
        self.assertEqual(search_output["selected_tool_count"], 1)
        self.assertEqual(
            continuation["response"]["active_tool_count_after"],
            continuation["response"]["active_tool_count_before"] + 1,
        )
        self.assertTrue(
            continuation["response"]["selected_definition_present_in_response_tools"]
        )
        self.assertEqual(continuation["accepted_variant"]["omitted_field"], "status")
        self.assertEqual(continuation["accepted_variant"]["http_status"], 200)

        def collect_keys(value: Any) -> set[str]:
            if isinstance(value, dict):
                return set(value).union(
                    *(collect_keys(element) for element in value.values())
                )
            if isinstance(value, list):
                return set().union(*(collect_keys(element) for element in value))
            return set()

        retained_content_fields = {
            "arguments",
            "authorization",
            "call_id",
            "description",
            "id",
            "input",
            "instructions",
            "parameters",
            "prompt",
            "request_id",
            "text",
        }
        self.assertTrue(retained_content_fields.isdisjoint(collect_keys(fixture)))


class ConfigurationSafetyTest(unittest.TestCase):
    def test_refuses_default_codex_home(self) -> None:
        with self.assertRaisesRegex(ValueError, "isolated CODEX_HOME"):
            PROXY._validate_codex_home(str(Path.home() / ".codex"))

    def test_refuses_non_loopback_http_upstream(self) -> None:
        with self.assertRaisesRegex(ValueError, "loopback"):
            PROXY._validate_upstream("http://example.com/v1", allow_http=True)

    def test_requires_explicit_https_without_test_override(self) -> None:
        with self.assertRaisesRegex(ValueError, "HTTPS"):
            PROXY._validate_upstream("http://127.0.0.1:8080/v1", allow_http=False)

    def test_refuses_non_loopback_bind_by_default(self) -> None:
        with self.assertRaisesRegex(ValueError, "loopback"):
            PROXY._validate_bind("0.0.0.0")


if __name__ == "__main__":
    unittest.main()
