import pytest
from openai import OpenAI
from utils import *

server: ServerProcess

@pytest.fixture(autouse=True)
def create_server(tmp_path, monkeypatch):
    global server
    monkeypatch.setenv("LLAMA_RESPONSES_DB", str(tmp_path / "responses.sqlite3"))
    server = ServerPreset.tinyllama2()
    server.server_port = 18089

def make_responses_request(input_items, **kwargs):
    global server
    data = {
        "model": "gpt-4.1",
        "input": input_items,
        "max_output_tokens": 16,
        "temperature": 0.8,
    }
    data.update(kwargs)
    return server.make_request("POST", "/v1/responses", data=data)

def assert_terminal_response(response):
    assert response.status_code == 200
    body = response.body
    assert body["status"] in {"completed", "incomplete"}
    if body["status"] == "completed":
        assert isinstance(body["completed_at"], int)
        assert body["incomplete_details"] is None
    else:
        assert body["completed_at"] is None
        assert body["incomplete_details"] == {"reason": "max_output_tokens"}
    return response

def assert_terminal_response_for_input(input_items, **kwargs):
    res = make_responses_request(input_items, **kwargs)
    return assert_terminal_response(res)

def test_responses_with_openai_library():
    global server
    server.start()
    client = OpenAI(api_key="dummy", base_url=f"http://{server.server_host}:{server.server_port}/v1")
    res = client.responses.create(
        model="gpt-4.1",
        input=[
            {"role": "system", "content": "Book"},
            {"role": "user", "content": "What is the best book"},
        ],
        max_output_tokens=16,
        temperature=0.8,
    )
    assert res.id.startswith("resp_")
    assert res.output[0].id is not None
    assert res.output[0].id.startswith("msg_")
    assert match_regex("(Suddenly)+", res.output_text)
    assert res.status in {"completed", "incomplete"}

def test_responses_stream_with_openai_library():
    global server
    server.start()
    client = OpenAI(api_key="dummy", base_url=f"http://{server.server_host}:{server.server_port}/v1")
    stream = client.responses.create(
        model="gpt-4.1",
        input=[
            {"role": "system", "content": "Book"},
            {"role": "user", "content": "What is the best book"},
        ],
        max_output_tokens=16,
        temperature=0.8,
        stream=True,
    )

    gathered_text = ''
    resp_id = ''
    msg_id = ''
    for r in stream:
        if r.type == "response.created":
            assert r.response.id.startswith("resp_")
            resp_id = r.response.id
        if r.type == "response.in_progress":
            assert r.response.id == resp_id
        if r.type == "response.output_item.added":
            assert r.item.id is not None
            assert r.item.id.startswith("msg_")
            msg_id = r.item.id
        if (r.type == "response.content_part.added" or
            r.type == "response.output_text.delta" or
            r.type == "response.output_text.done" or
            r.type == "response.content_part.done"):
            assert r.item_id == msg_id
        if r.type == "response.output_item.done":
            assert r.item.id == msg_id

        if r.type == "response.output_text.delta":
            gathered_text += r.delta
        if r.type in {"response.completed", "response.incomplete"}:
            assert r.response.id.startswith("resp_")
            assert r.response.output[0].id is not None
            assert r.response.output[0].id.startswith("msg_")
            assert gathered_text == r.response.output_text
            assert match_regex("(Suddenly)+", r.response.output_text)


def test_responses_schema_fields():
    """Verify Response fields use API defaults and echo request values."""
    global server
    server.start()
    res = server.make_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": "Book",
        "max_output_tokens": 16,
        "temperature": 0.8,
    })
    assert res.status_code == 200
    body = res.body
    # Terminal usage has the complete Responses token breakdown.
    usage = body["usage"]
    assert isinstance(usage["input_tokens_details"]["cached_tokens"], int)
    assert isinstance(usage["output_tokens_details"]["reasoning_tokens"], int)
    # Response lifecycle and API defaults.
    assert isinstance(body["created_at"], int)
    assert_terminal_response(res)
    assert body["previous_response_id"] is None
    assert body["instructions"] is None
    assert body["error"] is None
    assert body["tools"] == []
    assert body["tool_choice"] == "auto"
    assert body["truncation"] == "disabled"
    assert body["parallel_tool_calls"] is True
    assert body["text"] == {"format": {"type": "text"}, "verbosity": "medium"}
    assert body["top_p"] == 1.0
    assert body["temperature"] == 0.8
    assert body["presence_penalty"] == 0.0
    assert body["frequency_penalty"] == 0.0
    assert body["top_logprobs"] == 0
    assert body["reasoning"] is None
    assert body["max_output_tokens"] == 16
    assert body["store"] is True
    assert body["service_tier"] == "default"
    assert body["metadata"] == {}
    assert body["background"] is False
    assert body["safety_identifier"] is None
    assert body["prompt_cache_key"] is None
    assert body["prompt_cache_retention"] is None
    assert body["prompt"] is None
    assert body["conversation"] is None
    assert body["user"] is None
    assert body["max_tool_calls"] is None


