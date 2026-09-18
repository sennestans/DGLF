# MeshLLM

MeshLLM is a deliberately small proof of concept: a Mac advertises its local
LLM compute on a trusted LAN, and another computer discovers it and chats with
one of its models from a terminal.

## Build

Requirements:

- CMake 3.28 or newer, a C++20 compiler, and internet access for the first build
- macOS for the worker (the controller also builds on Linux and Windows)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/meshllm
```

On macOS, CMake downloads the pinned llama.cpp v0.4.1 source and builds
`llama-cli` alongside MeshLLM. Metal is enabled by llama.cpp, and MeshLLM
offloads supported model layers on Apple Silicon. A release build can take
several minutes the first time. Controller-only Linux and Windows builds skip
llama.cpp because they do not perform inference.

When the Mac has no `.gguf` files in `./models`, **Share this device** offers to
download the public Qwen3.5 0.8B Q4_0 starter model (about 563 MB). It shows the
choice before using the network or disk and stores the result in llama.cpp's
normal user cache. Later starts reuse that cache. You can therefore build and
run MeshLLM without installing llama.cpp or configuring a model directory
manually.

The defaults can be changed without a configuration file:

```sh
MESHLLM_MODELS=/path/to/gguf MESHLLM_LLAMA_CLI=/path/to/llama-cli ./build/meshllm
```

To make an advanced build that uses an existing `llama-cli` instead of fetching
llama.cpp:

```sh
cmake -S . -B build -DMESHLLM_BUNDLE_LLAMA_CPP=OFF
cmake --build build
```

Run the program on the Mac and choose **Share this device**. Run it on another
computer on the same LAN and choose **Find available devices**. UDP broadcast
discovery must be permitted on port 39554 and TCP connections on port 39555.

## v0 design and limitations

Discovery is a single UDP broadcast/reply exchange; chat uses a tiny
length-prefixed TCP protocol. Prompts and generated chunks never leave that
connection. The worker scans its configured model directory and can also use
the managed starter model from llama.cpp's cache. Model and prompt arguments
are passed directly to `llama-cli`, never through a shell. Output is streamed
back as llama.cpp produces it.

For simplicity, v0 starts `llama-cli` for each prompt, so conversations are
stateless and the model is reloaded on every turn. There is deliberately no
authentication, encryption, HTTP API, or internet-facing service. Use it only
on a trusted local network. The managed model requires internet access on its
first use; local GGUF files remain fully offline.
