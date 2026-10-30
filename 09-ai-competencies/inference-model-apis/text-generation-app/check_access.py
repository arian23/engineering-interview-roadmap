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
    # Fine-grained permissions come in two lists: "global" ones, and ones scoped to an entity. On the token page,
    # "Make calls to Inference Providers" is under "User permissions", so it appears in the scoped list for your user.
    fine = access.get("fineGrained") or {}
    global_perms = fine.get("global") or []
    scoped = fine.get("scoped") or []
    print(f"  global permissions: {', '.join(global_perms) or 'none'}")
    all_perms = list(global_perms)
    for entry in scoped:
        entity = entry.get("entity") or {}
        perms = entry.get("permissions") or []
        all_perms += perms
        print(f"  {entity.get('type', '?')} '{entity.get('name', '?')}' permissions: {', '.join(perms) or 'none'}")
    if any(p.startswith("inference.") for p in all_perms):
        print("  OK: 'Make calls to Inference Providers' is enabled")
    else:
        print("  !! Missing 'Make calls to Inference Providers'. Edit the token: User permissions -> Inference.")
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