def test_responses_stream_schema_fields():
    """Verify streaming done-events have the sequence_number, output_index,
    and content_index fields and the terminal Response envelope is stable."""
    global server
    server.start()
    res = server.make_stream_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": "Book",
        "max_output_tokens": 16,
        "temperature": 0.8,
        "stream": True,
    })
    seen_seq_nums = []
    saw_output_text_done = False
    saw_content_part_done = False
    saw_output_item_done = False
    initial_created_at = None
    terminal_response = None
    for data in res:
        assert "sequence_number" in data, f"missing sequence_number in {data.get('type')}"
        seen_seq_nums.append(data["sequence_number"])
        if data.get("type") == "response.created":
            initial_created_at = data["response"]["created_at"]
            assert data["response"]["usage"] is None
        if data.get("type") == "response.in_progress":
            assert data["response"]["created_at"] == initial_created_at
            assert data["response"]["usage"] is None
        if data.get("type") == "response.output_text.done":
            saw_output_text_done = True
            assert "content_index" in data
            assert "output_index" in data
            assert "logprobs" in data
            assert isinstance(data["logprobs"], list)
        if data.get("type") == "response.content_part.done":
            saw_content_part_done = True
            assert "content_index" in data
            assert "output_index" in data
        if data.get("type") == "response.output_item.done":
            saw_output_item_done = True
            assert "output_index" in data
        if data.get("type") in {"response.completed", "response.incomplete"}:
            terminal_response = data["response"]
    # Must have seen all done-event types
    assert saw_output_text_done, "never received response.output_text.done"
    assert saw_content_part_done, "never received response.content_part.done"
    assert saw_output_item_done, "never received response.output_item.done"
    # sequence_number must be present on done events and monotonically increasing
    assert len(seen_seq_nums) >= 4, f"expected >= 4 sequenced events, got {len(seen_seq_nums)}"
    assert all(a < b for a, b in zip(seen_seq_nums, seen_seq_nums[1:])), "sequence_numbers not strictly increasing"
    # The terminal response must retain request metadata and lifecycle state.
    assert initial_created_at is not None
    assert terminal_response is not None
    assert terminal_response["created_at"] == initial_created_at
    assert terminal_response["metadata"] == {}
    assert terminal_response["store"] is True
    assert terminal_response["temperature"] == 0.8
    assert terminal_response["max_output_tokens"] == 16
    assert terminal_response["truncation"] == "disabled"
    assert terminal_response["usage"]["output_tokens_details"]["reasoning_tokens"] == 0


def test_responses_unavailable_hosted_tools_fail_closed():
    """A valid hosted-tool declaration must not be silently discarded.

    Providers are a later phase, so this profile reports the missing
    capability before inference rather than pretending no tools were sent.
    """
    global server
    server.start()
    res = server.make_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": [
            {"role": "system", "content": "Book"},
            {"role": "user", "content": "What is the best book"},
        ],
        "max_output_tokens": 16,
        "temperature": 0.8,
        "tools": [
            {"type": "web_search"},
            {"type": "code_interpreter"},
        ],
    })
    assert res.status_code == 400
    assert res.body["error"]["type"] == "invalid_request_error"
    assert res.body["error"]["code"] == "unsupported_parameter"
    assert res.body["error"]["param"] == "tools[0].type"


