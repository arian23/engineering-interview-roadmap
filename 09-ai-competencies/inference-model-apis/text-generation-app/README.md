# Text Generation Playground (Hugging Face / Ollama + LangChain + Streamlit)

A small web app: type a prompt, choose a model, and get AI-generated text back. The model runs either
**on your own computer** through [Ollama](https://ollama.com) (free, no token, no internet) or **on
Hugging Face's servers** (larger models, needs an API token); this app is only the user interface and
the model calls. [How to run it](#how-to-run-it-local-or-hugging-face) covers both. The
[design decisions](#design-decisions) section explains the main choices.

| File | Purpose |
|---|---|
| [`app.py`](app.py) | The whole app: UI, model setup, prompt, generation |
| [`TEACHING_GUIDE.md`](TEACHING_GUIDE.md) | Step-by-step lessons with the theory behind each step, exercises and a session plan, for teaching others |
| [`requirements.txt`](requirements.txt) | Exact library versions the app was tested with |
| [`.streamlit/secrets.toml.example`](.streamlit/secrets.toml.example) | Template for storing your token in a file (optional) |
| [`.gitignore`](.gitignore) | Keeps the virtual environment and your real token out of git |
| [`check_access.py`](check_access.py) | Diagnoses token, licence and provider problems (see Troubleshooting) |
| [`docs/architecture.pptx`](docs/architecture.pptx) | Architecture diagrams: system overview, request flow, components, configuration and errors |

## How it works

```
 Browser (localhost:8501)            app.py (Streamlit)                        Model source
 ┌──────────────────────┐   input   ┌─────────────────────────────────┐        ┌──────────────────────┐
 │ source, model,       │ ────────► │ ChatPromptTemplate              │ local  │ Ollama on this PC    │
 │ prompt, sliders,     │           │   | ChatOllama  or              │ ─────► │ localhost:11434      │
 │ (token), Generate    │           │   | ChatHuggingFace             │ HTTPS  │ Hugging Face         │
 │ generated text       │ ◄──────── │   | StrOutputParser   (stream)  │ ─────► │ Inference Providers  │
 └──────────────────────┘  tokens   └─────────────────────────────────┘ ◄───── └──────────────────────┘
```

- **Streamlit** turns the Python script into a web page. Every widget change re-runs the script from the top.
- **LangChain** joins the prompt template, the model and the output parser into one chain. Only the model
  object differs between the two sources.
- **Ollama** (Local mode) runs a downloaded model on your own CPU/GPU: free and private, no token.
- **Hugging Face Inference Providers** (Hugging Face mode) run the model on their servers. Your API token
  proves who you are, and usage is billed against your account's free monthly credits.

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

`HuggingFaceEndpoint` and `ChatHuggingFace` live in the separate `langchain-huggingface` package, not in
`langchain` itself. `.gitignore` excludes `.venv/` and `.streamlit/secrets.toml`.

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
   - Provider availability changes over time. Older models such as `Meta-Llama-3-8B-Instruct` and
     `gemma-2-27b-it` are served by only one provider (featherless-ai), and `SmolLM2-1.7B-Instruct` by none,
     which is why the app uses newer versions.

The app accepts the token in three ways. The first one found is used:

| Order | Where | Good for |
|---|---|---|
| 1 | The **sidebar** password box | Trying the app |
| 2 | `.streamlit/secrets.toml` → `HF_TOKEN = "hf_..."` (copy it from `secrets.toml.example`) | Not retyping it every time |
| 3 | The `HF_TOKEN` environment variable | Servers and deployment |

The token is never printed or logged.

### Step 3: Build the interface

`app.py`, section *Step 3*:

| Element | Streamlit call |
|---|---|
| Title ("Text Generation Playground") and a one-line description | `st.title`, `st.markdown` |
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
    ("system", "You are a friendly, knowledgeable writing assistant. Respond to the request below "
               "accurately and concisely."),
    ("human", "{input_text}"),
])
```

The prompt is split into a **system** message, which sets the assistant's role and tone, and a **human**
message (what you typed). `{input_text}` is filled in with your prompt when the chain runs.

### Step 6: Chain everything and generate text

```python
chain = PROMPT | chat_model | StrOutputParser()

if st.button("Generate Text", type="primary"):
    ...                                   # token and prompt are checked first
    st.write_stream(chain.stream({"input_text": prompt}))
```

- **`|` (LangChain Expression Language):** the template's output goes into the model, and the model's
  reply goes into `StrOutputParser`, which turns it into plain text. This is LangChain's current way to
  compose steps (the older `LLMChain` class is deprecated).
- **Streaming:** `chain.stream(...)` yields the answer piece by piece, and `st.write_stream` shows each
  piece as it arrives. The answer appears in real time instead of all at once at the end.
- **Errors:** missing token, empty prompt, rejected token (401), gated model (403), no provider for the
  model (404) and rate limits (429) each show a clear message instead of a stack trace.

### Step 7: Run the app

The commands below are for macOS/Linux with the Hugging Face model source. For Windows, and for running
a llama model locally with Ollama, see [How to run it](#how-to-run-it-local-or-hugging-face), which has
step-by-step instructions for both model sources on Windows, macOS and Linux.

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

## How to run it: local or Hugging Face

One app, two places the model can run. You choose in the sidebar under **Model source**, and can switch
at any time without restarting:

| | **Local (Ollama)** (default) | **Hugging Face (hosted)** |
|---|---|---|
| Where the model runs | Your own computer | Hugging Face Inference Providers |
| Needs | Ollama + a downloaded model (e.g. llama3.2) | A Hugging Face API token |
| Internet | Only to download the model, once | Needed for every request |
| Cost | Free | Free monthly credits, then paid |
| Models | Whatever you `ollama pull` (llama3.2 is 3B parameters) | Qwen3 8B, gpt-oss 20B, Llama 3.1 8B, Gemma 3 27B |
| Answer quality / speed | Simpler answers; speed depends on your CPU/GPU | Usually better answers from larger models |

The chain is the same in both modes; only the model object changes (`ChatOllama` or `ChatHuggingFace`).

Jump to your case:

| | Windows | macOS | Linux |
|---|---|---|---|
| **Step 0:** install the app (needed for both) | [Windows](#step-0-on-windows) | [macOS](#step-0-on-macos) | [Linux](#step-0-on-linux) |
| **Scenario A:** llama running locally with Ollama | [Windows](#scenario-a-on-windows) | [macOS](#scenario-a-on-macos) | [Linux](#scenario-a-on-linux) |
| **Scenario B:** a model called with a Hugging Face API token | [Windows](#scenario-b-on-windows) | [macOS](#scenario-b-on-macos) | [Linux](#scenario-b-on-linux) |

---

### Step 0: install the app (once, needed for both scenarios)

You need **Python 3.10 or newer**. Check with `python --version` (Windows) or `python3 --version`
(macOS/Linux).

#### Step 0 on Windows

Run in **PowerShell**. The virtual environment goes at a short path such as `C:\venvs\textgen`, because
this folder is nested deeply enough that Windows' 260-character path limit makes `pip install streamlit`
fail inside a `.venv` here.

```powershell
cd <path-to>\09-ai-competencies\inference-model-apis\text-generation-app
python -m venv C:\venvs\textgen
C:\venvs\textgen\Scripts\pip install -r requirements.txt
```

If `python` is not found, install it with `winget install Python.Python.3.12`, then open a new
PowerShell window.

#### Step 0 on macOS

Run in **Terminal**:

```bash
cd <path-to>/09-ai-competencies/inference-model-apis/text-generation-app
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

If `python3` is not found, install it with `brew install python` (or from python.org).

#### Step 0 on Linux

Run in a **terminal**:

```bash
cd <path-to>/09-ai-competencies/inference-model-apis/text-generation-app
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

On Debian/Ubuntu, if `python3 -m venv` fails, install the venv module first:
`sudo apt install python3-venv` (Fedora: `sudo dnf install python3`).

---

### Scenario A: llama running locally with Ollama

No API token and no internet are needed once the model is downloaded. Ollama serves the model at
`http://localhost:11434`, and the app talks to it there.

#### Scenario A on Windows

1. **Install Ollama** (once), in PowerShell:
   ```powershell
   winget install Ollama.Ollama
   ```
   or download the installer from [ollama.com/download](https://ollama.com/download). Ollama starts
   automatically and keeps running in the system tray (the llama icon).
2. **Download the model** (once, about 2 GB). Open a **new** PowerShell window so `ollama` is found:
   ```powershell
   ollama pull llama3.2
   ollama list                 # llama3.2:latest should be listed
   ```
3. **Start the app**, from the project folder:
   ```powershell
   C:\venvs\textgen\Scripts\streamlit run app.py
   ```
4. **In the browser** (http://localhost:8501): **Model source** → *Local (Ollama)* (the default) →
   model `llama3.2:latest` → type a prompt → **Generate Text**.

If the app says Ollama is not running: start **Ollama** from the Start menu (the llama icon appears in
the tray), or run `ollama serve` in a separate PowerShell window.

#### Scenario A on macOS

1. **Install Ollama** (once): download the app from [ollama.com/download](https://ollama.com/download),
   move it to *Applications* and open it. It keeps running in the menu bar (the llama icon).
   *With Homebrew instead:* `brew install ollama`, then start the server with `brew services start ollama`
   (or run `ollama serve` in a separate Terminal window). The Homebrew version does not start by itself.
2. **Download the model** (once, about 2 GB):
   ```bash
   ollama pull llama3.2
   ollama list                 # llama3.2:latest should be listed
   ```
3. **Start the app**, from the project folder:
   ```bash
   source .venv/bin/activate
   streamlit run app.py
   ```
4. **In the browser** (http://localhost:8501): **Model source** → *Local (Ollama)* (the default) →
   model `llama3.2:latest` → type a prompt → **Generate Text**.

If the app says Ollama is not running: open the Ollama app, or run `ollama serve` /
`brew services start ollama`.

#### Scenario A on Linux

1. **Install Ollama** (once), with the official install script:
   ```bash
   curl -fsSL https://ollama.com/install.sh | sh
   ```
   It installs Ollama as a **systemd service** that starts automatically. Check it with
   `systemctl status ollama`.
2. **Download the model** (once, about 2 GB):
   ```bash
   ollama pull llama3.2
   ollama list                 # llama3.2:latest should be listed
   ```
3. **Start the app**, from the project folder:
   ```bash
   source .venv/bin/activate
   streamlit run app.py
   ```
4. **In the browser** (http://localhost:8501): **Model source** → *Local (Ollama)* (the default) →
   model `llama3.2:latest` → type a prompt → **Generate Text**.

If the app says Ollama is not running: `sudo systemctl start ollama`, or run `ollama serve` in a
separate terminal (on systems without systemd).

---

### Scenario B: a model called with a Hugging Face API token

The model runs on Hugging Face's servers; Ollama is not needed. Get a token first, as described in
[Step 2](#step-2-get-a-hugging-face-api-token) (fine-grained with *Make calls to Inference Providers*,
or *Read*). It starts with `hf_`. Treat it like a password.

There are three ways to give the app the token; the first one found is used:

| Way | Good for |
|---|---|
| **Sidebar** password box in the app | Trying it out; nothing is saved |
| `.streamlit/secrets.toml` file | Not retyping it every time (the file is git-ignored) |
| `HF_TOKEN` environment variable | Servers and deployment |

The steps below show all three for each platform; pick one.

#### Scenario B on Windows

1. **Give the app your token**, choosing one way, in PowerShell from the project folder:
   - *Sidebar:* nothing to do now; paste it in the app in step 3.
   - *secrets.toml:*
     ```powershell
     Copy-Item .streamlit\secrets.toml.example .streamlit\secrets.toml
     notepad .streamlit\secrets.toml      # replace hf_your_token_here with your token, save
     ```
   - *Environment variable,* for this PowerShell window only:
     ```powershell
     $env:HF_TOKEN = "hf_..."
     ```
     or permanently, for new windows: `setx HF_TOKEN "hf_..."`, then open a new PowerShell window.
2. **Start the app**, from the project folder:
   ```powershell
   C:\venvs\textgen\Scripts\streamlit run app.py
   ```
3. **In the browser** (http://localhost:8501): **Model source** → *Hugging Face (hosted)* → paste the
   token in **Hugging Face API Token** (unless you used secrets.toml / `HF_TOKEN`; the sidebar then says
   which token is in use) → pick a model, starting with `Qwen/Qwen3-8B` or `openai/gpt-oss-20b` →
   type a prompt → **Generate Text**.
4. **If a call is denied**, diagnose the token, licences and providers:
   ```powershell
   C:\venvs\textgen\Scripts\python check_access.py
   ```

#### Scenario B on macOS

1. **Give the app your token**, choosing one way, in Terminal from the project folder:
   - *Sidebar:* nothing to do now; paste it in the app in step 3.
   - *secrets.toml:*
     ```bash
     cp .streamlit/secrets.toml.example .streamlit/secrets.toml
     open -e .streamlit/secrets.toml       # replace hf_your_token_here with your token, save
     ```
   - *Environment variable,* for this Terminal window only. `read -s` keeps the token out of your shell
     history:
     ```bash
     read -s HF_TOKEN && export HF_TOKEN      # paste the token, press Enter (nothing is shown)
     ```
     To make it permanent, add `export HF_TOKEN="hf_..."` to `~/.zshrc` and open a new Terminal window.
2. **Start the app**, from the project folder:
   ```bash
   source .venv/bin/activate
   streamlit run app.py
   ```
3. **In the browser** (http://localhost:8501): **Model source** → *Hugging Face (hosted)* → paste the
   token in **Hugging Face API Token** (unless you used secrets.toml / `HF_TOKEN`) → pick a model,
   starting with `Qwen/Qwen3-8B` or `openai/gpt-oss-20b` → type a prompt → **Generate Text**.
4. **If a call is denied:** `.venv/bin/python check_access.py`

#### Scenario B on Linux

1. **Give the app your token**, choosing one way, in a terminal from the project folder:
   - *Sidebar:* nothing to do now; paste it in the app in step 3.
   - *secrets.toml:*
     ```bash
     cp .streamlit/secrets.toml.example .streamlit/secrets.toml
     nano .streamlit/secrets.toml          # replace hf_your_token_here with your token, save (Ctrl+O, Ctrl+X)
     ```
   - *Environment variable,* for this terminal only. `read -s` keeps the token out of your shell
     history:
     ```bash
     read -s HF_TOKEN && export HF_TOKEN      # paste the token, press Enter (nothing is shown)
     ```
     To make it permanent, add `export HF_TOKEN="hf_..."` to `~/.bashrc` and open a new terminal.
2. **Start the app**, from the project folder:
   ```bash
   source .venv/bin/activate
   streamlit run app.py
   ```
3. **In the browser** (http://localhost:8501): **Model source** → *Hugging Face (hosted)* → paste the
   token in **Hugging Face API Token** (unless you used secrets.toml / `HF_TOKEN`) → pick a model,
   starting with `Qwen/Qwen3-8B` or `openai/gpt-oss-20b` → type a prompt → **Generate Text**.
4. **If a call is denied:** `.venv/bin/python check_access.py`

---

### Using both, stopping the app

- **Compare the two:** with Ollama running and a token provided, run a prompt in one mode, switch
  **Model source**, and run the same prompt again. No restart is needed.
- **Stop the app:** press **Ctrl+C** in the terminal where `streamlit run` is running.
- **First run only:** Streamlit may wait in the terminal at an **"Email:"** question before the page
  loads. Press **Enter** to skip it.
- **Stop Ollama** when you don't need it: Windows/macOS: quit it from the tray / menu-bar icon;
  Linux: `sudo systemctl stop ollama`.

## The settings explained

| Setting | What it does | Tips |
|---|---|---|
| **Max new tokens** | Upper limit on the answer length. A *token* is a piece of a word, about ¾ of an English word on average, so 200 tokens is roughly 150 words | The model may stop earlier when it's finished |
| **Temperature** | How random the word choice is. Low (0.1–0.3): focused, repeatable, good for facts. High (0.8–1.0): varied and creative, good for stories | The slider starts at 0.1, because the API rejects exactly 0 |
| **Model** | Which model answers. Larger models (Gemma 3 27B, gpt-oss 20B) usually write better; smaller ones (Qwen3 8B, Llama 3.1 8B) are usually faster | Which models are served changes over time; if one fails, try another |

## Troubleshooting

| Message | Cause | Fix |
|---|---|---|
| "No local models found" / "Ollama is not running" (Local mode) | Ollama isn't installed or isn't running | Start the Ollama app (or run `ollama serve`); check with `ollama list` |
| "Ollama does not have this model yet" (Local mode) | The model hasn't been downloaded | `ollama pull <model>`, e.g. `ollama pull llama3.2` |
| `pip install` fails with "The filename or extension is too long" (Windows) | 260-character path limit inside `.venv` | Create the virtual environment at a short path, e.g. `C:\venvs\textgen` (see [How to run it](#how-to-run-it-local-or-hugging-face)) |
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
| Local mode against a real Ollama (llama3.2), driven by `AppTest` | Local is the default with no token box · installed models listed · a real answer streamed · empty prompt rejected · Hugging Face mode still shows its 4 models and requires a token · "Ollama not running" and "model not downloaded" messages shown |

**Not verified:** an actual call to Hugging Face. No API token was available when this was built. The
first real run with your token is the final check.

---

## Design decisions

| Decision | Why |
|---|---|
| The token is taken from the sidebar, then `secrets.toml`, then `HF_TOKEN` | Easy to try in the browser, convenient locally, and works on servers |
| The token is never printed or logged; `secrets.toml` is git-ignored | API tokens are secrets: anyone with the token can spend your credits |
| LCEL chain: `prompt \| model \| StrOutputParser()` | LangChain's current, composable style; `LLMChain` is deprecated |
| `langchain-huggingface` with versions pinned in `requirements.txt` | The maintained Hugging Face integration; pinned versions make installs reproducible |
| `max_new_tokens` for the length limit | It's the parameter the Hugging Face API actually uses for answer length |
| Temperature slider starts at 0.1 | The API rejects a temperature of exactly 0 |
| `ChatHuggingFace` with a chat prompt (system + user messages) | Instruct models answer much better with chat-formatted input than raw text |
| Answers are streamed with `st.write_stream` | Text appears as it's generated instead of all at once at the end |
| Clear messages for 401 / 402 / 403 / 404 / 429 | Tells the user what to do, plus Hugging Face's own message for detail |
| Only models with several live providers, plus `check_access.py` | Fewer "no provider" failures, and a quick way to diagnose token and access problems |

## Next steps

- **Run a model locally** with the `transformers` library: no API token and no network, but it needs a
  capable machine.
- **Add chat history** (`st.chat_message`, `st.chat_input`) so it becomes a conversation instead of one
  question at a time.
- **Deploy** to [Streamlit Community Cloud](https://streamlit.io/cloud): put the token in the app's
  *Secrets* setting there, never in the code.
