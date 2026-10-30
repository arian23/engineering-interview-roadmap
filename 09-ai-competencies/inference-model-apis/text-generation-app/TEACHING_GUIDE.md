# Teaching guide: build an AI text generation app, step by step

This guide teaches how a modern AI app works by building one from scratch: a web page where you type
a prompt, pick a language model, and watch the answer appear as it's generated. It's written so you can
teach it to others.

Each step has three parts:
- **Theory:** the idea behind the step, explained from first principles.
- **Build:** the code. Every step is a complete, runnable file.
- **Check:** what you should see, a small experiment, and a few questions to discuss.

The finished result is [`app.py`](app.py) in this folder.

| | |
|---|---|
| **Audience** | Comfortable with basic Python (variables, functions, `if`, `for`, `pip install`). No AI background needed |
| **Time** | About 2.5–3 hours, as one workshop or two sessions (see the [session plan](#session-plan-for-teachers)) |
| **You'll learn** | What a language model is, tokens, temperature, chat models, hosted inference APIs, API keys, LangChain, Streamlit, streaming, error handling |
| **Each learner needs** | A laptop with Python 3.11+, and a free Hugging Face account |

---

## Contents

0. [The big picture](#step-0-the-big-picture)
1. [Set up the project](#step-1-set-up-the-project)
2. [A first web page with Streamlit](#step-2-a-first-web-page-with-streamlit)
3. [Add the controls](#step-3-add-the-controls)
4. [Get an API token](#step-4-get-an-api-token)
5. [Talk to a model directly](#step-5-talk-to-a-model-directly)
6. [Prompt templates and chains with LangChain](#step-6-prompt-templates-and-chains-with-langchain)
7. [Put it together: the full app](#step-7-put-it-together-the-full-app)
8. [Handle errors and keep secrets safe](#step-8-handle-errors-and-keep-secrets-safe)
9. [Where to go next](#step-9-where-to-go-next)

Also: [glossary](#glossary) · [session plan for teachers](#session-plan-for-teachers) · [answers](#answers-to-the-discussion-questions)

---

## Step 0: The big picture

### Theory

**What is a large language model (LLM)?**
An LLM is a very large neural network trained on a huge amount of text to do one thing: **predict the
next piece of text**. Given "The capital of France is", it gives a high probability to " Paris".

**How does it write a whole answer?** One piece at a time. It predicts the next piece, adds it to the
text, and repeats. This loop is called *autoregressive generation*:

```
"Explain gravity"                  → predicts " Gravity"
"Explain gravity Gravity"          → predicts " is"
"Explain gravity Gravity is"       → predicts " a"
...                                   until it predicts "end of answer", or hits a length limit
```

**Tokens.** Models don't read letters or whole words. Text is split into **tokens**: common words are
one token, rare words are split into pieces ("unbelievable" → "un", "believ", "able"). On average a
token is about ¾ of an English word. Everything is measured in tokens: input size, output length, speed
and price.

**Base models vs chat (instruct) models.** A *base* model only continues text. An *instruct* or *chat*
model has been trained further to **follow instructions** and hold a conversation. Chat models take a
list of **messages**, each with a **role**:

| Role | Who writes it | Example |
|---|---|---|
| `system` | The app developer | "You are a concise assistant." Sets behaviour and tone |
| `user` | The person using the app | "Explain gravity in two sentences." |
| `assistant` | The model | The answer |

All four models in this app (Qwen3, gpt-oss, Llama 3.1, Gemma 3) are chat models.

**Where does the model run?** A model with billions of parameters needs powerful hardware, usually
GPUs. Running a trained model to produce an answer is called **inference**. You can do it on your own
machine, or send your request to a company that runs the model for you, a **hosted inference API**.
This app uses Hugging Face **Inference Providers**: Hugging Face receives your request and forwards it
to a partner company (Groq, Nscale, DeepInfra and others) that runs the model.

### The app you'll build

```
 Your computer                                          The internet
 ┌─────────────────────────────────────────┐           ┌────────────────────────────────┐
 │ Browser  ◄────────►  Python script       │   HTTPS   │ Hugging Face router            │
 │ (the page)           ┌───────────────┐   │ ────────► │  checks token, licence, credit │
 │                      │ Streamlit: UI │   │           │        │                       │
 │                      │ LangChain:    │   │ ◄──────── │ Inference provider runs the    │
 │                      │ prompt→model  │   │  tokens   │ model on GPUs                  │
 │                      └───────────────┘   │  stream   └────────────────────────────────┘
 └─────────────────────────────────────────┘
```

| Tool | Job |
|---|---|
| **Streamlit** | Turns a Python script into a web page, with no HTML or JavaScript |
| **LangChain** | Builds the prompt, calls the model, cleans up the output |
| **Hugging Face** | Hosts the models and runs them for us |

> **Discuss:** Why not run the model on your laptop? What do you gain and lose by using a hosted API?

---

## Step 1: Set up the project

### Theory

**Virtual environments.** Every project needs its own set of libraries, at specific versions. A
*virtual environment* is a private folder of libraries for one project, so projects don't interfere
with each other. You *activate* it to use it.

**Pinning versions.** Libraries change. Writing exact versions in `requirements.txt` means everyone
installs the same thing, and code that worked yesterday still works tomorrow.

### Build

```bash
mkdir text-generation-app
cd text-generation-app

python3 -m venv .venv                 # create the environment (once)
source .venv/bin/activate             # activate it (every new terminal). Windows: .venv\Scripts\activate

pip install streamlit==1.66.1 langchain-core==1.7.3 langchain-huggingface==1.2.3 huggingface_hub==1.33.0
```

| Library | What it gives us |
|---|---|
| `streamlit` | The web UI |
| `langchain-core` | Prompt templates, chains, output parsers |
| `langchain-huggingface` | LangChain's connector to Hugging Face models |
| `huggingface_hub` | The low-level client that talks to Hugging Face over HTTPS |

### Check

```bash
python -c "import streamlit, langchain_core, langchain_huggingface; print('ready')"
```

> **Discuss:** What would go wrong if two projects on the same computer needed different versions of
> the same library, without virtual environments?

---

## Step 2: A first web page with Streamlit

### Theory

Streamlit runs your Python file **from top to bottom**, and each `st.` call adds something to the page.
There's no HTML to write. Run it with `streamlit run`, which starts a small **web server** on your
computer and opens the page at `http://localhost:8501`:
- `localhost` means "this computer".
- `8501` is the **port**, the "door number" the server listens on.

### Build: `hello.py`

```python
import streamlit as st

st.title("Text Generation Playground")
st.write("Hello! This whole page is generated from a Python script.")
```

```bash
streamlit run hello.py
```

**First time only:** Streamlit asks for an **Email:** in the terminal and waits. The page won't load
until you answer. Press **Enter** to skip.

### Check

The page shows a title and one line of text. Change the text in `hello.py` and save: the page offers to
**Rerun**. Stop the server with **Ctrl+C** in the terminal.

> **Try this:** add `st.write(2 + 2)` and `st.write({"model": "Qwen3", "size": "8B"})`. What does
> `st.write` do with different kinds of values?

---

## Step 3: Add the controls

### Theory

**Widgets** are inputs: text boxes, sliders, buttons. A widget call *returns the current value*:
```python
temperature = st.slider("Temperature", 0.1, 1.0, 0.7)   # temperature is now a number, e.g. 0.7
```

**Streamlit's key rule: every interaction re-runs the whole script.** Typing in a box, moving a slider
or clicking a button runs the file again from the top. Each widget returns its new value on that run.
This is very different from most programs, and it's the most important idea in Streamlit.

`st.button` returns `True` only on the single re-run caused by the click, so code under
`if st.button(...)` runs once per click.

**The two model settings, in theory:**
- **Max new tokens** is the most tokens the model may generate for the answer. It's a ceiling: the model
  can stop earlier. It protects against very long, slow and expensive answers.
- **Temperature** controls randomness. At each step the model has a probability for every possible next
  token. Temperature reshapes those probabilities:
  - **Low (0.1–0.3):** the most likely tokens dominate. Answers are focused and nearly the same every
    time, good for facts and code.
  - **High (0.8–1.0):** less likely tokens get more chance. Answers are varied and creative, good for
    stories and brainstorming. Too high gives nonsense.

  The API doesn't allow exactly 0, so the slider starts at 0.1.

### Build: `ui.py`

```python
import streamlit as st

st.title("Text Generation Playground")

# Sidebar: settings
token = st.sidebar.text_input("Hugging Face API Token", type="password")   # shown as dots
max_new_tokens = st.sidebar.slider("Max new tokens", 50, 500, 200, 10)
temperature = st.sidebar.slider("Temperature", 0.1, 1.0, 0.7, 0.1)

# Main area: model and prompt
model = st.selectbox("Select a model:", ["Qwen/Qwen3-8B", "openai/gpt-oss-20b"])
prompt = st.text_area("Enter your prompt:", placeholder="Explain how a hash map works...")

if st.button("Generate Text"):
    st.write(f"Would send {prompt!r} to {model} (max {max_new_tokens} tokens, temperature {temperature}).")

st.caption(f"The script just ran from the top again. Token entered: {'yes' if token else 'no'}")
```

```bash
streamlit run ui.py
```

### Check

Move a slider: the caption at the bottom updates, which proves the whole script re-ran. Click the
button: the "Would send…" line appears, and it disappears when you move a slider again.

> **Try this:** print `import time; st.write(time.time())` at the top. Watch it change on every
> interaction.
>
> **Discuss:** Why does the "Would send…" message disappear when you move a slider?

---

## Step 4: Get an API token

### Theory

**What is an API?** An *Application Programming Interface* is a way for one program to use another
over a defined set of requests. Here, our script sends an **HTTPS request** to Hugging Face, like a
browser loading a page, but the reply is data (the generated text), not a web page.

**Why a token?** The API must know **who** is asking, to check permissions and charge usage to the right
account. A token is a long secret string sent with every request (in an `Authorization: Bearer hf_…`
header). **Anyone who has your token can use your account**, so treat it like a password.

**Permissions.** A *fine-grained* token only does what you allow. Calling models needs the permission
**"Make calls to Inference Providers"**, which is under *User permissions → Inference*.

**Gated models.** Some model owners (Meta for Llama, Google for Gemma) require you to accept their
licence first. This is called **gated** access. Open models like Qwen3 and gpt-oss need no approval.

**Credits.** Running models costs money. Free accounts get a small monthly allowance of credits; when
it's used up, requests fail until it renews or you add credits.

### Build

1. Create a free account at **https://huggingface.co/join**.
2. Go to **Settings → Access Tokens → Create new token**.
3. Choose **Fine-grained** and, under **User permissions → Inference**, tick
   **"Make calls to Inference Providers"**. Alternatively, choose the **Read** type, which includes it.
4. Copy the token (it starts with `hf_`) and keep it private.
5. Optional, for Llama or Gemma: open the model's page on huggingface.co and accept the licence. Track the
   status at **https://huggingface.co/settings/gated-repos**. Llama approval can take hours.

> **Teacher's note:** each learner needs **their own** account and token. Never share one token in a
> group; it's a password, and one shared free allowance runs out fast.
>
> **Discuss:** If you accidentally pushed your token to GitHub, what should you do? *(See answers.)*

---

## Step 5: Talk to a model directly

### Theory

Before using any framework, see the raw request. A chat request is just:
- **which model** to use,
- **a list of messages** (system + user),
- **settings** (`max_tokens`, `temperature`).

The reply contains the generated message and **usage**: how many tokens went in (the prompt) and came
out (the completion). Usage is what you pay for.

Hugging Face's router uses the same request format as OpenAI's Chat Completions API. That format has
become an industry standard, so these ideas transfer to almost any AI provider.

### Build: `first_call.py`

```python
import getpass

from huggingface_hub import InferenceClient

token = getpass.getpass("Hugging Face token (hidden): ")   # never hard-code a token in a file
client = InferenceClient(provider="auto", api_key=token)   # "auto": Hugging Face picks a provider

reply = client.chat_completion(
    model="Qwen/Qwen3-8B",
    messages=[
        {"role": "system", "content": "You are a concise assistant."},
        {"role": "user", "content": "What is a token in a language model? One sentence."},
    ],
    max_tokens=60,
    temperature=0.3,
)

print(reply.choices[0].message.content)
print("usage:", reply.usage)
```

```bash
python first_call.py
```

### Check

You see a one-sentence answer, and a usage line with the prompt, completion and total token counts.

> **Try this:**
> - Run it 3 times with `temperature=0.1`, then 3 times with `temperature=1.0`. Compare the answers.
> - Change the system message to "Answer like a pirate."
> - Set `max_tokens=10`. What happens to the answer?
>
> **Discuss:** The system message is invisible to the end user. What is it good for, and could it be misused?

---

## Step 6: Prompt templates and chains with LangChain

### Theory

Real apps repeat the same pattern: **fill a prompt → call a model → clean up the output**. LangChain
gives each part a reusable building block:

| Building block | Job |
|---|---|
| `ChatPromptTemplate` | A message list with **placeholders**, e.g. `{input_text}`, filled in for each request |
| `ChatHuggingFace` (wrapping `HuggingFaceEndpoint`) | The model: sends the messages to Hugging Face and returns the reply |
| `StrOutputParser` | Turns the model's reply object into a plain string |

**Chains with `|`.** LangChain Expression Language (LCEL) joins blocks with the pipe operator, like
pipes in a Unix shell. The output of each block becomes the input of the next:

```python
chain = prompt | model | StrOutputParser()
chain.invoke({"input_text": "..."})   # run once, get the whole answer
chain.stream({"input_text": "..."})   # get the answer piece by piece, as it's generated
```

**Why a framework at all?** You can swap any block (another model, another provider, a local model)
without changing the rest. Chains can also grow: add retrieval, memory, tools.

**Streaming.** Without streaming you wait for the *whole* answer, which can take many seconds. With
streaming, the provider sends each piece of text as soon as it's generated, so the user sees the
answer appear word by word. It's the same total time, but it *feels* much faster.

### Build: `chain.py`

```python
import getpass

from langchain_core.output_parsers import StrOutputParser
from langchain_core.prompts import ChatPromptTemplate
from langchain_huggingface import ChatHuggingFace, HuggingFaceEndpoint

token = getpass.getpass("Hugging Face token (hidden): ")

# 1. The template: fixed instructions + a placeholder for the user's text
prompt = ChatPromptTemplate.from_messages([
    ("system", "You are a friendly, knowledgeable writing assistant. Respond accurately and concisely."),
    ("human", "{input_text}"),
])
print("Messages sent to the model:", prompt.invoke({"input_text": "Explain recursion"}).to_messages())

# 2. The model
llm = HuggingFaceEndpoint(
    repo_id="Qwen/Qwen3-8B",
    task="text-generation",
    max_new_tokens=150,
    temperature=0.5,
    huggingfacehub_api_token=token,
    provider="auto",
)
model = ChatHuggingFace(llm=llm)

# 3. The chain, run with streaming
chain = prompt | model | StrOutputParser()
for piece in chain.stream({"input_text": "Explain recursion to a 10-year-old."}):
    print(piece, end="", flush=True)
print()
```

```bash
python chain.py
```

### Check

First you see the filled-in messages: your text has replaced `{input_text}`. Then the answer **types
itself out** in the terminal piece by piece. That's streaming.

> **Try this:** replace `chain.stream(...)` with `print(chain.invoke(...))` and feel the difference.
> Then change `repo_id` to `"openai/gpt-oss-20b"`: only one line changes.
>
> **Note:** Qwen3 is a "reasoning" model and may begin with a `<think>…</think>` section showing its
> reasoning. Adding `/no_think` to the end of a prompt asks Qwen3 to skip it.
>
> **Discuss:** Which line would you change to use a completely different AI company's model?

---

## Step 7: Put it together: the full app

### Theory

Now join step 3 (the UI) with step 6 (the chain):
- The widgets supply the **token, model, settings and prompt**.
- When the button is clicked, build the chain with those values and **stream** the answer onto the page.
  `st.write_stream()` takes the stream and shows each piece as it arrives.

The app also checks the inputs **before** calling the API, which saves time and credits.

### Build

The core of the app, which combines `ui.py` and `chain.py`:

```python
def build_chain(model_id, token, max_new_tokens, temperature):
    llm = HuggingFaceEndpoint(repo_id=model_id, task="text-generation", max_new_tokens=max_new_tokens,
                              temperature=temperature, huggingfacehub_api_token=token, provider="auto",
                              timeout=120)
    return PROMPT | ChatHuggingFace(llm=llm) | StrOutputParser()

if st.button("Generate Text", type="primary"):
    if not token:
        st.error("Please enter your Hugging Face API token in the sidebar.")
    elif not prompt.strip():
        st.error("Please enter a valid prompt.")
    else:
        chain = build_chain(selected_model, token, max_new_tokens, temperature)
        st.subheader("Generated Text:")
        with st.spinner("Generating text..."):
            st.write_stream(chain.stream({"input_text": prompt}))
```

The complete, finished version is [`app.py`](app.py) in this folder. Walk through it top to bottom
with your group: every section is labelled with the step it comes from.

```bash
streamlit run app.py
```

### Check

Paste your token, choose **Qwen/Qwen3-8B**, type a prompt, and click **Generate Text**. The answer
streams onto the page.

> **Try this:**
> - Ask the same question to two different models and compare the answers.
> - Ask for a story at temperature 0.2, then at 1.0.
>
> **Discuss:** Why build the chain inside the button's `if`, and not once at the top of the script?
> *(Hint: Step 3's rule.)*

---

## Step 8: Handle errors and keep secrets safe

### Theory

**Things will fail**: wrong tokens, missing permissions, no credits, busy servers. APIs report problems
with **HTTP status codes**, and a good app turns them into advice:

| Code | Meaning | What the user should do |
|---|---|---|
| **401** Unauthorized | The token is wrong or incomplete | Copy the whole token again, or create a new one |
| **402** Payment Required | Out of credits | Wait for the monthly reset, or add credits |
| **403** Forbidden | Not allowed: licence not approved, or the token lacks permission | Accept the licence; tick "Make calls to Inference Providers" |
| **404** / not supported | No enabled provider serves this model | Pick another model, or enable more providers |
| **429** Too Many Requests | Rate limit | Wait and try again |

The app wraps the call in `try / except`, and its `friendly_error()` function turns these codes into
plain advice, followed by Hugging Face's own message. The separate [`check_access.py`](check_access.py)
script tests a token and every model from the terminal.

**Keeping secrets safe:**
- **Never write a token in code.** Code gets shared, copied and committed.
- Ask for it at runtime instead: a password box (`type="password"`) or `getpass`.
- Or read it from a place that's never committed: `.streamlit/secrets.toml`, or the `HF_TOKEN`
  environment variable.
- **Never print or log it.**
- Add secret files to `.gitignore`. This project ignores `.streamlit/secrets.toml` and `.venv/`.

The app looks for a token in three places, in order: the **sidebar**, then **`secrets.toml`**, then
the **`HF_TOKEN`** environment variable.

### Check

Deliberately break things and read the messages:
- Enter `hf_wrong` as the token. You should get **401**.
- Leave the prompt empty. The app shows its own error and never calls the API.
- Pick a gated model before your licence is approved. You should get **403**.

> **Discuss:** Why show the user advice instead of the raw error? And why still include the raw
> message underneath?

---

## Step 9: Where to go next

| Idea | What you'd learn |
|---|---|
| **Chat history** with `st.chat_message` and `st.chat_input`, keeping past messages in `st.session_state` | How chatbots "remember": the whole conversation is re-sent every time |
| **Run a model locally** with Ollama or the `transformers` library | No credits or network; the trade-offs of model size, memory and speed |
| **Retrieval (RAG):** answer questions about your own PDFs or notes | Embeddings, vector search, giving a model facts it was never trained on |
| **Deploy** to Streamlit Community Cloud, with the token in its *Secrets* settings | Hosting, environment configuration, secrets in production |
| **Compare models side by side** in two columns (`st.columns`) | Model quality vs speed vs cost |

---

## Glossary

| Term | Meaning |
|---|---|
| **LLM** | Large language model: a neural network trained to predict the next token of text |
| **Token** | A piece of text (a word or part of a word); the unit models read, write and bill by |
| **Inference** | Running a trained model to produce output |
| **Chat / instruct model** | A model trained to follow instructions, using messages with roles |
| **System / user / assistant** | The three message roles: developer instructions, user input, model output |
| **Temperature** | How random the model's choice of next token is |
| **Max new tokens** | The upper limit on how long the answer may be |
| **Prompt template** | A reusable message layout with placeholders |
| **Chain (LCEL)** | Blocks joined with `\|`, where each one's output feeds the next |
| **Streaming** | Receiving the answer piece by piece as it's generated |
| **API / endpoint** | A URL a program sends requests to, and gets data back from |
| **API token** | A secret that identifies your account to an API |
| **Gated model** | A model that needs licence approval before use |
| **Inference provider** | A company that runs models on its GPUs for others |
| **Virtual environment** | A private set of libraries for one project |
| **localhost:8501** | "This computer, port 8501": where Streamlit serves the page |

---

## Session plan for teachers

**Before the session:** ask everyone to install Python 3.11+, create a Hugging Face account, and create
a token (Step 4). Account setup takes the longest on the day if left until then.

| Time | Part | Activity |
|---|---|---|
| 0:00 | Step 0 | Big picture: draw the architecture diagram on a whiteboard. Live-demo the finished app first, so people know where they're heading |
| 0:20 | Steps 1–2 | Everyone sets up the environment and runs `hello.py` (allow for the first-run "Email:" prompt) |
| 0:40 | Step 3 | Build `ui.py`. Make sure everyone *sees* the re-run rule with the caption or `time.time()` experiment |
| 1:05 | Step 4 | Check that everyone's token works; explain permissions, gating, credits |
| 1:15 | *Break* | |
| 1:25 | Step 5 | `first_call.py`: the temperature experiment, done in pairs, comparing outputs |
| 1:50 | Step 6 | `chain.py`: templates and streaming; swap the model by changing one line |
| 2:15 | Steps 7–8 | Run `app.py`; walk through the code; break it deliberately (wrong token, empty prompt) |
| 2:40 | Step 9 | Next ideas; each person picks one to try at home |

**Common problems on the day:**
- *Page doesn't load at localhost:8501:* the terminal is waiting at Streamlit's first-run **Email:** prompt. Press Enter.
- *`streamlit: command not found`:* the virtual environment isn't activated (`source .venv/bin/activate`).
- *403 on every model:* the token lacks "Make calls to Inference Providers". Edit the token's permissions.
- *402:* that account's free credits are used up. Pair the learner with someone else, or demo from the front.
- *"No provider" (404):* pick Qwen3 or gpt-oss, which have several providers.

---

## Answers to the discussion questions

**Step 0. Laptop vs hosted API?** A hosted API gives access to big, fast models with no setup or GPU,
but it costs money, needs internet, and sends your text to another company. Running locally is free,
private and works offline, but your machine limits the model size and speed.

**Step 1. Without virtual environments?** Installing one project's version would break the other.
Only one version of a library can be installed in one place at a time.

**Step 3. Why does the message disappear?** Moving a slider re-runs the script from the top. On that
run `st.button(...)` returns `False`, because the button wasn't clicked this time, so the `st.write`
inside the `if` doesn't run.

**Step 4. Leaked token?** Revoke (delete) it immediately on the Hugging Face tokens page and create a new
one. Removing it from the code isn't enough: it's still in the git history, and automated bots scan
GitHub for tokens within minutes.

**Step 5. The system message?** It sets behaviour, tone, format and rules ("answer in French", "never
give medical advice"). It can be misused to hide manipulative instructions, and users can try to
override it ("ignore previous instructions"), which is called *prompt injection*.

**Step 6. Use another company's model?** Only the model block changes, for example replacing
`ChatHuggingFace(...)` with another provider's LangChain chat model. The template, parser and chain
stay the same. That swap-ability is the point of the framework.

**Step 7. Why build the chain inside the `if`?** Because the script re-runs on every interaction.
Building it at the top would run on every slider move, before the user has even entered a token. Inside
the `if`, it's built once per click with the current settings.

**Step 8. Advice plus the raw error?** Advice tells a non-expert what to do next; the raw message gives
the exact detail needed to debug unusual cases or ask for help.