def test_responses_unknown_tool_type_is_not_silently_dropped():
    global server
    server.start()
    res = server.make_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": [
            {"role": "system", "content": "Book"},
            {"role": "user", "content": "What is the best book"},
        ],
        "max_output_tokens": 16,
        "temperature": 0.8,
        "tools": [
            {"type": "future_magic"},
        ],
    })
    assert res.status_code == 400
    assert res.body["error"]["type"] == "invalid_request_error"
    assert res.body["error"]["code"] == "invalid_value"
    assert res.body["error"]["param"] == "tools[0].type"


def test_responses_inert_local_metadata_does_not_change_generation():
    """Validated identity/cache metadata may be locally inert.

    Fields with unavailable response semantics are covered by explicit-error
    tests instead of being silently stripped here.
    """
    global server
    server.start()
    # Baseline without extra keys
    baseline = server.make_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": [
            {"role": "system", "content": "Book"},
            {"role": "user", "content": "What is the best book"},
        ],
        "max_output_tokens": 16,
        "temperature": 0.8,
    })
    assert baseline.status_code == 200
    # Same request with extra Responses-only keys
    res = server.make_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": [
            {"role": "system", "content": "Book"},
            {"role": "user", "content": "What is the best book"},
        ],
        "max_output_tokens": 16,
        "temperature": 0.8,
        "store": True,
        # Codex requests this projection. Local models do not produce an
        # encrypted reasoning payload, so it is a compatibility no-op here.
        "include": ["reasoning.encrypted_content"],
        "prompt_cache_key": "test_key",
        "text": {"format": {"type": "text"}},
        "truncation": "disabled",
        "metadata": {"key": "value"},
    })
    assert_terminal_response(res)
    # Extra keys must not affect token consumption
    assert res.body["usage"]["input_tokens"] == baseline.body["usage"]["input_tokens"]


def test_responses_developer_role_merging():
    """Developer role messages must be merged into the first system message
    at position 0. This ensures templates that require a single system
    message don't see developer content as a separate turn.

    We verify by comparing token counts: system + developer merged should
    consume the same prompt tokens as a single system message with the
    combined content."""
    global server
    server.start()
    # Single combined system message
    combined = server.make_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": [
            {"role": "system", "content": [
                {"type": "input_text", "text": "Book"},
                {"type": "input_text", "text": "Keep it short"},
            ]},
            {"role": "user", "content": [{"type": "input_text", "text": "What is the best book"}]},
        ],
        "max_output_tokens": 16,
        "temperature": 0.8,
    })
    assert combined.status_code == 200
    # Split system + developer (should be merged to same prompt)
    split = server.make_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": [
            {"role": "system", "content": [{"type": "input_text", "text": "Book"}]},
            {"role": "user", "content": [{"type": "input_text", "text": "What is the best book"}]},
            {"role": "developer", "content": [{"type": "input_text", "text": "Keep it short"}]},
        ],
        "max_output_tokens": 16,
        "temperature": 0.8,
    })
    assert_terminal_response(split)
    # Merged prompt should consume same number of input tokens
    assert split.body["usage"]["input_tokens"] == combined.body["usage"]["input_tokens"]


def test_responses_input_text_type_multi_turn():
    """input_text type must be accepted for assistant messages (EasyInputMessage).
    An assistant message without explicit type:'message' must also be accepted
    (AssistantMessageItemParam). Verify the multi-turn context is preserved
    by checking the model sees the full conversation."""
    global server
    server.start()
    res = server.make_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": [
            {"role": "user", "content": [{"type": "input_text", "text": "Hello"}]},
            {
                "role": "assistant",
                "content": [{"type": "input_text", "text": "Hi there"}],
            },
            {"role": "user", "content": [{"type": "input_text", "text": "How are you"}]},
        ],
        "max_output_tokens": 16,
        "temperature": 0.8,
    })
    assert_terminal_response(res)
    # Multi-turn input should result in more prompt tokens than single-turn
    single = server.make_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": "How are you",
        "max_output_tokens": 16,
        "temperature": 0.8,
    })
    assert single.status_code == 200
    assert res.body["usage"]["input_tokens"] > single.body["usage"]["input_tokens"]


