import pytest

from utils import ServerPreset, download_file


MODEL_DRAFT_FILE_URL = "https://huggingface.co/ggml-org/tiny-llamas/resolve/main/stories15M-q4_0.gguf"


@pytest.fixture
def server():
    instance = ServerPreset.stories15m_moe()
    instance.server_port = 18090
    instance.model_draft = download_file(MODEL_DRAFT_FILE_URL)
    instance.spec_type = "draft-simple"
    instance.spec_draft_n_min = 4
    instance.spec_draft_n_max = 8
    instance.fa = "off"
    yield instance
    instance.stop()


def test_logprobs_are_populated_for_speculatively_accepted_tokens(server):
    server.start()
    response = server.make_request(
        "POST",
        "/completion",
        data={
            "prompt": "I believe the meaning of life is",
            "temperature": 0.0,
            "top_k": 5,
            "seed": 4242,
            "n_predict": 16,
            "n_probs": 3,
        },
    )

    assert response.status_code == 200
    assert response.body["timings"]["draft_n"] > 0
    probabilities = response.body["completion_probabilities"]
    assert len(probabilities) == response.body["tokens_predicted"]
    assert len(probabilities) > 1
    for token in probabilities:
        assert token["logprob"] <= 0.0
        assert len(token["top_logprobs"]) == 3
