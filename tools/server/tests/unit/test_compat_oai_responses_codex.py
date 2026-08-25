"""Codex-critical OpenAI Responses conformance fixtures and HTTP checks.

The tiny model used by the server suite cannot deterministically produce tool
calls.  Exact tool-call streams therefore live here as SDK-validated golden
transcripts, while the HTTP tests exercise the replay/continuation direction
against llama-server.  Scripted generation tests in ``tools/llama-responses``
should reuse the invariants below when that seam is available.
"""

from __future__ import annotations

import asyncio
import json
import os
import sqlite3
from collections import defaultdict
from typing import Any
from urllib.parse import quote

import pytest
from openai import AsyncOpenAI, BadRequestError, NotFoundError, OpenAI
from openai.types.responses import ResponseStreamEvent
from pydantic import TypeAdapter

from utils import ServerPreset


server = ServerPreset.tinyllama2()
STREAM_EVENT_ADAPTER = TypeAdapter(ResponseStreamEvent)
TEST_API_KEY = "sk-llama-responses-test"


@pytest.fixture(autouse=True)
def create_server(tmp_path, monkeypatch):
    global server
    monkeypatch.setenv("LLAMA_RESPONSES_DB", str(tmp_path / "responses.sqlite3"))
    server = ServerPreset.tinyllama2()
    # Port 8080 is commonly occupied by local MCP/HTTP development services.
    # Keep this focused suite isolated from both those services and the live
    # Qwen profile on 8081.
    server.server_port = 18088
    server.api_key = TEST_API_KEY


def authenticated_request(method: str, path: str, **kwargs: Any) -> Any:
    headers = dict(kwargs.pop("headers", {}))
    headers["Authorization"] = f"Bearer {TEST_API_KEY}"
    return server.make_request(method, path, headers=headers, **kwargs)


def openai_client() -> OpenAI:
    global server
    server.start()
    return OpenAI(
        api_key=TEST_API_KEY,
        base_url=f"http://{server.server_host}:{server.server_port}/v1",
    )


def assert_foreground_terminal(response: Any) -> None:
    """Accept either ordinary EOS completion or an honest token-limit stop."""
    assert response.status in {"completed", "incomplete"}
    if response.status == "incomplete":
        assert response.completed_at is None
        assert response.incomplete_details is not None
        assert response.incomplete_details.reason == "max_output_tokens"


def test_codex_model_catalog_route_chain():
    global server
    server.start()

    unauthenticated = server.make_request(
        "GET", "/v1/models?client_version=0.148.0"
    )
    assert unauthenticated.status_code == 401

    plain_v1 = authenticated_request("GET", "/v1/models")
    unrelated_v1 = authenticated_request("GET", "/v1/models?unrelated=value")
    public_models = authenticated_request(
        "GET", "/models?client_version=0.148.0"
    )

    for response in (plain_v1, unrelated_v1, public_models):
        assert response.status_code == 200
        assert response.body["object"] == "list"
        assert response.body["data"][0]["id"] == server.model_alias
        assert "slug" not in response.body["models"][0]

    for version in (
        "0.148.0",
        "0.149.0-alpha.4.3",
        "1.148.0",
        "99.0.0",
        "opaque-preview",
    ):
        response = authenticated_request(
            "GET", f"/v1/models?client_version={version}"
        )
        assert response.status_code == 200
        assert set(response.body) == {"models"}
        assert len(response.body["models"]) == 1
        model = response.body["models"][0]
        assert model["slug"] == server.model_alias
        assert model["shell_type"] == "unified_exec"
        assert model["apply_patch_tool_type"] == "freeform"
        assert model["supported_reasoning_levels"] == []
        assert "default_reasoning_level" not in model
        assert model["context_window"] == server.n_ctx // server.n_slots
        assert model["max_context_window"] == model["context_window"]
        assert model["input_modalities"] == ["text"]
        instructions = model["base_instructions"]
        assert len(instructions) > 1_000
        assert instructions.startswith("You are a coding agent running in the Codex CLI")
        assert "AGENTS.md" in instructions
        assert "apply_patch" in instructions