def test_responses_output_text_matches_content():
    """output_text must be the concatenation of all output_text content parts.
    Verify this for both streaming and non-streaming responses."""
    global server
    server.start()
    # Non-streaming
    res = server.make_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": [
            {"role": "system", "content": "Book"},
            {"role": "user", "content": "What is the best book"},
        ],
        "max_output_tokens": 16,
        "temperature": 0.8,
    })
    assert res.status_code == 200
    # Manually reconstruct output_text from content parts
    reconstructed = ""
    for item in res.body["output"]:
        if item.get("type") == "message":
            for part in item["content"]:
                if part.get("type") == "output_text":
                    reconstructed += part["text"]
    assert res.body["output_text"] == reconstructed
    assert len(reconstructed) > 0


def test_responses_stream_output_text_consistency():
    """Streaming gathered text must match output_text in the terminal event."""
    global server
    server.start()
    res = server.make_stream_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": [
            {"role": "system", "content": "Book"},
            {"role": "user", "content": "What is the best book"},
        ],
        "max_output_tokens": 16,
        "temperature": 0.8,
        "stream": True,
    })
    gathered_text = ""
    terminal_output_text = None
    for data in res:
        if data.get("type") == "response.output_text.delta":
            gathered_text += data["delta"]
        if data.get("type") in {"response.completed", "response.incomplete"}:
            terminal_output_text = data["response"]["output_text"]
            # Also verify content parts match
            for item in data["response"]["output"]:
                if item.get("type") == "message":
                    for part in item["content"]:
                        if part.get("type") == "output_text":
                            assert part["text"] == gathered_text
    assert terminal_output_text is not None
    assert gathered_text == terminal_output_text
    assert len(gathered_text) > 0


def test_responses_stream_created_event_has_full_response():
    """response.created must contain the full response object with all required
    fields, not just {id, object, status}. This is needed by strict client
    libraries like async-openai."""
    global server
    server.start()
    res = server.make_stream_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": [
            {"role": "system", "content": "Book"},
            {"role": "user", "content": "What is the best book"},
        ],
        "max_output_tokens": 16,
        "temperature": 0.8,
        "stream": True,
    })
    created_resp = None
    in_progress_resp = None
    terminal_resp = None
    for data in res:
        if data.get("type") == "response.created":
            created_resp = data["response"]
        if data.get("type") == "response.in_progress":
            in_progress_resp = data["response"]
        if data.get("type") in {
            "response.completed",
            "response.incomplete",
            "response.failed",
        }:
            terminal_resp = data["response"]
    assert created_resp is not None, "never received response.created"
    assert in_progress_resp is not None, "never received response.in_progress"
    assert terminal_resp is not None, "never received a terminal response event"
    # Both must have the full response object, not just minimal fields
    for resp in [created_resp, in_progress_resp]:
        assert resp["status"] == "in_progress"
        assert resp["id"].startswith("resp_")
        assert resp["object"] == "response"
        assert resp["model"] is not None
        assert resp["completed_at"] is None
        assert resp["metadata"] == {}
        assert resp["store"] is True
        assert resp["truncation"] == "disabled"
        assert resp["tools"] == []
        assert resp["usage"] is None
        assert resp["output"] == []
        assert resp["output_text"] == ""
    assert in_progress_resp["created_at"] == created_resp["created_at"]
    assert terminal_resp["created_at"] == created_resp["created_at"]
    assert terminal_resp["usage"] is not None


def test_responses_stream_all_events_have_sequence_number():
    """Every streaming event must have a sequence_number field and they must
    be strictly increasing across the entire stream."""
    global server
    server.start()
    res = server.make_stream_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": [
            {"role": "system", "content": "Book"},
            {"role": "user", "content": "What is the best book"},
        ],
        "max_output_tokens": 16,
        "temperature": 0.8,
        "stream": True,
    })
    all_seq_nums = []
    event_types = []
    for data in res:
        assert "sequence_number" in data, f"missing sequence_number in event type {data.get('type')}"
        all_seq_nums.append(data["sequence_number"])
        event_types.append(data.get("type", "unknown"))
    # Must have received multiple events
    assert len(all_seq_nums) >= 6, f"expected >= 6 events, got {len(all_seq_nums)}: {event_types}"
    # Must be strictly increasing
    for i in range(1, len(all_seq_nums)):
        assert all_seq_nums[i] > all_seq_nums[i-1], \
            f"sequence_number not strictly increasing at index {i}: {all_seq_nums[i-1]} -> {all_seq_nums[i]} (events: {event_types[i-1]} -> {event_types[i]})"


