"""Codex-critical OpenAI Responses conformance fixtures and HTTP checks.

The tiny model used by the server suite cannot deterministically produce tool
calls.  Exact tool-call streams therefore live here as SDK-validated golden
transcripts, while the HTTP tests exercise the replay/continuation direction
against llama-server.  Scripted generation tests in ``tools/llama-responses``
should reuse the invariants below when that seam is available.
"""

from __future__ import annotations

import hashlib
from collections import defaultdict
from typing import Any

import pytest
from openai import BadRequestError, NotFoundError, OpenAI
from openai.types.responses import ResponseStreamEvent
from pydantic import TypeAdapter

from utils import ServerPreset


server = ServerPreset.tinyllama2()
STREAM_EVENT_ADAPTER = TypeAdapter(ResponseStreamEvent)
TEST_API_KEY = "sk-llama-responses-test"


@pytest.fixture(autouse=True)
def create_server():
    global server
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

    for version in ("0.148.0", "99.0.0", "opaque-preview"):
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
        instructions = model["base_instructions"].encode()
        assert len(instructions) == 20_903
        assert hashlib.sha256(instructions).hexdigest() == (
            "ac8ae107a0d72fe3476b430afb161ea4e67da2e446d778aefc44828160559807"
        )


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
        max_output_tokens=8,
        temperature=0.0,
    )

    assert response.status == "completed"
    assert response.id.startswith("resp_")
    assert response.output_text


@pytest.mark.parametrize(
    "body, message_fragment",
    [
        ({"model": "tinyllama-2"}, "'input' is required"),
        ({"model": "tinyllama-2", "input": 17}, "'input' must be"),
        ({"model": "tinyllama-2", "input": None}, "'input' must be"),
        (
            {"model": "tinyllama-2", "input": "hello", "store": "true"},
            "'store' must be",
        ),
        (
            {"model": "tinyllama-2", "input": "hello", "conversation": "conv_missing"},
            "conversation state is not available",
        ),
        (
            {"model": "tinyllama-2", "input": "hello", "background": True},
            "background responses are not available",
        ),
        (
            {"model": "tinyllama-2", "input": "hello", "instructions": {}},
            "'instructions' must be",
        ),
        (
            {
                "model": "tinyllama-2",
                "input": [{"type": "item_reference", "id": "missing_item_fixture"}],
            },
            "item_reference_not_found",
        ),
    ],
)
def test_responses_validation_errors_have_openai_shape(body, message_fragment):
    global server
    server.start()
    response = authenticated_request("POST", "/v1/responses", data=body)

    assert response.status_code == 400
    assert set(response.body) == {"error"}
    error = response.body["error"]
    assert error["code"] == "invalid_request"
    assert error["type"] == "invalid_request_error"
    assert message_fragment in error["message"]


def test_openai_sdk_surfaces_responses_validation_error():
    client = openai_client()

    with pytest.raises(BadRequestError) as exc_info:
        client.responses.create(model="tinyllama-2")

    assert exc_info.value.status_code == 400
    assert exc_info.value.body["code"] == "invalid_request"
    assert exc_info.value.body["type"] == "invalid_request_error"
    assert "'input' is required" in exc_info.value.body["message"]


def test_stored_previous_response_id_continues_and_is_echoed_via_sdk():
    client = openai_client()
    first = client.responses.create(
        model="tinyllama-2",
        input="Remember the word amber.",
        max_output_tokens=8,
        store=True,
    )
    continued = client.responses.create(
        model="tinyllama-2",
        input="What word did I ask you to remember?",
        max_output_tokens=8,
        previous_response_id=first.id,
        store=True,
    )
    standalone = client.responses.create(
        model="tinyllama-2",
        input="What word did I ask you to remember?",
        max_output_tokens=8,
        store=False,
    )
    third = client.responses.create(
        model="tinyllama-2",
        input="Repeat the remembered word once more.",
        max_output_tokens=8,
        previous_response_id=continued.id,
        store=True,
    )

    assert continued.status == "completed"
    assert continued.previous_response_id == first.id
    # Echoing the identifier is insufficient: the stored input/output must be
    # present in the continued prompt.  Token count makes that observable with
    # the deterministic tiny fixture without depending on generated wording.
    assert continued.usage is not None
    assert standalone.usage is not None
    assert continued.usage.input_tokens > standalone.usage.input_tokens
    assert third.usage is not None
    assert third.usage.input_tokens > continued.usage.input_tokens


