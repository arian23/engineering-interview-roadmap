"""Checks why a Hugging Face model call is denied (403): token type and permissions, gated-model
approval, and a tiny real request to each model. The token is read without being shown or printed.

Run:  .venv/bin/python check_access.py
"""

import getpass
import os

from huggingface_hub import InferenceClient, auth_check, whoami
from huggingface_hub.errors import GatedRepoError, RepositoryNotFoundError

MODELS = ["Qwen/Qwen3-8B", "openai/gpt-oss-20b", "meta-llama/Llama-3.1-8B-Instruct", "google/gemma-3-27b-it"]

token = os.environ.get("HF_TOKEN") or getpass.getpass("Paste your Hugging Face token (hidden): ").strip()

# 1. Is the token valid, and what is it allowed to do?
try:
    info = whoami(token=token)
except Exception as error:
    raise SystemExit(f"Token rejected: {error}\n-> Create a new token at https://huggingface.co/settings/tokens")

access = (info.get("auth") or {}).get("accessToken") or {}
role = access.get("role", "?")
print(f"Account: {info.get('name')}   token type: {role}")
if role == "fineGrained":
    fine = access.get("fineGrained") or {}
    global_perms = fine.get("global") or []
    print(f"  global permissions: {', '.join(global_perms) or 'none'}")
    if not any("inference" in p for p in global_perms):
        print("  !! Missing 'Make calls to Inference Providers'. Edit the token and tick it.")
else:
    print("  (read/write tokens can call Inference Providers)")

# 2. Has the gated-model request been approved, and does a real call work?
for model in MODELS:
    try:
        auth_check(model, token=token)
        approval = "approved"
    except GatedRepoError:
        approval = "NOT approved yet (pending, rejected, or never requested)"
    except RepositoryNotFoundError:
        approval = "model not found"
    try:
        reply = InferenceClient(provider="auto", api_key=token).chat_completion(
            messages=[{"role": "user", "content": "Say OK"}], model=model, max_tokens=5)
        call = f"works: {reply.choices[0].message.content!r}"
    except Exception as error:
        call = f"fails: {str(error).splitlines()[0][:160]}"
    print(f"\n{model}\n  licence access: {approval}\n  test request:   {call}")

print("\nCheck the status of your requests at https://huggingface.co/settings/gated-repos")