def test_responses_stream_delta_events_have_indices():
    """Delta and added events must have output_index. Content-related events
    must also have content_index."""
    global server
    server.start()
    res = server.make_stream_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": [
            {"role": "system", "content": "Book"},
            {"role": "user", "content": "What is the best book"},
        ],
        "max_output_tokens": 16,
        "temperature": 0.8,
        "stream": True,
    })
    saw_output_item_added = False
    saw_content_part_added = False
    saw_output_text_delta = False
    for data in res:
        evt = data.get("type", "")
        if evt == "response.output_item.added":
            saw_output_item_added = True
            assert "output_index" in data, "output_item.added missing output_index"
        if evt == "response.content_part.added":
            saw_content_part_added = True
            assert "output_index" in data, "content_part.added missing output_index"
            assert "content_index" in data, "content_part.added missing content_index"
        if evt == "response.output_text.delta":
            saw_output_text_delta = True
            assert "output_index" in data, "output_text.delta missing output_index"
            assert "content_index" in data, "output_text.delta missing content_index"
    assert saw_output_item_added, "never received response.output_item.added"
    assert saw_content_part_added, "never received response.content_part.added"
    assert saw_output_text_delta, "never received response.output_text.delta"


def test_responses_reasoning_content_array():
    """Reasoning items with content as array (spec format) must be accepted."""
    global server
    server.start()
    res = server.make_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": [
            {"role": "user", "content": [{"type": "input_text", "text": "Hi"}]},
            {"type": "reasoning", "summary": [],
             "content": [{"type": "reasoning_text", "text": "thinking"}]},
            {"role": "assistant", "type": "message",
             "content": [{"type": "output_text", "text": "Hello"}]},
            {"role": "user", "content": [{"type": "input_text", "text": "How are you"}]},
        ],
        "max_output_tokens": 16,
        "temperature": 0.8,
    })
    assert_terminal_response(res)


def test_responses_reasoning_content_string():
    """Reasoning items with content as plain string (OpenCode format) must be accepted."""
    global server
    server.start()
    res = server.make_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": [
            {"role": "user", "content": [{"type": "input_text", "text": "Hi"}]},
            {"type": "reasoning", "summary": [], "content": "thinking about it"},
            {"role": "assistant", "type": "message",
             "content": [{"type": "output_text", "text": "Hello"}]},
            {"role": "user", "content": [{"type": "input_text", "text": "How are you"}]},
        ],
        "max_output_tokens": 16,
        "temperature": 0.8,
    })
    assert_terminal_response(res)


def test_responses_reasoning_content_null():
    """Reasoning items with content:null (Codex format, issue openai/codex#11834)
    must be accepted — content may be null when encrypted_content is present."""
    global server
    server.start()
    res = server.make_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": [
            {"role": "user", "content": [{"type": "input_text", "text": "Hi"}]},
            {"type": "reasoning", "summary": [], "content": None,
             "encrypted_content": "opaque_data_here"},
            {"role": "assistant", "type": "message",
             "content": [{"type": "output_text", "text": "Hello"}]},
            {"role": "user", "content": [{"type": "input_text", "text": "How are you"}]},
        ],
        "max_output_tokens": 16,
        "temperature": 0.8,
    })
    assert_terminal_response(res)


def test_responses_reasoning_content_omitted():
    """Reasoning items with content omitted entirely must be accepted."""
    global server
    server.start()
    res = server.make_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": [
            {"role": "user", "content": [{"type": "input_text", "text": "Hi"}]},
            {"type": "reasoning", "summary": []},
            {"role": "assistant", "type": "message",
             "content": [{"type": "output_text", "text": "Hello"}]},
            {"role": "user", "content": [{"type": "input_text", "text": "How are you"}]},
        ],
        "max_output_tokens": 16,
        "temperature": 0.8,
    })
    assert_terminal_response(res)


