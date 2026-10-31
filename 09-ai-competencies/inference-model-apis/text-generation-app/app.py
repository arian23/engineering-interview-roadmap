"""Text generation playground: Streamlit UI + LangChain + a local or hosted language model.

Type a prompt, pick a model, and get generated text streamed back, either from a model running on this
computer through Ollama (free, no token, no internet) or from Hugging Face Inference Providers.
See README.md for the step-by-step build.

Run with:  streamlit run app.py
"""

import os

import httpx
import ollama
import streamlit as st
from langchain_core.output_parsers import StrOutputParser
from langchain_core.prompts import ChatPromptTemplate
from langchain_huggingface import ChatHuggingFace, HuggingFaceEndpoint
from langchain_ollama import ChatOllama

LOCAL = "Local (Ollama)"
HUGGING_FACE = "Hugging Face (hosted)"
SOURCES = [LOCAL, HUGGING_FACE]
OLLAMA_URL = "http://localhost:11434"

# Hugging Face models.
# First two: open models (no licence to accept) that several Inference Providers serve.
# Last two: gated models. Accept the licence on huggingface.co first (Llama 3.1 and the Gemma family each
# have one approval that covers the whole group).
# Every model here had at least two live providers when chosen; availability changes over time.
MODEL_OPTIONS = [
    "Qwen/Qwen3-8B",
    "openai/gpt-oss-20b",
    "meta-llama/Llama-3.1-8B-Instruct",
    "google/gemma-3-27b-it",
]


# ===================================================================== Step 2: the API token


def token_from_config() -> str | None:
    """A token from .streamlit/secrets.toml or the HF_TOKEN environment variable, if there is one."""
    try:
        for key in ("HF_TOKEN", "HUGGINGFACEHUB_API_TOKEN"):
            if st.secrets.get(key):
                return st.secrets[key]
    except Exception:  # no secrets.toml at all: that is fine, the sidebar is the main way in
        pass
    return os.environ.get("HF_TOKEN") or os.environ.get("HUGGINGFACEHUB_API_TOKEN")


@st.cache_data(ttl=30)
def local_models() -> list[str]:
    """Models downloaded into the local Ollama (e.g. after `ollama pull llama3.2`); empty if Ollama is not running."""
    try:
        return [m.model for m in ollama.Client(host=OLLAMA_URL).list().models]
    except Exception:
        return []


# ===================================================================== Steps 4-6: model, prompt, chain

# Step 5: the prompt template. A chat template, because these instruct models are chat models:
# the instruction goes in the "system" message and the user's text in the "human" message.
PROMPT = ChatPromptTemplate.from_messages(
    [
        ("system", "You are a friendly, knowledgeable writing assistant. Respond to the request below "
                   "accurately and concisely."),
        ("human", "{input_text}"),
    ]
)


def build_chain(source: str, model_id: str, token: str | None, max_new_tokens: int, temperature: float):
    """Step 4 + 6: connect to the model and chain it with the prompt."""
    if source == LOCAL:
        # A chat model served by Ollama on this computer. num_predict is Ollama's name for max new tokens.
        chat_model = ChatOllama(model=model_id, base_url=OLLAMA_URL,
                                num_predict=max_new_tokens, temperature=temperature)
        return PROMPT | chat_model | StrOutputParser()

    llm = HuggingFaceEndpoint(
        repo_id=model_id,
        task="text-generation",
        max_new_tokens=max_new_tokens,  # length limit for the generated answer
        temperature=temperature,
        huggingfacehub_api_token=token,  # the token actually entered in the sidebar
        provider="auto",  # let Hugging Face pick an inference provider that serves this model
        timeout=120,
    )
    chat_model = ChatHuggingFace(llm=llm)  # sends proper chat messages instead of raw text
    # LCEL pipe syntax: prompt -> model -> plain string.
    return PROMPT | chat_model | StrOutputParser()