def response_snapshot(
    response_id: str,
    output: list[dict[str, Any]],
    *,
    status: str = "completed",
    error: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Build the common Response envelope required by openai-python 2.14."""
    return {
        "id": response_id,
        "object": "response",
        "created_at": 1.0,
        "completed_at": 2.0 if status == "completed" else None,
        "status": status,
        "error": error,
        "incomplete_details": None,
        "instructions": None,
        "metadata": {},
        "model": "gpt-5.5",
        "output": output,
        "parallel_tool_calls": True,
        "previous_response_id": None,
        "reasoning": None,
        "store": False,
        "temperature": None,
        "text": {"format": {"type": "text"}},
        "tool_choice": "auto",
        "tools": [],
        "top_p": None,
        "truncation": "disabled",
        "usage": None,
    }


def event(sequence_number: int, event_type: str, **fields: Any) -> dict[str, Any]:
    return {
        "type": event_type,
        "sequence_number": sequence_number,
        **fields,
    }


def assert_sdk_valid_events(events: list[dict[str, Any]]) -> None:
    """Require SDK-decodable events and one gap-free sequence stream."""
    assert events
    assert [item["sequence_number"] for item in events] == list(range(len(events)))
    for item in events:
        STREAM_EVENT_ADAPTER.validate_python(item)


def assert_call_identity_contract(events: list[dict[str, Any]]) -> None:
    """Check item identity, correlation identity, arguments, and indices."""
    added: dict[str, tuple[int, dict[str, Any]]] = {}
    deltas: dict[str, str] = defaultdict(str)
    arguments_done: dict[str, str] = {}
    item_done: dict[str, tuple[int, dict[str, Any]]] = {}

    response_ids = []
    for item in events:
        event_type = item["type"]
        if "response" in item:
            response_ids.append(item["response"]["id"])
        if event_type == "response.output_item.added":
            output_item = item["item"]
            if output_item["type"] in {"function_call", "custom_tool_call"}:
                item_id = output_item["id"]
                assert item_id not in added
                assert output_item["call_id"].startswith("call_")
                assert item_id != output_item["call_id"]
                added[item_id] = (item["output_index"], output_item)
        elif event_type in {
            "response.function_call_arguments.delta",
            "response.custom_tool_call_input.delta",
        }:
            deltas[item["item_id"]] += item["delta"]
            assert item["output_index"] == added[item["item_id"]][0]
        elif event_type in {
            "response.function_call_arguments.done",
            "response.custom_tool_call_input.done",
        }:
            value_key = "arguments" if "arguments" in item else "input"
            arguments_done[item["item_id"]] = item[value_key]
            assert item["output_index"] == added[item["item_id"]][0]
        elif event_type == "response.output_item.done":
            output_item = item["item"]
            if output_item["type"] in {"function_call", "custom_tool_call"}:
                item_id = output_item["id"]
                item_done[item_id] = (item["output_index"], output_item)

    assert len(set(response_ids)) == 1
    assert added.keys() == item_done.keys() == arguments_done.keys()
    output_indices = [output_index for output_index, _ in added.values()]
    assert sorted(output_indices) == list(range(len(added)))
    call_ids = [output_item["call_id"] for _, output_item in added.values()]
    assert len(set(call_ids)) == len(call_ids)
    for item_id, (output_index, added_item) in added.items():
        done_index, done_item = item_done[item_id]
        value_key = "arguments" if done_item["type"] == "function_call" else "input"
        expected_prefix = "fc_" if done_item["type"] == "function_call" else "ctc_"
        assert item_id.startswith(expected_prefix)
        assert done_index == output_index
        assert done_item["id"] == added_item["id"]
        assert done_item["call_id"] == added_item["call_id"]
        assert deltas[item_id] == arguments_done[item_id] == done_item[value_key]

    completed = events[-1]
    assert completed["type"] == "response.completed"
    final_output = [
        output_item
        for output_item in completed["response"]["output"]
        if output_item["type"] in {"function_call", "custom_tool_call"}
    ]
    expected_order = [
        item_id
        for item_id, _ in sorted(added.items(), key=lambda entry: entry[1][0])
    ]
    assert [output_item["id"] for output_item in final_output] == expected_order
    final_items = {
        output_item["id"]: output_item
        for output_item in final_output
    }
    assert final_items.keys() == item_done.keys()
    for item_id, (_, done_item) in item_done.items():
        assert final_items[item_id] == done_item


def test_function_call_golden_stream_has_independent_stable_ids():
    response_id = "resp_function_fixture"
    item_id = "fc_function_fixture"
    call_id = "call_function_fixture"
    name = "exec_command"
    arguments = '{"cmd":"pwd"}'
    in_progress = {
        "id": item_id,
        "type": "function_call",
        "status": "in_progress",
        "call_id": call_id,
        "name": name,
        "arguments": "",
    }
    completed = {**in_progress, "status": "completed", "arguments": arguments}

    events = [
        event(0, "response.created", response=response_snapshot(response_id, [], status="in_progress")),
        event(1, "response.in_progress", response=response_snapshot(response_id, [], status="in_progress")),
        event(2, "response.output_item.added", output_index=0, item=in_progress),
        event(3, "response.function_call_arguments.delta", output_index=0, item_id=item_id, delta='{"cmd":'),
        event(4, "response.function_call_arguments.delta", output_index=0, item_id=item_id, delta='"pwd"}'),
        event(
            5,
            "response.function_call_arguments.done",
            output_index=0,
            item_id=item_id,
            name=name,
            arguments=arguments,
        ),
        event(6, "response.output_item.done", output_index=0, item=completed),
        event(7, "response.completed", response=response_snapshot(response_id, [completed])),
    ]

    assert_sdk_valid_events(events)
    assert_call_identity_contract(events)


def test_parallel_function_call_golden_stream_tracks_interleaved_calls():
    response_id = "resp_parallel_fixture"
    call_specs = [
        ("fc_parallel_a", "call_parallel_a", "exec_command", '{"cmd":"pwd"}'),
        ("fc_parallel_b", "call_parallel_b", "update_plan", '{"plan":[]}'),
    ]
    starts = [
        {
            "id": item_id,
            "type": "function_call",
            "status": "in_progress",
            "call_id": call_id,
            "name": name,
            "arguments": "",
        }
        for item_id, call_id, name, _ in call_specs
    ]
    completed = [
        {**start, "status": "completed", "arguments": spec[3]}
        for start, spec in zip(starts, call_specs)
    ]

    events = [
        event(0, "response.created", response=response_snapshot(response_id, [], status="in_progress")),
        event(1, "response.in_progress", response=response_snapshot(response_id, [], status="in_progress")),
        event(2, "response.output_item.added", output_index=0, item=starts[0]),
        event(3, "response.function_call_arguments.delta", output_index=0, item_id=call_specs[0][0], delta='{"cmd":'),
        event(4, "response.output_item.added", output_index=1, item=starts[1]),
        event(5, "response.function_call_arguments.delta", output_index=1, item_id=call_specs[1][0], delta='{"plan":'),
        event(6, "response.function_call_arguments.delta", output_index=0, item_id=call_specs[0][0], delta='"pwd"}'),
        event(
            7,
            "response.function_call_arguments.done",
            output_index=0,
            item_id=call_specs[0][0],
            name=call_specs[0][2],
            arguments=call_specs[0][3],
        ),
        event(8, "response.output_item.done", output_index=0, item=completed[0]),
        event(9, "response.function_call_arguments.delta", output_index=1, item_id=call_specs[1][0], delta="[]}"),
        event(
            10,
            "response.function_call_arguments.done",
            output_index=1,
            item_id=call_specs[1][0],
            name=call_specs[1][2],
            arguments=call_specs[1][3],
        ),
        event(11, "response.output_item.done", output_index=1, item=completed[1]),
        event(12, "response.completed", response=response_snapshot(response_id, completed)),
    ]

    assert_sdk_valid_events(events)
    assert_call_identity_contract(events)


def test_apply_patch_custom_call_golden_stream_preserves_raw_input_and_ids():
    response_id = "resp_custom_fixture"
    item_id = "ctc_apply_patch_fixture"
    call_id = "call_apply_patch_fixture"
    patch = "*** Begin Patch\n*** Add File: fixture.txt\n+ok\n*** End Patch\n"
    in_progress = {
        "id": item_id,
        "type": "custom_tool_call",
        "status": "in_progress",
        "call_id": call_id,
        "name": "apply_patch",
        "input": "",
    }
    completed = {**in_progress, "status": "completed", "input": patch}

    events = [
        event(0, "response.created", response=response_snapshot(response_id, [], status="in_progress")),
        event(1, "response.in_progress", response=response_snapshot(response_id, [], status="in_progress")),
        event(2, "response.output_item.added", output_index=0, item=in_progress),
        event(
            3,
            "response.custom_tool_call_input.delta",
            output_index=0,
            item_id=item_id,
            delta=patch[:24],
        ),
        event(
            4,
            "response.custom_tool_call_input.delta",
            output_index=0,
            item_id=item_id,
            delta=patch[24:],
        ),
        event(
            5,
            "response.custom_tool_call_input.done",
            output_index=0,
            item_id=item_id,
            input=patch,
        ),
        event(6, "response.output_item.done", output_index=0, item=completed),
        event(7, "response.completed", response=response_snapshot(response_id, [completed])),
    ]

    assert_sdk_valid_events(events)
    assert_call_identity_contract(events)


def test_failed_response_golden_stream_keeps_response_identity_and_error_shape():
    response_id = "resp_failed_fixture"
    error_body = {
        "code": "invalid_prompt",
        "message": "Tool arguments were not valid JSON.",
    }
    failed_response = response_snapshot(
        response_id,
        [],
        status="failed",
        error=error_body,
    )
    events = [
        event(0, "response.created", response=response_snapshot(response_id, [], status="in_progress")),
        event(1, "response.in_progress", response=response_snapshot(response_id, [], status="in_progress")),
        event(
            2,
            "error",
            code=error_body["code"],
            message=error_body["message"],
            param="input[1].arguments",
        ),
        event(3, "response.failed", response=failed_response),
    ]

    assert_sdk_valid_events(events)
    assert len({item["response"]["id"] for item in events if "response" in item}) == 1
    assert events[-1]["response"]["status"] == "failed"
    assert events[-1]["response"]["error"] == error_body


def test_codex_replayed_function_and_custom_tool_outputs_round_trip_through_sdk():
    client = openai_client()
    function_call_id = "call_replayed_function"
    custom_call_id = "call_replayed_custom"

    response = client.responses.create(
        model="tinyllama-2",
        input=[
            {
                "type": "message",
                "id": "msg_replayed_user",
                "role": "user",
                "content": [{"type": "input_text", "text": "Run the checks."}],
            },
            {
                "type": "function_call",
                "id": "fc_replayed_function",
                "status": "completed",
                "call_id": function_call_id,
                "name": "exec_command",
                "arguments": '{"cmd":"pwd"}',
            },
            {
                "type": "function_call_output",
                "id": "fco_replayed_function",
                "call_id": function_call_id,
                "output": "/tmp/fixture\n",
            },
            {
                "type": "custom_tool_call",
                "id": "ctc_replayed_custom",
                "status": "completed",
                "call_id": custom_call_id,
                "name": "apply_patch",
                "input": "*** Begin Patch\n*** End Patch\n",
            },
            {
                "type": "custom_tool_call_output",
                "id": "ctco_replayed_custom",
                "call_id": custom_call_id,
                "output": "Done!",
            },
            {
                "type": "message",
                "id": "msg_replayed_followup",
                "role": "user",
                "content": [{"type": "input_text", "text": "Summarize the result."}],
            },
        ],
        max_output_tokens=16,
        temperature=0.0,
    )

    assert_foreground_terminal(response)
    assert response.id.startswith("resp_")
    assert response.output_text


@pytest.mark.parametrize(
    "body, message_fragment, code, param",
    [
        ({"model": "tinyllama-2"}, "'input' is required", "invalid_request", None),
        (
            {"model": "tinyllama-2", "input": 17},
            "'input' must be",
            "invalid_request",
            None,
        ),
        (
            {"model": "tinyllama-2", "input": None},
            "'input' must be",
            "invalid_request",
            None,
        ),
        (
            {"model": "tinyllama-2", "input": "hello", "store": "true"},
            "Invalid type for 'store'",
            "invalid_type",
            "store",
        ),
        (
            {"model": "tinyllama-2", "input": "hello", "conversation": "conv_missing"},
            "Conversation resources are not available",
            "unsupported_parameter",
            "conversation",
        ),
        (
            {"model": "tinyllama-2", "input": "hello", "background": True},
            "Background responses are not available",
            "unsupported_parameter",
            "background",
        ),
        (
            {"model": "tinyllama-2", "input": "hello", "instructions": {}},
            "Invalid type for 'instructions'",
            "invalid_type",
            "instructions",
        ),
        (
            {
                "model": "tinyllama-2",
                "input": [{"type": "item_reference", "id": "missing_item_fixture"}],
            },
            "item_reference_not_found",
            "invalid_request",
            None,
        ),
    ],
)
def test_responses_validation_errors_have_openai_shape(
    body, message_fragment, code, param
):
    global server
    server.start()
    response = authenticated_request("POST", "/v1/responses", data=body)

    assert response.status_code == 400
    assert set(response.body) == {"error"}
    error = response.body["error"]
    assert error["code"] == code
    assert error["type"] == "invalid_request_error"
    assert error["param"] == param
    assert message_fragment in error["message"]


def test_openai_sdk_surfaces_responses_validation_error():
    client = openai_client()

    with pytest.raises(BadRequestError) as exc_info:
        client.responses.create(model="tinyllama-2")

    assert exc_info.value.status_code == 400
    assert exc_info.value.body["code"] == "invalid_request"
    assert exc_info.value.body["type"] == "invalid_request_error"
    assert "'input' is required" in exc_info.value.body["message"]


def test_max_output_tokens_matches_openai_minimum_error():
    global server
    server.start()
    response = authenticated_request(
        "POST",
        "/v1/responses",
        data={
            "model": "tinyllama-2",
            "input": "hello",
            "max_output_tokens": 1,
        },
    )

    assert response.status_code == 400
    assert response.body["error"]["code"] == "integer_below_min_value"
    assert response.body["error"]["type"] == "invalid_request_error"
    assert response.body["error"]["param"] == "max_output_tokens"


def test_top_logprobs_zero_is_a_responses_default_not_chat_logprobs():
    client = openai_client()
    response = client.responses.create(
        model="tinyllama-2",
        input="hello",
        max_output_tokens=16,
        top_logprobs=0,
    )

    assert_foreground_terminal(response)
    assert response.top_logprobs == 0


@pytest.mark.parametrize(
    "extra, code, param",
    [
        ({"mystery": True}, "unknown_parameter", "mystery"),
        ({"client_metadata": []}, "invalid_type", "client_metadata"),
        (
            {"client_metadata": {"codex_version": 148}},
            "invalid_type",
            "client_metadata.codex_version",
        ),
        ({"tools": {}}, "invalid_type", "tools"),
        ({"tools": [1]}, "invalid_type", "tools[0]"),
        (
            {"tools": [{}]},
            "missing_required_parameter",
            "tools[0].type",
        ),
        ({"tools": [{"type": 1}]}, "invalid_type", "tools[0].type"),
        (
            {"tools": [{"type": "function"}]},
            "missing_required_parameter",
            "tools[0].name",
        ),
        (
            {"tools": [{"type": "function", "name": 1}]},
            "invalid_type",
            "tools[0].name",
        ),
        (
            {
                "tools": [
                    {"type": "function", "name": "fixture", "parameters": 1}
                ]
            },
            "invalid_type",
            "tools[0].parameters",
        ),
        (
            {
                "tools": [
                    {"type": "function", "name": "fixture", "strict": "yes"}
                ]
            },
            "invalid_type",
            "tools[0].strict",
        ),
        (
            {
                "tools": [
                    {
                        "type": "function",
                        "name": "fixture",
                        "defer_loading": True,
                    }
                ]
            },
            "unsupported_parameter",
            "tools[0].defer_loading",
        ),
        (
            {
                "tools": [
                    {"type": "function", "name": "duplicate"},
                    {"type": "custom", "name": "duplicate"},
                ]
            },
            "invalid_value",
            "tools[1].name",
        ),
        (
            {
                "tools": [
                    {"type": "function", "name": "local_shell"},
                    {"type": "local_shell"},
                ]
            },
            "invalid_value",
            "tools[1].type",
        ),
        (
            {"tools": [{"type": "custom", "name": "fixture", "format": {}}]},
            "missing_required_parameter",
            "tools[0].format.type",
        ),
        (
            {"tools": [{"type": "namespace", "name": "mcp_fixture"}]},
            "missing_required_parameter",
            "tools[0].tools",
        ),
        (
            {
                "tools": [
                    {"type": "namespace", "name": "mcp_fixture", "tools": {}}
                ]
            },
            "invalid_type",
            "tools[0].tools",
        ),
        (
            {
                "tools": [
                    {"type": "namespace", "name": "mcp_fixture", "tools": [1]}
                ]
            },
            "invalid_type",
            "tools[0].tools[0]",
        ),
        (
            {
                "tools": [
                    {
                        "type": "namespace",
                        "name": "mcp_fixture",
                        "tools": [{"type": "local_shell"}],
                    }
                ]
            },
            "invalid_value",
            "tools[0].tools[0].type",
        ),
        (
            {"tools": [{"type": "web_search"}]},
            "unsupported_parameter",
            "tools[0].type",
        ),
        (
            {"tools": [{"type": "banana"}]},
            "invalid_value",
            "tools[0].type",
        ),
        ({"tool_choice": 1}, "invalid_type", "tool_choice"),
        ({"tool_choice": "any"}, "invalid_value", "tool_choice"),
        ({"tool_choice": "required"}, "invalid_value", "tool_choice"),
        (
            {"tool_choice": {}},
            "missing_required_parameter",
            "tool_choice.type",
        ),
        (
            {"tool_choice": {"type": "function"}},
            "missing_required_parameter",
            "tool_choice.name",
        ),
        (
            {"tool_choice": {"type": "function", "name": "missing"}},
            "invalid_value",
            "tool_choice.name",
        ),
        ({"text": "bad"}, "invalid_type", "text"),
        ({"text": {"format": []}}, "invalid_type", "text.format"),
        (
            {"text": {"format": {}}},
            "missing_required_parameter",
            "text.format.type",
        ),
        (
            {"text": {"format": {"type": 1}}},
            "invalid_value",
            "text.format.type",
        ),
        (
            {"text": {"format": {"type": "banana"}}},
            "invalid_value",
            "text.format.type",
        ),
        (
            {"text": {"format": {"type": "json_schema"}}},
            "missing_required_parameter",
            "text.format.name",
        ),
        (
            {
                "text": {
                    "format": {"type": "json_schema", "name": "fixture"}
                }
            },
            "missing_required_parameter",
            "text.format.schema",
        ),
        (
            {
                "text": {
                    "format": {
                        "type": "json_schema",
                        "name": 1,
                        "schema": {},
                    }
                }
            },
            "invalid_type",
            "text.format.name",
        ),
        (
            {
                "text": {
                    "format": {
                        "type": "json_schema",
                        "name": "fixture",
                        "schema": 1,
                    }
                }
            },
            "invalid_type",
            "text.format.schema",
        ),
        (
            {"text": {"mystery": True}},
            "unknown_parameter",
            "text.mystery",
        ),
        (
            {"text": {"format": {"type": "text", "mystery": True}}},
            "unknown_parameter",
            "text.format.mystery",
        ),
        ({"context_management": {}}, "invalid_type", "context_management"),
        ({"reasoning": "bad"}, "invalid_type", "reasoning"),
        ({"reasoning": {"effort": 1}}, "invalid_type", "reasoning.effort"),
        (
            {"reasoning": {"effort": "banana"}},
            "invalid_value",
            "reasoning.effort",
        ),
        (
            {"reasoning": {"mystery": "x"}},
            "unknown_parameter",
            "reasoning.mystery",
        ),
        (
            {"stream_options": {}},
            None,
            "stream_options",
        ),
        ({"context_management": []}, "empty_array", "context_management"),
        (
            {"stream": True, "stream_options": {"include_obfuscation": "yes"}},
            "invalid_type",
            "stream_options.include_obfuscation",
        ),
        (
            {"stream": True, "stream_options": {"include_usage": True}},
            "unknown_parameter",
            "stream_options.include_usage",
        ),
        ({"stream": "true"}, "invalid_type", "stream"),
        ({"top_logprobs": -1}, "integer_below_min_value", "top_logprobs"),
        (
            {"top_logprobs": 4_294_967_296},
            "integer_above_max_value",
            "top_logprobs",
        ),
    ],
)
def test_create_policy_type_errors_are_parameter_attributed(extra, code, param):
    global server
    server.start()
    body = {
        "model": "tinyllama-2",
        "input": "hello",
        "max_output_tokens": 16,
        **extra,
    }
    response = authenticated_request("POST", "/v1/responses", data=body)

    assert response.status_code == 400
    assert response.body["error"]["type"] == "invalid_request_error"
    assert response.body["error"]["code"] == code
    assert response.body["error"]["param"] == param


@pytest.mark.parametrize("reasoning", [{"effort": "low"}, {"effort": None}])
def test_supported_reasoning_effort_shapes_reach_generation(reasoning):
    global server
    server.start()
    response = authenticated_request(
        "POST",
        "/v1/responses",
        data={
            "model": "tinyllama-2",
            "input": "hello",
            "max_output_tokens": 16,
            "reasoning": reasoning,
        },
    )

    assert response.status_code == 200
    assert response.body["reasoning"] == reasoning


def test_nullable_create_defaults_are_normalized_in_the_response():
    global server
    server.start()
    response = authenticated_request(
        "POST",
        "/v1/responses",
        data={
            "model": "tinyllama-2",
            "input": "hello",
            "max_output_tokens": 16,
            "store": None,
            "background": None,
            "stream": None,
            "metadata": None,
            "text": None,
            "truncation": None,
            "service_tier": None,
            "top_logprobs": None,
            "tools": None,
            "tool_choice": None,
        },
    )

    assert response.status_code == 200
    assert response.body["store"] is True
    assert response.body["background"] is False
    assert response.body["metadata"] == {}
    assert response.body["text"] == {
        "format": {"type": "text"},
        "verbosity": "medium",
    }
    assert response.body["truncation"] == "disabled"
    assert response.body["service_tier"] == "default"
    assert response.body["top_logprobs"] == 0
    assert response.body["tools"] == []
    assert response.body["tool_choice"] == "auto"


@pytest.mark.parametrize("text", [{}, {"format": None}, {"verbosity": None}])
def test_nested_text_defaults_are_normalized(text):
    global server
    server.start()
    response = authenticated_request(
        "POST",
        "/v1/responses",
        data={
            "model": "tinyllama-2",
            "input": "hello",
            "max_output_tokens": 16,
            "text": text,
        },
    )

    assert response.status_code == 200
    assert response.body["text"] == {
        "format": {"type": "text"},
        "verbosity": "medium",
    }


def test_codex_client_metadata_and_client_executed_tools_are_accepted_and_echoed():
    global server
    server.jinja = True
    server.start()
    tools = [
        {
            "type": "function",
            "name": "read_fixture",
            "description": "Read a fixture.",
            "strict": True,
            "defer_loading": None,
            "parameters": {
                "type": "object",
                "properties": {},
                "additionalProperties": False,
            },
        },
        {
            "type": "custom",
            "name": "apply_patch",
            "description": "Apply a patch.",
            "format": {
                "type": "grammar",
                "syntax": "lark",
                "definition": "start: /.+/s",
            },
        },
        {"type": "local_shell"},
        {
            "type": "namespace",
            "name": "mcp_fixture",
            "description": "Fixture connector tools.",
            "tools": [
                {
                    "type": "function",
                    "name": "read_remote_fixture",
                    "description": "Read a remote fixture.",
                    "strict": False,
                    "parameters": {
                        "type": "object",
                        "properties": {},
                        "additionalProperties": False,
                    },
                }
            ],
        },
    ]
    response = authenticated_request(
        "POST",
        "/v1/responses",
        data={
            "model": "tinyllama-2",
            "input": "Reply without calling a tool.",
            "max_output_tokens": 16,
            "client_metadata": {
                "codex_version": "0.148.0",
                "x-codex-turn-metadata": '{"fixture":true}',
            },
            "tools": tools,
            "tool_choice": "none",
        },
    )

    assert response.status_code == 200
    assert response.body["tools"] == tools
    assert response.body["tool_choice"] == "none"
    assert "client_metadata" not in response.body


def test_json_schema_text_format_reaches_the_model_grammar_and_echoes():
    global server
    server.start()
    format_config = {
        "type": "json_schema",
        "name": "answer_fixture",
        "strict": True,
        "schema": {
            "type": "object",
            "properties": {"answer": {"type": "string"}},
            "required": ["answer"],
            "additionalProperties": False,
        },
    }
    response = authenticated_request(
        "POST",
        "/v1/responses",
        data={
            "model": "tinyllama-2",
            "input": "Return a JSON object with a short answer string.",
            "max_output_tokens": 64,
            "temperature": 0.0,
            "text": {"format": format_config},
        },
    )

    assert response.status_code == 200
    assert response.body["text"] == {
        "format": format_config,
        "verbosity": "medium",
    }
    assert isinstance(json.loads(response.body["output_text"])["answer"], str)


def test_create_string_limits_count_unicode_characters_not_utf8_bytes():
    global server
    server.start()
    metadata_key = "é" * 64
    metadata_value = "é" * 512
    response = authenticated_request(
        "POST",
        "/v1/responses",
        data={
            "model": "tinyllama-2",
            "input": "hello",
            "max_output_tokens": 16,
            "metadata": {metadata_key: metadata_value},
            "safety_identifier": "é" * 64,
            "prompt_cache_key": "é" * 64,
        },
    )

    assert response.status_code == 200
    assert response.body["metadata"] == {metadata_key: metadata_value}


@pytest.mark.parametrize(
    "extra, code, param",
    [
        (
            {"metadata": {f"key_{index}": "value" for index in range(17)}},
            "object_above_max_properties",
            "metadata",
        ),
        (
            {"metadata": {"é" * 65: "value"}},
            "property_name_above_max_length",
            "metadata." + "é" * 65,
        ),
        (
            {"metadata": {"key": "é" * 513}},
            "string_above_max_length",
            "metadata.key",
        ),
        (
            {"safety_identifier": "é" * 65},
            "string_above_max_length",
            "safety_identifier",
        ),
        (
            {"prompt_cache_key": "é" * 65},
            "string_above_max_length",
            "prompt_cache_key",
        ),
    ],
)
def test_create_string_limits_reject_one_character_over_the_boundary(
    extra, code, param
):
    global server
    server.start()
    response = authenticated_request(
        "POST",
        "/v1/responses",
        data={
            "model": "tinyllama-2",
            "input": "hello",
            "max_output_tokens": 16,
            **extra,
        },
    )

    assert response.status_code == 400
    assert response.body["error"]["type"] == "invalid_request_error"
    assert response.body["error"]["code"] == code
    assert response.body["error"]["param"] == param


@pytest.mark.parametrize(
    "field, value",
    [
        ("include", ["file_search_call.results"]),
        ("truncation", "auto"),
        ("service_tier", "priority"),
        ("top_logprobs", 1),
        ("prompt", {"id": "pmpt_fixture"}),
        ("moderation", {"model": "omni-moderation-latest"}),
        ("context_management", [{"type": "compaction"}]),
        ("max_tool_calls", 1),
        ("prompt_cache_options", {"mode": "explicit"}),
        ("prompt_cache_retention", "24h"),
        ("text", {"format": {"type": "text"}, "verbosity": "low"}),
        ("reasoning", {"summary": "auto"}),
        ("reasoning", {"generate_summary": "auto"}),
        ("reasoning", {"context": "all_turns"}),
        ("reasoning", {"mode": "standard"}),
        ("personality", "pragmatic"),
    ],
)
def test_recognized_unavailable_create_fields_fail_explicitly(field, value):
    global server
    server.start()
    response = authenticated_request(
        "POST",
        "/v1/responses",
        data={
            "model": "tinyllama-2",
            "input": "hello",
            field: value,
        },
    )

    assert response.status_code == 400
    assert response.body["error"]["code"] == "unsupported_parameter"
    assert response.body["error"]["type"] == "invalid_request_error"
    expected_param = field
    if field == "text":
        expected_param = "text.verbosity"
    elif field == "reasoning" and "summary" in value:
        expected_param = "reasoning.summary"
    elif field == "reasoning" and "generate_summary" in value:
        expected_param = "reasoning.generate_summary"
    elif field == "reasoning" and "context" in value:
        expected_param = "reasoning.context"
    elif field == "reasoning":
        expected_param = "reasoning.mode"
    assert response.body["error"]["param"] == expected_param


def test_stream_obfuscation_fails_explicitly():
    global server
    server.start()
    response = authenticated_request(
        "POST",
        "/v1/responses",
        data={
            "model": "tinyllama-2",
            "input": "hello",
            "stream": True,
            "stream_options": {"include_obfuscation": True},
        },
    )

    assert response.status_code == 400
    assert response.body["error"]["code"] == "unsupported_parameter"
    assert response.body["error"]["type"] == "invalid_request_error"
    assert response.body["error"]["param"] == "stream_options.include_obfuscation"


def test_stored_previous_response_id_continues_and_is_echoed_via_sdk():
    client = openai_client()
    first = client.responses.create(
        model="tinyllama-2",
        input="Remember the word amber.",
        max_output_tokens=16,
        store=True,
    )
    continued = client.responses.create(
        model="tinyllama-2",
        input="What word did I ask you to remember?",
        max_output_tokens=16,
        previous_response_id=first.id,
        store=True,
    )
    standalone = client.responses.create(
        model="tinyllama-2",
        input="What word did I ask you to remember?",
        max_output_tokens=16,
        store=False,
    )
    third = client.responses.create(
        model="tinyllama-2",
        input="Repeat the remembered word once more.",
        max_output_tokens=16,
        previous_response_id=continued.id,
        store=True,
    )

    assert_foreground_terminal(continued)
    assert continued.previous_response_id == first.id
    # Echoing the identifier is insufficient: the stored input/output must be
    # present in the continued prompt.  Token count makes that observable with
    # the deterministic tiny fixture without depending on generated wording.
    assert continued.usage is not None
    assert standalone.usage is not None
    assert continued.usage.input_tokens > standalone.usage.input_tokens
    assert third.usage is not None
    assert third.usage.input_tokens > continued.usage.input_tokens

    continued_items = client.responses.input_items.list(
        continued.id,
        order="asc",
        limit=100,
    )
    assert len(continued_items.data) == 3
    assert continued_items.data[1].id == first.output[0].id


def test_current_input_ids_cannot_collide_with_stored_lineage():
    client = openai_client()
    parent = client.responses.create(
        model="tinyllama-2",
        input=[
            {
                "type": "message",
                "id": "msg_duplicate_lineage_fixture",
                "role": "user",
                "content": [{"type": "input_text", "text": "Remember this."}],
            }
        ],
        max_output_tokens=16,
        store=True,
    )

    response = authenticated_request(
        "POST",
        "/v1/responses",
        data={
            "model": "tinyllama-2",
            "previous_response_id": parent.id,
            "input": [
                {
                    "type": "message",
                    "id": "msg_duplicate_lineage_fixture",
                    "role": "user",
                    "content": [
                        {"type": "input_text", "text": "Reuse the same id."}
                    ],
                }
            ],
            "max_output_tokens": 16,
            "store": True,
        },
    )

    assert response.status_code == 400
    assert response.body["error"]["type"] == "invalid_request_error"
    assert response.body["error"]["code"] == "invalid_value"
    assert response.body["error"]["param"] == "input"
    assert client.responses.delete(parent.id) is None


def test_stored_response_retrieve_and_delete_via_sdk():
    client = openai_client()
    created = client.responses.create(
        model="tinyllama-2",
        input="Store this response.",
        max_output_tokens=16,
        store=True,
    )

    retrieved = client.responses.retrieve(created.id)
    assert retrieved.id == created.id
    assert retrieved.output == created.output

    assert client.responses.delete(created.id) is None
    with pytest.raises(NotFoundError) as exc_info:
        client.responses.retrieve(created.id)
    assert exc_info.value.body["code"] == "response_not_found"
    assert exc_info.value.body["param"] == "response_id"


def test_sync_store_true_fails_if_the_resource_cannot_be_persisted():
    global server
    server.start()
    database_path = os.environ["LLAMA_RESPONSES_DB"]
    blocker = sqlite3.connect(database_path, timeout=0)
    blocker.execute("BEGIN IMMEDIATE")
    try:
        response = authenticated_request(
            "POST",
            "/v1/responses",
            data={
                "model": "tinyllama-2",
                "input": "This response must be stored atomically.",
                "max_output_tokens": 16,
                "store": True,
            },
        )
    finally:
        blocker.rollback()
        blocker.close()

    assert response.status_code == 500
    assert response.body["error"]["type"] == "server_error"
    assert response.body["error"]["code"] == "response_store_error"


def test_stream_store_true_fails_before_acknowledging_an_unstored_terminal():
    client = openai_client()
    conversation_id = "conv-responses-store-failure"
    database_path = os.environ["LLAMA_RESPONSES_DB"]
    blocker = sqlite3.connect(database_path, timeout=0)
    blocker.execute("BEGIN IMMEDIATE")
    try:
        events = list(
            client.responses.create(
                model="tinyllama-2",
                input="This streamed response must be stored atomically.",
                max_output_tokens=16,
                store=True,
                stream=True,
                extra_headers={"X-Conversation-Id": conversation_id},
            )
        )
    finally:
        blocker.rollback()
        blocker.close()

    assert events
    assert events[-1].type == "error"
    assert events[-1].code == "response_store_error"
    assert not any(
        event.type
        in {
            "response.completed",
            "response.incomplete",
            "response.failed",
            "response.cancelled",
        }
        for event in events
    )

    replay = authenticated_request(
        "GET", f"/v1/stream?conv_id={quote(conversation_id, safe='')}&from=0"
    )
    assert replay.status_code == 200
    assert "response_store_error" in str(replay.body)
    assert '"type":"response.completed"' not in str(replay.body)
    assert (
        authenticated_request(
            "DELETE", f"/v1/stream?conv_id={quote(conversation_id, safe='')}"
        ).status_code
        == 204
    )


def test_streamed_child_remains_continuable_when_parent_is_deleted_mid_generation():
    client = openai_client()
    parent = client.responses.create(
        model="tinyllama-2",
        input=[
            {
                "type": "message",
                "id": "msg_delete_race_parent_input",
                "role": "user",
                "content": [{"type": "input_text", "text": "Remember parent."}],
            }
        ],
        max_output_tokens=16,
        store=True,
    )

    child = None
    parent_deleted = False
    stream = client.responses.create(
        model="tinyllama-2",
        previous_response_id=parent.id,
        input=[
            {
                "type": "message",
                "id": "msg_delete_race_child_input",
                "role": "user",
                "content": [{"type": "input_text", "text": "Continue as a child."}],
            }
        ],
        max_output_tokens=16,
        store=True,
        stream=True,
    )
    for event in stream:
        if event.type == "response.created" and not parent_deleted:
            assert client.responses.delete(parent.id) is None
            parent_deleted = True
        if event.type in {"response.completed", "response.incomplete"}:
            child = event.response

    assert parent_deleted
    assert child is not None
    assert_foreground_terminal(child)

    grandchild = client.responses.create(
        model="tinyllama-2",
        previous_response_id=child.id,
        input=[
            {
                "type": "message",
                "id": "msg_delete_race_grandchild_input",
                "role": "user",
                "content": [{"type": "input_text", "text": "Continue once more."}],
            }
        ],
        max_output_tokens=16,
        store=True,
    )
    lineage = client.responses.input_items.list(grandchild.id, order="asc", limit=100)
    assert [lineage.data[index].id for index in (0, 2, 4)] == [
        "msg_delete_race_parent_input",
        "msg_delete_race_child_input",
        "msg_delete_race_grandchild_input",
    ]

    assert client.responses.delete(child.id) is None
    assert client.responses.delete(grandchild.id) is None


def test_generated_input_item_ids_remain_unique_across_server_restart():
    global server
    client = openai_client()
    created_ids: list[str] = []

    try:
        parent = client.responses.create(
            model="tinyllama-2",
            input="Persist a generated input id.",
            max_output_tokens=16,
            store=True,
        )
        created_ids.append(parent.id)
        parent_items = client.responses.input_items.list(
            parent.id,
            order="asc",
            limit=100,
        )
        parent_input_id = parent_items.data[0].id

        server.stop()
        client = openai_client()

        child = client.responses.create(
            model="tinyllama-2",
            previous_response_id=parent.id,
            input="Generate a different input id after restart.",
            max_output_tokens=16,
            store=True,
        )
        created_ids.append(child.id)
        retrieved = client.responses.retrieve(child.id)
        assert retrieved.id == child.id

        lineage = client.responses.input_items.list(
            child.id,
            order="asc",
            limit=100,
        )
        lineage_ids = [item.id for item in lineage.data]
        assert len(lineage_ids) == 3
        assert lineage_ids[0] == parent_input_id
        assert len(set(lineage_ids)) == len(lineage_ids)
    finally:
        if server.process is None:
            client = openai_client()
        for response_id in reversed(created_ids):
            try:
                client.responses.delete(response_id)
            except NotFoundError:
                pass


def test_stored_response_lifecycle_survives_server_restarts():
    global server
    client = openai_client()
    created_ids: list[str] = []

    try:
        parent = client.responses.create(
            model="tinyllama-2",
            input=[
                {
                    "type": "message",
                    "id": "msg_restart_parent_input",
                    "role": "user",
                    "content": [
                        {"type": "input_text", "text": "Persist this response."}
                    ],
                }
            ],
            max_output_tokens=16,
            store=True,
        )
        created_ids.append(parent.id)
        parent_output_id = parent.output[0].id

        # The fixture retains the same LLAMA_RESPONSES_DB across starts. A fresh
        # process and client therefore exercise SQLite, not process-local state.
        server.stop()
        client = openai_client()

        retrieved = client.responses.retrieve(parent.id)
        assert retrieved.id == parent.id
        assert retrieved.output == parent.output

        referenced = client.responses.create(
            model="tinyllama-2",
            input=[
                {"type": "item_reference", "id": parent_output_id},
                {
                    "type": "message",
                    "id": "msg_restart_reference_input",
                    "role": "user",
                    "content": [
                        {"type": "input_text", "text": "Acknowledge the reference."}
                    ],
                },
            ],
            max_output_tokens=16,
            store=True,
        )
        created_ids.append(referenced.id)
        referenced_items = client.responses.input_items.list(
            referenced.id,
            order="asc",
            limit=100,
        )
        assert [item.id for item in referenced_items.data] == [
            parent_output_id,
            "msg_restart_reference_input",
        ]

        continued = client.responses.create(
            model="tinyllama-2",
            previous_response_id=parent.id,
            input=[
                {
                    "type": "message",
                    "id": "msg_restart_child_input",
                    "role": "user",
                    "content": [
                        {"type": "input_text", "text": "Continue after the restart."}
                    ],
                }
            ],
            max_output_tokens=16,
            store=True,
        )
        created_ids.append(continued.id)
        assert continued.previous_response_id == parent.id

        lineage = client.responses.input_items.list(
            continued.id,
            order="asc",
            limit=100,
        )
        assert [item.id for item in lineage.data] == [
            "msg_restart_parent_input",
            parent_output_id,
            "msg_restart_child_input",
        ]

        # Deleting an ancestor must atomically materialize enough context into
        # its child for the lineage to remain continuable after another process
        # opens the database.
        assert client.responses.delete(parent.id) is None

        server.stop()
        client = openai_client()

        detached_child = client.responses.retrieve(continued.id)
        assert detached_child.id == continued.id
        assert detached_child.output == continued.output
        assert detached_child.previous_response_id == parent.id

        grandchild = client.responses.create(
            model="tinyllama-2",
            previous_response_id=continued.id,
            input=[
                {
                    "type": "message",
                    "id": "msg_restart_grandchild_input",
                    "role": "user",
                    "content": [
                        {
                            "type": "input_text",
                            "text": "Continue through the detached child.",
                        }
                    ],
                }
            ],
            max_output_tokens=16,
            store=True,
        )
        created_ids.append(grandchild.id)
        assert grandchild.previous_response_id == continued.id

        retrieved_grandchild = client.responses.retrieve(grandchild.id)
        assert retrieved_grandchild.id == grandchild.id
        assert retrieved_grandchild.output == grandchild.output

        grandchild_lineage = client.responses.input_items.list(
            grandchild.id,
            order="asc",
            limit=100,
        )
        assert [item.id for item in grandchild_lineage.data] == [
            "msg_restart_parent_input",
            parent_output_id,
            "msg_restart_child_input",
            continued.output[0].id,
            "msg_restart_grandchild_input",
        ]

        # Remove the surviving resources and prove all deletions survive one
        # final process restart. The parent was already removed above.
        for response_id in (referenced.id, continued.id, grandchild.id):
            assert client.responses.delete(response_id) is None

        server.stop()
        client = openai_client()
        for response_id in created_ids:
            with pytest.raises(NotFoundError):
                client.responses.retrieve(response_id)
    finally:
        # Keep the fixture database clean even when an assertion interrupts the
        # lifecycle above. Missing resources are the expected happy path.
        if server.process is None:
            try:
                client = openai_client()
            except Exception:
                client = None
        if client is not None:
            for response_id in reversed(created_ids):
                try:
                    client.responses.delete(response_id)
                except NotFoundError:
                    pass


def test_stored_response_input_items_page_via_sdk():
    client = openai_client()
    created = client.responses.create(
        model="tinyllama-2",
        input=[
            {
                "type": "message",
                "id": "msg_stored_input_one",
                "role": "user",
                "content": [{"type": "input_text", "text": "Store item one."}],
            },
            {
                "type": "message",
                "id": "msg_stored_input_two",
                "role": "user",
                "content": [{"type": "input_text", "text": "Store item two."}],
            },
        ],
        max_output_tokens=16,
        store=True,
    )

    first_page = client.responses.input_items.list(created.id, order="asc", limit=1)
    assert [item.id for item in first_page.data] == ["msg_stored_input_one"]
    assert first_page.has_more is True
    second_page = client.responses.input_items.list(
        created.id,
        order="asc",
        limit=1,
        after="msg_stored_input_one",
    )
    assert [item.id for item in second_page.data] == ["msg_stored_input_two"]
    assert second_page.has_more is False

    bad_order = authenticated_request(
        "GET", f"/v1/responses/{created.id}/input_items?order=sideways"
    )
    assert bad_order.status_code == 400
    assert bad_order.body["error"]["param"] == "order"
    for limit in ("0", "101", "1junk"):
        bad_limit = authenticated_request(
            "GET", f"/v1/responses/{created.id}/input_items?limit={limit}"
        )
        assert bad_limit.status_code == 400
        assert bad_limit.body["error"]["param"] == "limit"
    bad_after = authenticated_request(
        "GET", f"/v1/responses/{created.id}/input_items?after=msg_missing_cursor"
    )
    assert bad_after.status_code == 400
    assert bad_after.body["error"]["param"] == "after"


def test_input_token_count_sync_and_async_sdk_includes_stored_context():
    global server
    client = openai_client()
    parent = client.responses.create(
        model="tinyllama-2",
        input="Remember this context for token counting.",
        max_output_tokens=16,
        store=True,
    )

    standalone = client.responses.input_tokens.count(
        model="tinyllama-2",
        input="Count this continuation.",
    )
    continued = client.responses.input_tokens.count(
        model="tinyllama-2",
        input="Count this continuation.",
        previous_response_id=parent.id,
    )
    assert continued.input_tokens > standalone.input_tokens

    async def count_async() -> int:
        async with AsyncOpenAI(
            api_key=TEST_API_KEY,
            base_url=f"http://{server.server_host}:{server.server_port}/v1",
        ) as async_client:
            result = await async_client.responses.input_tokens.count(
                model="tinyllama-2",
                input="Count this continuation.",
                previous_response_id=parent.id,
            )
            return result.input_tokens

    assert asyncio.run(count_async()) == continued.input_tokens
    assert client.responses.delete(parent.id) is None


def test_unavailable_resource_operations_fail_explicitly():
    client = openai_client()
    created = client.responses.create(
        model="tinyllama-2",
        input="Create a resource for unsupported operation checks.",
        max_output_tokens=16,
        store=True,
    )

    requests = [
        ("GET", f"/v1/responses/{created.id}?stream=true"),
        ("GET", f"/v1/responses/{created.id}?starting_after=7"),
        ("POST", f"/v1/responses/{created.id}/cancel"),
        ("POST", "/v1/responses/compact"),
        ("GET", f"/v1/responses/{created.id}/input_items?include=reasoning"),
    ]
    for method, path in requests:
        response = authenticated_request(method, path)
        assert response.status_code == 501, path
        assert response.body["error"]["type"] == "invalid_request_error"
        assert response.body["error"]["code"] == "not_supported"

    assert client.responses.delete(created.id) is None


def test_missing_response_resources_share_the_not_found_envelope():
    global server
    server.start()
    missing_id = "resp_missing_resource_fixture"
    for method, path in [
        ("GET", f"/v1/responses/{missing_id}"),
        ("DELETE", f"/v1/responses/{missing_id}"),
        ("GET", f"/v1/responses/{missing_id}/input_items"),
    ]:
        response = authenticated_request(method, path)
        assert response.status_code == 404
        assert response.body["error"]["type"] == "invalid_request_error"
        assert response.body["error"]["code"] == "response_not_found"
        assert response.body["error"]["param"] == "response_id"


def test_streamed_response_is_stored_and_retrievable_via_sdk():
    client = openai_client()
    stream = client.responses.create(
        model="tinyllama-2",
        input="Store this streamed response.",
        max_output_tokens=16,
        store=True,
        stream=True,
    )

    terminal = None
    for item in stream:
        if item.type in {"response.completed", "response.incomplete"}:
            terminal = item.response

    assert terminal is not None
    assert_foreground_terminal(terminal)
    retrieved = client.responses.retrieve(terminal.id)
    assert retrieved.id == terminal.id
    assert retrieved.output == terminal.output


def test_max_output_tokens_produces_incomplete_sync_and_stream_resources():
    client = openai_client()

    response = client.responses.create(
        model="tinyllama-2",
        input="Continue the alphabet for several words: alpha, beta,",
        max_output_tokens=16,
        temperature=0.0,
    )
    assert response.status == "incomplete"
    assert response.completed_at is None
    assert response.incomplete_details is not None
    assert response.incomplete_details.reason == "max_output_tokens"

    stream = client.responses.create(
        model="tinyllama-2",
        input="Continue the alphabet for several words: alpha, beta,",
        max_output_tokens=16,
        temperature=0.0,
        stream=True,
    )
    terminal = None
    for item in stream:
        if item.type in {
            "response.completed",
            "response.incomplete",
            "response.failed",
        }:
            terminal = item

    assert terminal is not None
    assert terminal.type == "response.incomplete"
    assert terminal.response.status == "incomplete"
    assert terminal.response.completed_at is None
    assert terminal.response.incomplete_details is not None
    assert terminal.response.incomplete_details.reason == "max_output_tokens"


def test_async_sdk_resource_lifecycle():
    global server
    server.start()

    async def exercise() -> None:
        async with AsyncOpenAI(
            api_key=TEST_API_KEY,
            base_url=f"http://{server.server_host}:{server.server_port}/v1",
        ) as client:
            created = await client.responses.create(
                model="tinyllama-2",
                input=[
                    {
                        "type": "message",
                        "id": "msg_async_input",
                        "role": "user",
                        "content": [
                            {"type": "input_text", "text": "Store this asynchronously."}
                        ],
                    }
                ],
                max_output_tokens=16,
                store=True,
            )
            retrieved = await client.responses.retrieve(created.id)
            assert retrieved.id == created.id

            page = await client.responses.input_items.list(
                created.id,
                order="asc",
                limit=10,
            )
            assert [item.id for item in page.data] == ["msg_async_input"]

            continued = await client.responses.create(
                model="tinyllama-2",
                input="Continue this asynchronously.",
                previous_response_id=created.id,
                max_output_tokens=16,
                store=True,
            )
            continued_page = await client.responses.input_items.list(
                continued.id,
                order="asc",
                limit=100,
            )
            assert len(continued_page.data) == 3
            assert continued_page.data[1].id == created.output[0].id

            stream = await client.responses.create(
                model="tinyllama-2",
                input="Store this asynchronous stream.",
                max_output_tokens=16,
                store=True,
                stream=True,
            )
            streamed_response = None
            async for event_item in stream:
                if event_item.type in {"response.completed", "response.incomplete"}:
                    streamed_response = event_item.response
            assert streamed_response is not None
            streamed_retrieved = await client.responses.retrieve(streamed_response.id)
            assert streamed_retrieved.output == streamed_response.output

            assert await client.responses.delete(streamed_response.id) is None
            assert await client.responses.delete(continued.id) is None
            assert await client.responses.delete(created.id) is None
            with pytest.raises(NotFoundError):
                await client.responses.retrieve(created.id)

    asyncio.run(exercise())


def test_store_false_does_not_create_a_response_resource():
    client = openai_client()
    created = client.responses.create(
        model="tinyllama-2",
        input="Do not store this response.",
        max_output_tokens=16,
        store=False,
    )

    with pytest.raises(NotFoundError):
        client.responses.retrieve(created.id)


def test_item_reference_obeys_store_and_delete_lifecycle():
    client = openai_client()
    stored = client.responses.create(
        model="tinyllama-2",
        input="Create a stored output item.",
        max_output_tokens=16,
        store=True,
    )
    item_id = stored.output[0].id

    replayed = client.responses.create(
        model="tinyllama-2",
        input=[
            {"type": "item_reference", "id": item_id},
            {"role": "user", "content": "Acknowledge the referenced item."},
        ],
        max_output_tokens=16,
        store=True,
    )
    assert_foreground_terminal(replayed)
    replayed_items = client.responses.input_items.list(
        replayed.id,
        order="asc",
        limit=10,
    )
    assert replayed_items.data[0].id == item_id

    instructed = client.responses.create(
        model="tinyllama-2",
        instructions=[{"type": "item_reference", "id": item_id}],
        input="Acknowledge the referenced instruction item.",
        max_output_tokens=16,
        store=False,
    )
    assert_foreground_terminal(instructed)

    client.responses.delete(stored.id)
    with pytest.raises(BadRequestError) as deleted_error:
        client.responses.create(
            model="tinyllama-2",
            input=[{"type": "item_reference", "id": item_id}],
            max_output_tokens=16,
        )
    assert "item_reference_not_found" in deleted_error.value.body["message"]

    ephemeral = client.responses.create(
        model="tinyllama-2",
        input="Create an ephemeral output item.",
        max_output_tokens=16,
        store=False,
    )
    with pytest.raises(BadRequestError) as ephemeral_error:
        client.responses.create(
            model="tinyllama-2",
            input=[{"type": "item_reference", "id": ephemeral.output[0].id}],
            max_output_tokens=16,
        )
    assert "item_reference_not_found" in ephemeral_error.value.body["message"]