def test_responses_input_file_with_data_graceful():
    """input_file items with file_data must be rendered as text content
    instead of rejecting the entire request."""
    global server
    server.start()
    res = server.make_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": [
            {"role": "user", "content": [
                {"type": "input_text", "text": "Summarize this file"},
                {"type": "input_file", "file_data": "hello world", "filename": "test.txt"},
            ]},
        ],
        "max_output_tokens": 16,
        "temperature": 0.8,
    })
    assert_terminal_response(res)
    # The file content must reach the model as prompt tokens
    baseline = server.make_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": [
            {"role": "user", "content": [
                {"type": "input_text", "text": "Summarize this file"},
            ]},
        ],
        "max_output_tokens": 16,
        "temperature": 0.8,
    })
    assert baseline.status_code == 200
    # With file_data injected as text, prompt must be longer
    assert res.body["usage"]["input_tokens"] > baseline.body["usage"]["input_tokens"]


def test_responses_input_file_filename_only():
    """input_file with only filename (no file_data) must produce a placeholder."""
    global server
    server.start()
    res = server.make_request("POST", "/v1/responses", data={
        "model": "gpt-4.1",
        "input": [
            {"role": "user", "content": [
                {"type": "input_text", "text": "What is this?"},
                {"type": "input_file", "filename": "report.pdf"},
            ]},
        ],
        "max_output_tokens": 16,
        "temperature": 0.8,
    })
    assert_terminal_response(res)


def test_responses_unknown_content_type_recovery_visible():
    """Unknown user content types should keep the request alive and inject
    visible recovery text into the converted prompt."""
    global server
    server.start()
    baseline = assert_terminal_response_for_input([
        {"role": "user", "content": [
            {"type": "input_text", "text": "Hello"},
        ]},
    ])
    recovered = assert_terminal_response_for_input([
        {"role": "user", "content": [
            {"type": "input_text", "text": "Hello"},
            {"type": "input_audio", "data": "base64stuff"},
        ]},
    ])
    assert recovered.body["usage"]["input_tokens"] > baseline.body["usage"]["input_tokens"]


def test_responses_unknown_assistant_content_type_recovery_visible():
    """Unknown assistant content types should keep the request alive and inject
    visible recovery text into the converted prompt."""
    global server
    server.start()
    baseline = assert_terminal_response_for_input([
        {"role": "user", "content": [{"type": "input_text", "text": "Hi"}]},
        {"role": "assistant", "type": "message", "content": [
            {"type": "output_text", "text": "Hello"},
        ]},
        {"role": "user", "content": [{"type": "input_text", "text": "How are you"}]},
    ])
    recovered = assert_terminal_response_for_input([
        {"role": "user", "content": [{"type": "input_text", "text": "Hi"}]},
        {"role": "assistant", "type": "message", "content": [
            {"type": "output_text", "text": "Hello"},
            {"type": "some_future_type", "data": "foo"},
        ]},
        {"role": "user", "content": [{"type": "input_text", "text": "How are you"}]},
    ])
    assert recovered.body["usage"]["input_tokens"] > baseline.body["usage"]["input_tokens"]


def test_responses_unknown_toplevel_item_skipped():
    """Unknown top-level item types must be skipped rather than rejecting."""
    global server
    server.start()
    assert_terminal_response_for_input([
        {"role": "user", "content": [{"type": "input_text", "text": "Hi"}]},
        {"type": "some_new_item_type", "data": "whatever"},
        {"role": "user", "content": [{"type": "input_text", "text": "How are you"}]},
    ])


def test_responses_malformed_input_text_recovery_visible():
    """Malformed input_text should keep the request alive and inject visible
    recovery text instead of silently disappearing."""
    global server
    server.start()
    baseline = assert_terminal_response_for_input([
        {"role": "user", "content": [
            {"type": "input_text", "text": "Hello"},
        ]},
    ])
    recovered = assert_terminal_response_for_input([
        {"role": "user", "content": [
            {"type": "input_text", "text": "Hello"},
            {"type": "input_text"},
        ]},
    ])
    assert recovered.body["usage"]["input_tokens"] > baseline.body["usage"]["input_tokens"]