def friendly_error(error: Exception) -> str:
    """Turns common API failures into advice the user can act on, followed by Hugging Face's own message."""
    # Local model errors, recognised by exception type rather than by searching the message text.
    if isinstance(error, (httpx.ConnectError, ConnectionError)):
        return (f"Ollama is not running at {OLLAMA_URL}. Start the Ollama app (or run `ollama serve`) "
                "and try again.")
    if isinstance(error, ollama.ResponseError) and error.status_code == 404:
        return f"Ollama does not have this model yet. Download it with `ollama pull <model>`. ({error.error})"

    text = str(error)
    # Keep the informative lines; drop the request ID and the generic "for more information" link.
    useful = [line.strip() for line in text.splitlines()
              if line.strip() and not line.strip().startswith(("(Request ID", "For more information"))]
    details = f"\n\nDetails from Hugging Face: {' '.join(useful)[:400]}" if useful else ""
    lowered = text.lower()
    if "401" in text or "invalid credentials" in lowered or "invalid username or password" in lowered:
        return "The API token was rejected (401). Check that you copied the whole token." + details
    if "403" in text or "gated" in lowered:
        return ("Access denied (403). Either your licence request for this model is not approved yet "
                "(check https://huggingface.co/settings/gated-repos), or your token lacks the "
                "'Make calls to Inference Providers' permission. Run check_access.py to see which." + details)
    if "404" in text or "not supported" in lowered or "no provider" in lowered:
        return ("No inference provider you have enabled serves this model. Pick another model, or enable more "
                "providers at https://huggingface.co/settings/inference-providers." + details)
    if "402" in text or "no remaining credits" in lowered:
        return ("Out of Inference Providers credits (402). Your Hugging Face account has used its free monthly "
                "credits. Wait for the monthly reset, add credits or PRO at https://huggingface.co/settings/billing, "
                "or run a model locally instead." + details)
    if "429" in text:
        return "Rate limit or free credits reached (429). Wait a bit, or check your Hugging Face billing page." + details
    return f"An error occurred: {' '.join(useful) or text}"


# ===================================================================== Step 3: the interface

st.set_page_config(page_title="Text Generation Playground", page_icon="✍️")

st.title("Text Generation Playground")
st.markdown(
    "Pick an open-source language model, write a prompt, and watch the answer appear as it is generated. "
    "The model runs either on this computer (Ollama) or on Hugging Face Inference Providers; "
    "this app is the interface."
)

# Sidebar: where the model runs, the token (Hugging Face only) and model settings
st.sidebar.title("Model Settings")
source = st.sidebar.radio("Model source", SOURCES,
                          help="Local: free, private, no token or internet needed (requires Ollama). "
                               "Hugging Face: larger hosted models, needs an API token.")
token = None
if source == HUGGING_FACE:
    entered_token = st.sidebar.text_input(
        "Hugging Face API Token",
        type="password",  # shown as dots; never printed or logged
        help="Create one at https://huggingface.co/settings/tokens",
    )
    configured_token = token_from_config()
    token = entered_token.strip() or configured_token
    if entered_token.strip():
        st.sidebar.success("Using the token entered above.")
    elif configured_token:
        st.sidebar.info("Using the token from secrets.toml / HF_TOKEN.")
    else:
        st.sidebar.warning("Enter your token to start.")

st.sidebar.markdown("Configure the model settings for text generation.")
max_new_tokens = st.sidebar.slider("Max new tokens", min_value=50, max_value=500, value=200, step=10,
                                   help="Upper limit on the length of the answer, in tokens (pieces of words).")
temperature = st.sidebar.slider("Temperature", min_value=0.1, max_value=1.0, value=0.7, step=0.1,
                                help="Lower = more focused and repeatable, higher = more varied and creative.")

# Main area: model choice and prompt
if source == LOCAL:
    model_options = local_models()
    if not model_options:
        st.warning(f"No local models found. Make sure Ollama is running ({OLLAMA_URL}) and download a model, "
                   "e.g. `ollama pull llama3.2`.")
else:
    model_options = MODEL_OPTIONS
selected_model = st.selectbox("Select a model for text generation:", model_options)
prompt = st.text_area("Enter your prompt:", placeholder="Explain how a hash map works, in three sentences...")

# ===================================================================== Step 6: generate

if st.button("Generate Text", type="primary"):
    if source == HUGGING_FACE and not token:
        st.error("Please enter your Hugging Face API token in the sidebar.")
    elif not selected_model:
        st.error("No model available. Start Ollama and download a model first.")
    elif not prompt.strip():
        st.error("Please enter a valid prompt.")
    else:
        try:
            chain = build_chain(source, selected_model, token, max_new_tokens, temperature)
            st.subheader("Generated Text:")
            with st.spinner("Generating text..."):
                # Stream the answer token by token, so it appears in real time.
                st.write_stream(chain.stream({"input_text": prompt}))
        except Exception as error:
            st.error(friendly_error(error))
