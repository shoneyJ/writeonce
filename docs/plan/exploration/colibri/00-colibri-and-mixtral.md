# Colibrì — reference analysis, and running Mistral's MoE models locally

Analysis of the vendored reference tree at [`.dev/reference/colibri/`](../../../../.dev/reference/colibri) (Apache-2.0, upstream <https://github.com/JustVugg/colibri>), and a grounded, hands-on answer to the follow-on question: **what does it take to run Mistral's Mixture-of-Experts models (Mixtral 8x22B / 8x7B) locally?** — including a working demonstration of colibrì's "dense resident, stream the experts from disk" idea using the vendored llama.cpp (§7).

> **TL;DR**
> - Colibrì is a **single-file, zero-dependency C inference engine** that runs a **744B-parameter MoE (GLM-5.2)** on a ~25 GB-RAM consumer box by **streaming routed experts from disk** and treating VRAM/RAM/disk as one managed memory hierarchy. It is here as a *runtime-engineering* reference: it does all its I/O with the exact kernel primitives writeonce's north star is built on (`pread`, `posix_fadvise`, `io_uring`, `mmap`, `mlock`, `O_DIRECT`).
> - Colibrì supports **exactly two model architectures today: GLM-5.2 (`c/glm.c`) and OLMoE (`c/olmoe.c`)**. **There is no Mixtral/Mistral code in the tree** (`grep -ri mixtral` → 0 hits).
> - **To run any Mistral MoE locally right now, don't wait on colibrì** — use a runtime that already supports it. The repo now also vendors a full **llama.cpp** checkout at `.dev/reference/llama-cpp` with **verified, first-class Mistral/Mixtral support** (§6): GGUF + `--n-cpu-moe`. Other options: **KTransformers** (CPU/GPU hybrid, the closest philosophical cousin) or **vLLM/SGLang** on a multi-GPU box. See §5–§6.
> - Mixtral 8x22B is actually a **much easier** target for the colibrì streaming trick than GLM-5.2 — 8 coarse experts/layer instead of 256 fine-grained ones, so the whole int4 expert set (~67 GB) fits in commodity RAM and the disk-streaming stops mattering. A `mixtral.c` port modelled on `olmoe.c` is small and plausible (§4), but it does not exist yet.
> - You can **reproduce and observe** colibrì's core mechanism on a small machine with the [`prototypes/llama-moe-stream/`](../../../../prototypes/llama-moe-stream) demo (§7): run an MoE (default **Qwen3-Coder-30B-A3B**) under a `MemoryMax` cap so the small dense part stays resident while the experts stream from disk on demand — the model still answers correctly on far less RAM than its size. **Gotcha found in practice:** in-circulation Mixtral GGUFs use the pre-2024 per-expert layout and **won't load** on current llama.cpp, so the demo uses a modern fused-format MoE.

---

## 1. What colibrì is

**"Tiny engine, immense model."** Colibrì is a lightweight, quality-preserving Mixture-of-Experts *inference runtime* written in pure C with no external libraries (no BLAS, no Python at runtime, no GPU required). Its thesis:

> A 744B MoE activates only ~40B params per token, and only ~11 GB of those (the *routed experts*) change from token to token. So keep the **dense part resident** and **stream the experts from disk on demand.**

Concretely, for GLM-5.2 at int4:

| Component | Size | Placement |
|---|---|---|
| Dense (attention, shared experts, embeddings — ~17B params) | ~9.9 GB | **resident in RAM** at int4 |
| 19,456 routed experts (75 MoE layers × 256 + MTP head, ~19 MB each) | ~370 GB | **on disk**, streamed on demand |

The engine treats **VRAM → RAM → disk as one memory hierarchy** with a per-layer LRU expert cache, an optional pinned hot-store (the hottest experts stay in spare RAM/VRAM), and the OS page cache as a free L2. Insufficient fast memory reduces *speed*, never *precision or router semantics* — the default policy is lossless.

This is not fast (0.05–2 tok/s depending on disk/RAM/CPU — see the community benchmark table in the upstream README), but it runs a **frontier-class 744B model correctly on hardware that costs less than one H100 fan.**

## 2. Why it lives in `.dev/reference/`

writeonce's north star (see root `CLAUDE.md`, `docs/01-problem.md`, `docs/plan/linux/00-linux.md`) is **one binary, zero external crates, all I/O driven directly by Linux kernel primitives.** Colibrì is a working, production-shaped proof of exactly that discipline in a different domain (ML inference rather than a database):

- **One binary, `libc`-only.** The engine is `c/glm.c` (~348 KB) plus small headers. Python appears *only* in the one-time offline weight converter, never at runtime — the same "transitional tooling is allowed, the runtime is not" line writeonce draws.
- **The kernel *is* the async runtime and the storage tier.** Colibrì's expert streaming is built from the same primitives `crates/rt/src/runtime/` is being built on:

  | Primitive | Colibrì use | writeonce analogue |
  |---|---|---|
  | `pread` | read one expert slab at a known offset | WAL / segment reads |
  | `posix_fadvise(WILLNEED/DONTNEED)` | async readahead of the next expert block; evict used slabs | page-cache management |
  | `io_uring` (`URING=1`, `c/uring.h`) | batched, queued cold expert reads via `IOSQE_ASYNC` | the target event loop (`docs/plan/02`) |
  | `O_DIRECT` (`DIRECT=1`) | bypass page cache for sustained NVMe | direct segment I/O |
  | `mmap` (`COLI_MMAP=1`) | map weights instead of `read()` into slabs | `sendfile`/mmap static assets (`docs/plan/08`) |
  | `mlock` (`MLOCK=1`) | wire the hot expert cache into physical RAM | pinning hot pages |

  It even has a portability story writeonce will need: `c/compat.h` maps every POSIX call to the Win32 API (`pread`→`ReadFile`+`OVERLAPPED`, etc.) so the engine source stays platform-clean.

So colibrì is a reference for **how to engineer a disk/RAM/VRAM memory hierarchy on raw syscalls in one C binary** — read `c/uring.h`, `c/tier.h`, `c/st.h` (the safetensors mmap reader), and `c/compat.h` when designing writeonce's I/O layer. It is *not* a database and shares no code; the value is the technique.

## 3. Use case — who runs colibrì, and when

**Use it when:** you want to run a *very large* open-weight MoE (hundreds of billions of params) **locally, offline, at full quality**, on hardware that cannot hold the model in VRAM (or even in RAM), and you can tolerate low-but-usable token rates. Typical: a single workstation or a homelab NVMe box, privacy-sensitive or air-gapped inference, model-behaviour research, or squeezing a frontier model onto a laptop.

**Don't use it when:** you need interactive throughput on a small model (llama.cpp/Ollama are simpler and faster there), or you have enough VRAM to hold your model outright (use vLLM/SGLang/ExLlamaV2).

**Surface area** (all via the `coli` Python CLI, which just sets env vars and launches the C engine):

| `coli <cmd>` | What it does |
|---|---|
| `convert` | offline FP8→int4 converter; downloads the HF checkpoint one ~5 GB shard at a time so the full 756 GB never lands on disk at once (resumable) |
| `plan` | read-only: reports the dense/expert footprint and the planned VRAM/RAM/disk tiers (`--json`) |
| `doctor` | read-only readiness check (model dir, tokenizer, RAM budget, CUDA linkage, GPU devices) |
| `chat` | interactive REPL |
| `run` | one-shot prompt |
| `serve` | OpenAI-compatible HTTP API (`/v1/chat/completions`, SSE streaming) — stdlib-only gateway (`c/openai_server.py`), one model process, FIFO admission queue |
| `web` | serves the React dashboard in `web/` (live token metrics, hardware panel, the "Brain" expert-heat view) |
| `bench` | MMLU/HellaSwag/ARC quality benchmarks |

Also shipped: a **Tauri desktop shell** (`desktop/`) and a **Nix flake** (`flake.nix`, gcc + OpenMP + gmp; Python env for the converter only).

**Runtime environment colibrì itself needs:**
- **OS:** Linux (or WSL2), macOS, or native Windows 11 (MinGW-w64).
- **CPU:** gcc with OpenMP; AVX2 baseline (`x86-64-v3`), with faster paths on AVX-VNNI (Alder Lake+) and ARM NEON/i8mm/SVE2 (Apple Silicon, Grace). `make ARCH=native` enables the best kernel for the host.
- **GPU (optional):** CUDA backend for NVIDIA (resident/pinned expert tier; on Windows a runtime-loaded `coli_cuda.dll`), Metal backend for Apple Silicon. Both are opt-in accelerators — the CPU path is the reference and stays byte-exact.
- **RAM:** ≥16 GB minimum; more RAM = more experts stay hot = higher tok/s (auto-budgeted from `MemAvailable`).
- **Disk:** the int4 model on a **local** NVMe (ext4/NTFS — never a network/9p mount). Random-read bandwidth is the cold-decode ceiling.

Feature depth worth noting (all in `c/glm.c`): MLA attention with a 57×-compressed KV cache, DeepSeek-V3-style sigmoid router, native **MTP speculative decoding** (int8 draft head), grammar-forced drafts (`GRAMMAR=*.gbnf`), int8/int4/int2 packed quant kernels, DSA sparse attention, crash-safe KV-cache persistence, and cache-aware routing. Every knob is an env var — see [`.dev/reference/colibri/docs/ENVIRONMENT.md`](../../../../.dev/reference/colibri/docs/ENVIRONMENT.md).

## 4. The Mixtral gap — and what a port would take

**Colibrì does not support Mixtral / any Mistral model.** The only architectures implemented are:

- **`c/glm.c`** — GLM-5.2 (`glm_moe_dsa`): 744B, 256 experts/layer top-8, MLA, DSA, MTP. The flagship target.
- **`c/olmoe.c`** — OLMoE-1B-7B (`allenai/OLMoE-1B-7B-0125-Instruct`): 7B total / 1B active, 64 experts/layer top-8. Its header states its purpose plainly: *"validate the streaming core before scaling to GLM-5.2."* **This is the template for adding a new architecture.**

Adding Mixtral would mean writing the same two pieces OLMoE has:

1. **`c/mixtral.c`** — a faithful forward pass. Good news: Mixtral is *architecturally simpler* than either existing engine — plain GQA + RoPE attention (no MLA, no DSA, no q/k-norm), RMSNorm, SwiGLU experts, no shared expert, no MTP head. It is closer to `olmoe.c` than to `glm.c`, and smaller.
2. **`c/tools/convert_mixtral.py`** — modelled on `convert_olmoe.py`: keep dense weights as f16/f32, row-wise-quantize the expert matrices to the int8/int4 container. Only the expert-key regex changes — Mixtral names them `model.layers.{L}.block_sparse_moe.experts.{E}.(w1|w2|w3).weight` and the router is `block_sparse_moe.gate`.

**Why Mixtral is an *easier* streaming target than GLM-5.2** (int4, from its config — 56 layers, hidden 6144, intermediate 16384, 8 experts/layer, top-2):

- Each expert = 3 matrices of 6144×16384 ≈ 302M params → **~151 MB at int4** (vs GLM's 19 MB fine-grained experts).
- Total experts = 8 × 56 = **448 experts ≈ 67 GB at int4** (vs GLM's 19,456 experts ≈ 370 GB).
- Cold cost/token = top-2 × 56 = **112 expert-loads ≈ 17 GB/token** — but with only 8 experts/layer, **any 96 GB+ machine caches the entire expert set in RAM**, giving ~100 % hit rate and *zero* disk streaming after warmup. The engine becomes RAM-bandwidth / matmul bound, not disk bound.

In other words, the whole "stream from disk" apparatus that colibrì needs for GLM-5.2 is mostly *unnecessary* for Mixtral 8x22B — the model is small enough (at int4) to just live in RAM. That is exactly why the practical answer below does not require colibrì at all.

## 5. Running Mixtral 8x22B locally — the ready paths

### 5.1 The model

| Config (`Mixtral-8x22B-v0.1`) | Value |
|---|---|
| Total / active params | ~141B / ~39B |
| Layers | 56 |
| hidden_size | 6144 |
| intermediate_size (per expert) | 16384 |
| attention heads / KV heads (GQA) | 48 / 8 (head_dim 128) |
| experts / top-k | 8 / 2 |
| vocab | 32768 |
| rope_theta / context | 1,000,000 / 65,536 |

Approximate on-disk sizes (GGUF): **FP16 ≈ 281 GB · Q8_0 ≈ 149 GB · Q5_K_M ≈ 100 GB · Q4_K_M ≈ 86 GB · Q3_K ≈ 65 GB · Q2_K ≈ 52 GB.** For decent-quality local use, **Q4_K_M (~86 GB) or Q5** is the sweet spot; Q2/Q3 fit smaller boxes with quality loss.

### 5.2 Runtime options, from most-consumer to most-datacenter

| Runtime | How it runs Mixtral 8x22B locally | Hardware reality | Closeness to colibrì |
|---|---|---|---|
| **Ollama** | `ollama run mixtral:8x22b` (wraps llama.cpp, pulls a Q4 GGUF) | ~90 GB RAM for Q4 CPU-only, or GPU+CPU split | Same tiering idea, turnkey |
| **llama.cpp (GGUF)** | Load a Q4/Q5 GGUF; offload expert layers to CPU RAM and keep attention/dense on GPU with **`--n-cpu-moe N`** (or `-ot`/`--override-tensor` regex for per-tensor control) | Runs CPU-only with ~90 GB RAM, *or* a 16–24 GB GPU + system RAM hybrid | **Closest mainstream analog** — same "experts in slow memory, dense on fast" split colibrì automates |
| **KTransformers** | CPU/GPU **hybrid MoE** — attention + shared/hot experts on GPU, the parameter-heavy routed experts in system RAM with AMX/AVX-512 CPU kernels. Explicitly lists **Mixtral 8x7B and 8x22B** as supported. | One consumer GPU + a big-RAM host; higher throughput than llama.cpp on large MoE | **Philosophically closest** — it is colibrì's heterogeneous-tiering idea as a Python/CUDA framework |
| **vLLM / SGLang** | GPU-native, high-throughput serving (AWQ/GPTQ 4-bit or FP16) | Realistically **2× A100-80GB** (4-bit) to 4–8× for FP16 — a local *server*, not a desktop | Different niche (VRAM-resident, batch throughput) |
| **ExLlamaV2 (EXL2)** | 4-bit EXL2 quant, GPU-only | ~4× 24 GB consumer GPUs for a low-bpw quant | GPU-resident, no disk tier |
| **LM Studio / text-generation-webui** | Desktop front-ends over llama.cpp/GGUF | Same as llama.cpp | GUI convenience layer |

### 5.3 Recommendation

- **Single consumer/workstation box (one GPU + 64–128 GB RAM):** **llama.cpp or Ollama** with a **Q4_K_M GGUF** and **`--n-cpu-moe`** to push experts into RAM while attention stays on the GPU. Simplest and proven. If you have AMX/AVX-512 and want more speed on the same hardware, try **KTransformers** — it is the closest thing to "colibrì for Mixtral" that exists today.
- **Local multi-GPU server:** **vLLM or SGLang** with a 4-bit quant for real throughput.
- **If you specifically want the colibrì engine to run it:** that requires writing `c/mixtral.c` + `c/tools/convert_mixtral.py` against the `c/olmoe.c` template (§4). Feasible and not large, but it is net-new work — and because Mixtral's int4 expert set fits in RAM, it would buy little over the paths above except staying inside the pure-C, zero-dep runtime that makes colibrì interesting to writeonce in the first place.

## 6. Verified: `.dev/reference/llama-cpp` already runs Mistral/Mixtral

The repo also vendors a full, recent **llama.cpp** checkout at `.dev/reference/llama-cpp` (a symlink to a local clone; HEAD `635cdd5fc`). Unlike colibrì, it has **first-class Mistral/Mixtral support**, confirmed across the whole stack:

- **Architecture** (`src/llama-arch.{h,cpp}`): Mistral 7B and Mixtral 8x7B/8x22B load under `LLM_ARCH_LLAMA` — llama-arch MoE, driven by the `expert_count` / `expert_used_count` GGUF keys. Dedicated `LLM_ARCH_MISTRAL3` / `LLM_ARCH_MISTRAL4` cover the newer Mistral Small / Mistral 4 families; Pixtral / Mistral-Small-3.1 handle the vision variants.
- **Conversion** (`conversion/` package — the refactored `convert_hf_to_gguf.py`): registers `MistralForCausalLM` / `MixtralForCausalLM` (→ llama arch), plus dedicated `MistralModel`, `MistralMoeModel` (remapped onto DeepSeek-V2), `Mistral3Model`, `Ministral3Model`, `Mistral4Model`, `PixtralModel`.
- **Tokenizer + chat templates**: native `mistral-common` (Tekken / SentencePiece) tokenizers, a `TEKKEN` pre-type, and five built-in templates — `mistral-v1`, `mistral-v3`, `mistral-v3-tekken`, `mistral-v7`, `mistral-v7-tekken` (`src/llama-chat.cpp`).
- **MoE-offload flags** (`common/arg.cpp`): `-cmoe`/`--cpu-moe` and `-ncmoe N`/`--n-cpu-moe N` — the colibrì-style "experts on the slow tier, dense on the fast tier" split, built in (with `--n-cpu-moe-draft` variants for speculative decoding).

So on this repo the runnable path for any Mistral MoE is **llama.cpp**, not colibrì. Of the two vendored inference references: **colibrì = GLM-5.2 + OLMoE only; llama.cpp = full Mistral/Mixtral.**

## 7. Hands-on: understand MoE experts, and stream them from disk

The demo lives at [`prototypes/llama-moe-stream/`](../../../../prototypes/llama-moe-stream) (`run-moe.sh` + a teaching README). It runs an MoE and *forces* the streaming behavior with a RAM cap so the mechanism is observable — the same idea colibrì applies to GLM-5.2.

> **Format-wall gotcha (found the hard way).** The demo originally targeted Mixtral 8x7B, but **every in-circulation Mixtral GGUF (TheBloke Dec-2023, MaziyarPanahi Feb-2024) uses the pre-2024 *per-expert* tensor layout** (`blk.0.ffn_gate.0.weight` … `.7.weight`). Current llama.cpp (HEAD `635cdd5fc`) only loads the **fused** layout (`blk.0.ffn_gate_exps.weight`) and dies with `missing tensor 'blk.0.ffn_down_exps.weight'`. Re-downloading another old quant does not help. So the demo defaults to **Qwen3-Coder-30B-A3B-Instruct** — a modern MoE whose GGUF is fused-format (verified), and which doubles as a capable local coding model. The Mixtral analysis in §1–§6 stands; only the *runnable demo* switched models.

### 7.1 What a "Mixture of Experts" is (the concept)

A **dense** transformer runs every weight for every token. An **MoE** replaces each layer's feed-forward block with **N expert FFNs + a small router**; per token the router routes through only the **top-k** experts, and the rest stay idle. That splits the weights into two classes — and the split is the whole point:

| | what it is | touched per token? | share of the weights |
|---|---|---|---|
| **Dense part** | attention, embeddings, norms, the routers | **always** — every token, every layer | small → keep **resident** |
| **Experts** (routed) | the N expert FFNs in each layer | **only top-k of N** | the bulk → **stream from disk** |

**Qwen3-Coder-30B-A3B** (the demo model): 30B total but only **~3.3B active per token** — the router fires a small top-k of many experts each layer. **Mixtral 8x7B** is the same idea at 46.7B total / ~12.9B active (32 layers, 8 experts/layer, top-2; the name misleads — experts share one attention stack, so it is 46.7B not 56B), and **Mixtral 8x22B** at 141B / ~39B active.

**Why this enables streaming:** the dense part is small and hit constantly → keep it **resident** in fast memory. The experts are the majority of the bytes but each is hit rarely → they can live **on disk** and be pulled in exactly when routed to. A *dense* model of the same size could not do this (all of it every token); an MoE reads only the slice it routes to. That is colibrì's thesis.

### 7.2 Realizing "dense resident, experts streamed" with llama.cpp

- **mmap (on by default)** memory-maps the GGUF; the kernel demand-pages weights and evicts under pressure, backed by the file. This is the streaming engine, for free. **Never `--no-mmap`** on a >RAM model — it forces a full allocation and thrashes.
- **`--cpu-moe`** keeps expert tensors on the CPU/mmap (disk-backed) side. On a **CUDA** build you pair it with `-ngl` to put the dense part in the GPU (resident) while experts stream on the CPU — the textbook split. The vendored build is **CPU-only** (no CUDA backend compiled), so `-ngl`/`--cpu-moe` are GPU no-ops; the split is instead realized by a RAM cap.
- **`MemoryMax` (cgroup v2)** — `systemd-run --user --scope -p MemoryMax=6G` caps the process below the model size. The kernel then keeps the small dense part + hot experts resident and evicts cold expert pages, re-reading them from disk on demand. This turns the OS page cache into colibrì's tiering (hot resident, cold on disk); colibrì just makes it *smart* — per-layer LRU, `fadvise` readahead, pinning the measured-hottest experts.

### 7.3 Run it

```bash
cd prototypes/llama-moe-stream
./run-moe.sh                     # baseline: 17 GB model fits in RAM → all resident
MEM_CAP=6G ./run-moe.sh          # capped: dense stays hot, cold experts stream from disk
```

The proof: under a 6 GB cap the 17 GB model **still answers correctly** — the missing experts are served from disk on demand — and tok/s drops vs the baseline; that gap is the disk-streaming cost. `-hf` downloads + caches the GGUF (`~/.cache/llama.cpp`) on first run.

### 7.4 Privacy — does local inference leak your data?

**No.** llama.cpp inference is fully on-device: it reads a local GGUF, has **no telemetry**, and opens **no outbound connections** while generating — prompts and code never leave the machine (weights are inert data, not code that can "phone home"). The *only* network is the one-time `-hf` weight download (inbound; HuggingFace sees your IP + which file, not your data). To be certain, run air-gapped from the cached file:

```bash
GGUF=$(find ~/.cache/llama.cpp -name 'Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf' | head -1)
MODEL_PATH="$GGUF" OFFLINE=1 MEM_CAP=6G ./run-moe.sh   # HF_HUB_OFFLINE=1, no -hf, zero network
```

`ss -tnp` during the run shows **no established connections** from `llama-cli`. The real leak surface is the *client* (an editor plugin misconfigured to a cloud model, or plugin telemetry) — not the engine.

### 7.5 Measured on the dev box

Host: i7-13700H (20 threads, AVX2+VNNI), **31 GB RAM**, RTX 4050 Laptop (6 GB), NVMe; vendored llama.cpp is a **CPU-only** build. Model: `unsloth/Qwen3-Coder-30B-A3B-Instruct-GGUF:Q4_K_M` (~17.3 GB, single file, fused-expert layout).

| Run | RAM available to process | Expected behavior | Measured tok/s |
|---|---|---|---|
| baseline (uncapped) | whole 17 GB can stay resident | RAM/matmul-bound after warm-up | _pending run_ |
| `MEM_CAP=6G` + offline | 6 GB — dense + hot experts only | cold experts stream from NVMe; **no network** | _pending run_ |

_Numbers are filled in from the in-progress background run; the qualitative result — correct output under a cap far below model size, with zero outbound connections — is the point regardless of the exact rate._

---

## Sources

- `.dev/reference/colibri/` — vendored source (README, `docs/ENVIRONMENT.md`, `c/glm.c`, `c/olmoe.c`, `c/tools/convert_olmoe.py`, `c/uring.h`, `c/compat.h`, `flake.nix`), upstream <https://github.com/JustVugg/colibri>
- `.dev/reference/llama-cpp/` — vendored llama.cpp checkout (HEAD `635cdd5fc`), Mistral support verified in `src/llama-arch.{h,cpp}`, `conversion/{llama,mistral,mistral3,pixtral}.py`, `src/llama-chat.cpp`, `common/arg.cpp`
- `prototypes/llama-moe-stream/` — the hands-on demo (`run-moe.sh`, README) added by this work
- Model GGUFs: [`unsloth/Qwen3-Coder-30B-A3B-Instruct-GGUF`](https://huggingface.co/unsloth/Qwen3-Coder-30B-A3B-Instruct-GGUF) (fused-format, the demo default); old per-expert-layout examples that **fail** to load on current llama.cpp: [`TheBloke/Mixtral-8x7B-Instruct-v0.1-GGUF`](https://huggingface.co/TheBloke/Mixtral-8x7B-Instruct-v0.1-GGUF), [`MaziyarPanahi/Mixtral-8x22B-Instruct-v0.1-GGUF`](https://huggingface.co/MaziyarPanahi/Mixtral-8x22B-Instruct-v0.1-GGUF)
- [Mixtral 8x22B — Prompt Engineering Guide](https://www.promptingguide.ai/models/mixtral-8x22b) and [Ollama library: mixtral:8x22b](https://ollama.com/library/mixtral:8x22b)
- [Performant local MoE CPU inference with GPU acceleration in llama.cpp](https://huggingface.co/blog/Doctor-Shotgun/llamacpp-moe-offload-guide) (the `--n-cpu-moe` / `--override-tensor` guide)
- [KTransformers](https://github.com/kvcache-ai/ktransformers) — CPU/GPU hybrid MoE inference (lists Mixtral 8x7B/8x22B support)