def test_responses_malformed_input_image_recovery_visible():
    """Malformed input_image should keep the request alive and inject visible
    recovery text into the converted prompt."""
    global server
    server.start()
    baseline = assert_terminal_response_for_input([
        {"role": "user", "content": [
            {"type": "input_text", "text": "Describe this attachment"},
        ]},
    ])
    recovered = assert_terminal_response_for_input([
        {"role": "user", "content": [
            {"type": "input_text", "text": "Describe this attachment"},
            {"type": "input_image"},
        ]},
    ])
    assert recovered.body["usage"]["input_tokens"] > baseline.body["usage"]["input_tokens"]


def test_responses_malformed_input_file_recovery_visible():
    """Malformed input_file should keep the request alive and inject visible
    recovery text into the converted prompt."""
    global server
    server.start()
    baseline = assert_terminal_response_for_input([
        {"role": "user", "content": [
            {"type": "input_text", "text": "Summarize this upload"},
        ]},
    ])
    recovered = assert_terminal_response_for_input([
        {"role": "user", "content": [
            {"type": "input_text", "text": "Summarize this upload"},
            {"type": "input_file"},
        ]},
    ])
    assert recovered.body["usage"]["input_tokens"] > baseline.body["usage"]["input_tokens"]


def test_responses_malformed_assistant_output_text_recovery_visible():
    """Malformed assistant output_text history should keep the request alive
    and inject visible recovery text."""
    global server
    server.start()
    baseline = assert_terminal_response_for_input([
        {"role": "user", "content": [{"type": "input_text", "text": "Hi"}]},
        {"role": "assistant", "type": "message", "content": [
            {"type": "output_text", "text": "Hello"},
        ]},
        {"role": "user", "content": [{"type": "input_text", "text": "How are you"}]},
    ])
    recovered = assert_terminal_response_for_input([
        {"role": "user", "content": [{"type": "input_text", "text": "Hi"}]},
        {"role": "assistant", "type": "message", "content": [
            {"type": "output_text", "text": "Hello"},
            {"type": "output_text"},
        ]},
        {"role": "user", "content": [{"type": "input_text", "text": "How are you"}]},
    ])
    assert recovered.body["usage"]["input_tokens"] > baseline.body["usage"]["input_tokens"]


def test_responses_malformed_assistant_refusal_recovery_visible():
    """Malformed refusal history should keep the request alive and inject
    visible recovery text."""
    global server
    server.start()
    baseline = assert_terminal_response_for_input([
        {"role": "user", "content": [{"type": "input_text", "text": "Hi"}]},
        {"role": "assistant", "type": "message", "content": [
            {"type": "output_text", "text": "Hello"},
        ]},
        {"role": "user", "content": [{"type": "input_text", "text": "How are you"}]},
    ])
    recovered = assert_terminal_response_for_input([
        {"role": "user", "content": [{"type": "input_text", "text": "Hi"}]},
        {"role": "assistant", "type": "message", "content": [
            {"type": "output_text", "text": "Hello"},
            {"type": "refusal"},
        ]},
        {"role": "user", "content": [{"type": "input_text", "text": "How are you"}]},
    ])
    assert recovered.body["usage"]["input_tokens"] > baseline.body["usage"]["input_tokens"]


def test_responses_stream_with_llama_telemetry():
    global server
    server.n_ctx = 256
    server.n_batch = 32
    server.n_slots = 1
    server.start()

    saw_progress = False
    saw_delta_timings = False
    terminal = None

    res = server.make_stream_request("POST", "/responses", data={
        "input": "This is a test" * 10,
        "max_output_tokens": 16,
        "temperature": 0.8,
        "stream": True,
        "timings_per_token": True,
        "return_progress": True,
    })

    for data in res:
        if "prompt_progress" in data:
            assert data["type"] == "response.in_progress"
            assert data["prompt_progress"]["total"] > 0
            assert data["prompt_progress"]["processed"] >= data["prompt_progress"]["cache"]
            saw_progress = True
        if "timings" in data:
            assert "prompt_per_second" in data["timings"]
            assert "predicted_per_second" in data["timings"]
            if data["type"] == "response.output_text.delta":
                saw_delta_timings = True
        if data["type"] in {"response.completed", "response.incomplete"}:
            terminal = data

    assert saw_progress
    assert saw_delta_timings
    assert terminal is not None
    assert "usage" in terminal["response"]
    assert "timings" in terminal