def test_stored_response_retrieve_and_delete_via_sdk():
    client = openai_client()
    created = client.responses.create(
        model="tinyllama-2",
        input="Store this response.",
        max_output_tokens=8,
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


def test_stored_response_input_items_page_via_sdk():
    client = openai_client()
    created = client.responses.create(
        model="tinyllama-2",
        input=[
            {
                "type": "message",
                "id": "msg_stored_input",
                "role": "user",
                "content": [{"type": "input_text", "text": "Store this input item."}],
            }
        ],
        max_output_tokens=8,
        store=True,
    )

    page = client.responses.input_items.list(created.id, order="asc", limit=10)
    assert [item.id for item in page.data] == ["msg_stored_input"]

    bad_order = authenticated_request(
        "GET", f"/v1/responses/{created.id}/input_items?order=sideways"
    )
    assert bad_order.status_code == 400
    bad_limit = authenticated_request(
        "GET", f"/v1/responses/{created.id}/input_items?limit=1junk"
    )
    assert bad_limit.status_code == 400


def test_streamed_response_is_stored_and_retrievable_via_sdk():
    client = openai_client()
    stream = client.responses.create(
        model="tinyllama-2",
        input="Store this streamed response.",
        max_output_tokens=8,
        store=True,
        stream=True,
    )

    completed = None
    for item in stream:
        if item.type == "response.completed":
            completed = item.response

    assert completed is not None
    retrieved = client.responses.retrieve(completed.id)
    assert retrieved.id == completed.id
    assert retrieved.output == completed.output


def test_store_false_does_not_create_a_response_resource():
    client = openai_client()
    created = client.responses.create(
        model="tinyllama-2",
        input="Do not store this response.",
        max_output_tokens=8,
        store=False,
    )

    with pytest.raises(NotFoundError):
        client.responses.retrieve(created.id)


def test_item_reference_obeys_store_and_delete_lifecycle():
    client = openai_client()
    stored = client.responses.create(
        model="tinyllama-2",
        input="Create a stored output item.",
        max_output_tokens=8,
        store=True,
    )
    item_id = stored.output[0].id

    replayed = client.responses.create(
        model="tinyllama-2",
        input=[
            {"type": "item_reference", "id": item_id},
            {"role": "user", "content": "Acknowledge the referenced item."},
        ],
        max_output_tokens=8,
        store=False,
    )
    assert replayed.status == "completed"

    instructed = client.responses.create(
        model="tinyllama-2",
        instructions=[{"type": "item_reference", "id": item_id}],
        input="Acknowledge the referenced instruction item.",
        max_output_tokens=8,
        store=False,
    )
    assert instructed.status == "completed"

    client.responses.delete(stored.id)
    with pytest.raises(BadRequestError) as deleted_error:
        client.responses.create(
            model="tinyllama-2",
            input=[{"type": "item_reference", "id": item_id}],
            max_output_tokens=8,
        )
    assert "item_reference_not_found" in deleted_error.value.body["message"]

    ephemeral = client.responses.create(
        model="tinyllama-2",
        input="Create an ephemeral output item.",
        max_output_tokens=8,
        store=False,
    )
    with pytest.raises(BadRequestError) as ephemeral_error:
        client.responses.create(
            model="tinyllama-2",
            input=[{"type": "item_reference", "id": ephemeral.output[0].id}],
            max_output_tokens=8,
        )
    assert "item_reference_not_found" in ephemeral_error.value.body["message"]
