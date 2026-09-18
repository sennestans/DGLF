# MeshLLM

MeshLLM is a deliberately small proof of concept: a Mac advertises its local
`llama.cpp` installation on a trusted LAN, and another computer discovers it
and chats with one of its local GGUF models from a terminal.

## Build

Requirements:

- CMake 3.20 or newer and a C++20 compiler
- macOS for the worker (the controller also builds on Linux and Windows)
- [`llama-cli`](https://github.com/ggml-org/llama.cpp) installed on the worker
- one or more `.gguf` files in `./models`

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/meshllm
```

On Apple Silicon, install or build llama.cpp with Metal enabled (the normal
macOS default). MeshLLM passes `-ngl 99` so supported model layers are offloaded
to Metal.

The defaults can be changed without a configuration file:

```sh
MESHLLM_MODELS=/path/to/gguf MESHLLM_LLAMA_CLI=/path/to/llama-cli ./build/meshllm
```

Run the program on the Mac and choose **Share this device**. Run it on another
computer on the same LAN and choose **Find available devices**. UDP broadcast
discovery must be permitted on port 39554 and TCP connections on port 39555.

## v0 design and limitations

Discovery is a single UDP broadcast/reply exchange; chat uses a tiny
length-prefixed TCP protocol. Prompts and generated chunks never leave that
connection. The worker scans only its configured model directory and passes a
selected path to `llama-cli` as an argument (not through a shell). Output is
streamed back as llama.cpp produces it.

For simplicity, v0 starts `llama-cli` for each prompt, so conversations are
stateless and the model is reloaded on every turn. There is deliberately no
authentication, encryption, model downloading, HTTP API, or internet-facing
service. Use it only on a trusted local network.
