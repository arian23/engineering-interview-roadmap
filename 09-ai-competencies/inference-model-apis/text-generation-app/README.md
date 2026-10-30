# Simple text generation app (Hugging Face + LangChain + Streamlit)

A small web app: type a prompt, choose a model, and get AI-generated text back. The model runs on
Hugging Face's servers; this app is only the user interface and the API calls.

This is a working, up-to-date rebuild of the app from the blog post
[*Building a Simple Text Generation App with Hugging Face Model APIs*](https://medium.com/@jhonraza/building-a-simple-text-generation-app-with-hugging-face-model-apis-820a9859cc07)
(Jhon Raza, January 2025), with the problems in the original code fixed. The fixes are listed
[at the end](#what-was-changed-from-the-blog-post).

| File | Purpose |
|---|---|
| [`app.py`](app.py) | The whole app: UI, model setup, prompt, generation |
| [`requirements.txt`](requirements.txt) | Exact library versions the app was tested with |
| [`.streamlit/secrets.toml.example`](.streamlit/secrets.toml.example) | Template for storing your token in a file (optional) |
| [`.gitignore`](.gitignore) | Keeps the virtual environment and your real token out of git |
| [`check_access.py`](check_access.py) | Diagnoses token, licence and provider problems (see Troubleshooting) |
| [`docs/architecture.pptx`](docs/architecture.pptx) | Architecture diagrams: system overview, request flow, components, configuration and errors |

## How it works

```
 Browser (localhost:8501)            app.py (Streamlit)                       Hugging Face
 ┌──────────────────────┐   input   ┌───────────────────────────────┐  HTTPS  ┌──────────────────┐
 │ token, model, prompt │ ────────► │ ChatPromptTemplate            │ ──────► │ Inference        │
 │ sliders, Generate    │           │   | ChatHuggingFace           │         │ Providers run    │
 │ generated text       │ ◄──────── │   | StrOutputParser  (stream) │ ◄────── │ the model        │
 └──────────────────────┘  tokens   └───────────────────────────────┘ tokens  └──────────────────┘
```

- **Streamlit** turns the Python script into a web page. Every widget change re-runs the script from the top.
- **LangChain** joins the prompt template, the model and the output parser into one chain.
- **Hugging Face Inference Providers** run the model. Your API token proves who you are, and usage is
  billed against your account's free monthly credits.

---

## What was built, step by step

### Step 1: Create the project and install the libraries

A virtual environment keeps this project's libraries separate from the rest of your system.

```bash
mkdir text-generation-app && cd text-generation-app
python3 -m venv .venv
source .venv/bin/activate          # Windows: .venv\Scripts\activate
pip install streamlit langchain-core langchain-huggingface huggingface_hub
pip freeze | grep -iE "streamlit|langchain|huggingface"   # versions saved in requirements.txt
```

| Library | Role |
|---|---|
| `streamlit` | Builds the web UI from Python |
| `langchain-core` | Prompt templates, chains (`prompt \| model \| parser`), output parsers |
| `langchain-huggingface` | LangChain's connector to Hugging Face models (`HuggingFaceEndpoint`, `ChatHuggingFace`) |
| `huggingface_hub` | The underlying client that calls the Hugging Face API |

The blog installs `langchain` instead of `langchain-huggingface`, so its import of `HuggingFaceEndpoint`
fails. `.gitignore` excludes `.venv/` and `.streamlit/secrets.toml`.

### Step 2: Get a Hugging Face API token

1. Create a free account at [huggingface.co](https://huggingface.co/join).
2. Open **Settings → Access Tokens → Create new token**.
3. Choose the **Fine-grained** type and tick **"Make calls to Inference Providers"**, or choose **Read**.
4. Copy the token (it starts with `hf_`).
5. **Which models need extra steps:**
   - `Qwen/Qwen3-8B` and `openai/gpt-oss-20b` are open models: no licence to accept, and several
     providers serve them. **Start with one of these.**
   - `meta-llama/Llama-3.1-8B-Instruct` and `google/gemma-3-27b-it` are **gated**. Open the model page on
     huggingface.co and accept the licence (Llama requests are reviewed by Meta and can take hours; check the
     status at [settings/gated-repos](https://huggingface.co/settings/gated-repos)). One approval covers a
     whole group: "Meta's Llama 3.1 models & evals", "Google's Gemma models family".
   - The blog's models were replaced: `Meta-Llama-3-8B-Instruct` and `gemma-2-27b-it` are now served by only
     one provider (featherless-ai), and `SmolLM2-1.7B-Instruct` by none.

The app accepts the token in three ways. The first one found is used:

| Order | Where | Good for |
|---|---|---|
| 1 | The **sidebar** password box | Trying the app; the blog's approach |
| 2 | `.streamlit/secrets.toml` → `HF_TOKEN = "hf_..."` (copy it from `secrets.toml.example`) | Not retyping it every time |
| 3 | The `HF_TOKEN` environment variable | Servers and deployment |

The token is never printed or logged. The original code printed it to the terminal.

### Step 3: Build the interface

`app.py`, section *Step 3*:

| Element | Streamlit call |
|---|---|
| Title and welcome text | `st.title`, `st.markdown` |
| Token box (shown as dots) | `st.sidebar.text_input(type="password")` |
| **Max new tokens** slider, 50–500 | `st.sidebar.slider` |
| **Temperature** slider, 0.1–1.0 | `st.sidebar.slider` |
| Model dropdown (2 open models, then 2 gated models) | `st.selectbox` |
| Prompt box | `st.text_area` |
| **Generate Text** button | `st.button` |

The sidebar also shows which token source is in use, or a warning when there is none.

### Step 4: Connect to the hosted model

```python
llm = HuggingFaceEndpoint(
    repo_id=model_id,                 # e.g. "Qwen/Qwen3-8B"
    task="text-generation",
    max_new_tokens=max_new_tokens,    # length limit for the answer
    temperature=temperature,          # randomness
    huggingfacehub_api_token=token,   # from the sidebar, secrets.toml or HF_TOKEN
    provider="auto",                  # Hugging Face picks a provider that serves this model
    timeout=120,
)
chat_model = ChatHuggingFace(llm=llm)
```

- `HuggingFaceEndpoint` holds the connection settings.
- `ChatHuggingFace` wraps it so the request is sent as **chat messages** (system/user) formatted the way
  each instruct model expects. Instruct models answer much better that way than when given raw text.

### Step 5: Create the prompt template

```python
PROMPT = ChatPromptTemplate.from_messages([
    ("system", "You are a helpful assistant. Answer the user's query clearly."),
    ("human", "{input_text}"),
])
```

This is the blog's prompt, split into a **system** message (the assistant's role) and a **human**
message (what you typed). `{input_text}` is filled in with your prompt when the chain runs.

### Step 6: Chain everything and generate text

```python
chain = PROMPT | chat_model | StrOutputParser()

if st.button("Generate Text", type="primary"):
    ...                                   # token and prompt are checked first
    st.write_stream(chain.stream({"input_text": prompt}))
```

- **`|` (LangChain Expression Language):** the template's output goes into the model, and the model's
  reply goes into `StrOutputParser`, which turns it into plain text. This replaces the deprecated
  `LLMChain` used in the blog.
- **Streaming:** `chain.stream(...)` yields the answer piece by piece, and `st.write_stream` shows each
  piece as it arrives. The answer appears in real time instead of all at once at the end.
- **Errors:** missing token, empty prompt, rejected token (401), gated model (403), no provider for the
  model (404) and rate limits (429) each show a clear message instead of a stack trace.

### Step 7: Run the app

```bash
source .venv/bin/activate
streamlit run app.py
```

Your browser opens **http://localhost:8501**. Paste your token in the sidebar, pick a model, type a
prompt and click **Generate Text**. Stop the app with **Ctrl+C** in the terminal.

**First run only:** Streamlit first shows a welcome message in the terminal and waits at an **"Email:"**
question. The server doesn't start, and `localhost:8501` won't load, until you answer. Press **Enter**
to skip it. It's never asked again.

Use `streamlit run`, not `python app.py`. Plain `python` won't start a web page.

---

## Quick start (if you just want to run it)

```bash
cd 09-ai-competencies/inference-model-apis/text-generation-app
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
streamlit run app.py
```

## The settings explained

| Setting | What it does | Tips |
|---|---|---|
| **Max new tokens** | Upper limit on the answer length. A *token* is a piece of a word, about ¾ of an English word on average, so 200 tokens is roughly 150 words | The model may stop earlier when it's finished |
| **Temperature** | How random the word choice is. Low (0.1–0.3): focused, repeatable, good for facts. High (0.8–1.0): varied and creative, good for stories | The slider starts at 0.1, because the API rejects exactly 0 |
| **Model** | Which model answers. Larger models (Gemma 3 27B, gpt-oss 20B) usually write better; smaller ones (Qwen3 8B, Llama 3.1 8B) are usually faster | Which models are served changes over time; if one fails, try another |

## Troubleshooting

| Message | Cause | Fix |
|---|---|---|
| "The API token was rejected (401)" | Wrong or incomplete token | Copy the whole token again; create a new one if needed |
| "Access denied (403)" | Licence request not approved yet, or the token lacks permission | Check [settings/gated-repos](https://huggingface.co/settings/gated-repos); give the token the "Make calls to Inference Providers" permission; run `.venv/bin/python check_access.py` to see which |
| "No inference provider you have enabled serves this model" | No provider runs this model, or the providers that do are disabled in your account | Pick another model, or enable more providers at [settings/inference-providers](https://huggingface.co/settings/inference-providers) |
| "Out of Inference Providers credits (402)" | The account's free monthly credits are used up | Wait for the monthly reset, add credits or PRO at [settings/billing](https://huggingface.co/settings/billing), or run a model locally |
| "Rate limit or free credits reached (429)" | Too many requests, or the monthly free credits are used up | Wait, or check your Hugging Face billing page |
| Page never appears | Started with `python app.py` | Use `streamlit run app.py` |
| `localhost:8501` doesn't load on the very first run | Streamlit is waiting in the terminal at its one-time **"Email:"** question, and hasn't started the server yet | Press **Enter** in the terminal (leaving it blank is fine). It isn't asked again |

## How it was verified

| Check | Result |
|---|---|
| `streamlit run app.py` starts the real web server | Health check `ok`, page served (HTTP 200) |
| The real `HuggingFaceEndpoint` and `ChatHuggingFace` accept the app's settings | Built with `max_new_tokens`, `temperature`, `provider` as set |
| UI flows, driven by Streamlit's `AppTest` with a stand-in model replacing the network call | No token → error · empty prompt → error · token + prompt → answer shown · rejected token → friendly 401 message · `HF_TOKEN` variable used when the sidebar is empty |
| The values from the sidebar reach the model | Slider settings 120 tokens / 0.3, and the sidebar token, were passed through |

**Not verified:** an actual call to Hugging Face. No API token was available when this was built. The
first real run with your token is the final check.

---

## What was changed from the blog post

| # | Problem in the original | Fix here |
|---|---|---|
| 1 | The token typed in the sidebar was saved but **never used**; the model read `st.secrets` instead, so the app failed without a `secrets.toml` | The sidebar token is used, with `secrets.toml` and `HF_TOKEN` as fall-backs |
| 2 | `print(st.session_state.HUGGINGFACEHUB_API_TOKEN)` **printed the secret token** to the terminal | Removed; the token is never printed |
| 3 | `LLMChain` is deprecated in LangChain | `prompt \| model \| StrOutputParser()` (LCEL) |
| 4 | The install line missed `langchain-huggingface` (and had a typo, `huggingface_hubbash`) | Correct packages, versions pinned in `requirements.txt` |
| 5 | `model_kwargs={"max_length": ...}` didn't control the answer length | `max_new_tokens=...` |
| 6 | The temperature slider allowed 0.0, which the API rejects | The slider starts at 0.1 |
| 7 | Instruct models received raw text | `ChatHuggingFace` + a chat prompt (system + user messages) |
| 8 | The answer appeared only when complete | Streamed in real time with `st.write_stream` |
| 9 | Errors showed raw exception text | Clear messages for 401 / 403 / 404 / 429 |
| 10 | Only code fragments were shown, without imports | One complete, runnable `app.py` |

## Next steps (from the blog, still good ideas)

- **Run a model locally** with the `transformers` library: no API token and no network, but it needs a
  capable machine.
- **Add chat history** (`st.chat_message`, `st.chat_input`) so it becomes a conversation instead of one
  question at a time.
- **Deploy** to [Streamlit Community Cloud](https://streamlit.io/cloud): put the token in the app's
  *Secrets* setting there, never in the code.
