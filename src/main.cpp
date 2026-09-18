#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
using Socket = int;
constexpr Socket kInvalidSocket = -1;
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#endif

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

constexpr std::uint16_t kDiscoveryPort = 39554;
constexpr std::uint16_t kWorkerPort = 39555;
constexpr std::size_t kMaxFrame = 1024 * 1024;
constexpr std::string_view kDiscover = "MESHLLM_DISCOVER_V1";
constexpr std::string_view kAdvertise = "MESHLLM_WORKER_V1";
constexpr std::string_view kRecommendedModelName = "Qwen3.5 0.8B (Q4_0, managed)";
constexpr std::string_view kRecommendedModelRepo = "ggml-org/Qwen3.5-0.8B-GGUF:Q4_0";

enum class Message : std::uint8_t {
    info = 1, list_models = 2, models = 3, select_model = 4, selected = 5,
    prompt = 6, token = 7, done = 8, error = 9, quit = 10,
};

struct NodeInfo {
    std::string name;
    std::string device;
    std::uint64_t memory_bytes{};
    std::string backend;
};

struct WorkerInfo : NodeInfo {
    std::string address;
    std::uint16_t port{};
};

struct ModelOption {
    std::string display_name;
    std::string source;
    bool from_hugging_face{};
};

fs::path g_executable_directory;

void close_socket(Socket socket) {
    if (socket == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

struct SocketGuard {
    Socket value{kInvalidSocket};
    explicit SocketGuard(Socket socket = kInvalidSocket) : value(socket) {}
    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;
    ~SocketGuard() { close_socket(value); }
};

void initialize_sockets() {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw std::runtime_error("could not initialize Winsock");
    }
#endif
}

bool send_all(Socket socket, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const char*>(data);
    while (size > 0) {
#ifdef _WIN32
        const int sent = send(socket, bytes, static_cast<int>(size), 0);
#else
        // SIGPIPE is ignored in main, so zero flags are portable across macOS
        // and other POSIX controllers.
        const auto sent = send(socket, bytes, size, 0);
#endif
        if (sent <= 0) return false;
        bytes += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool receive_all(Socket socket, void* data, std::size_t size) {
    auto* bytes = static_cast<char*>(data);
    while (size > 0) {
#ifdef _WIN32
        const int received = recv(socket, bytes, static_cast<int>(size), 0);
#else
        const auto received = recv(socket, bytes, size, 0);
#endif
        if (received <= 0) return false;
        bytes += received;
        size -= static_cast<std::size_t>(received);
    }
    return true;
}

// Every TCP message has a one-byte kind and a network-order payload length.
// Framing avoids depending on prompt text or generated output containing lines.
bool send_frame(Socket socket, Message kind, std::string_view payload = {}) {
    const auto length = htonl(static_cast<std::uint32_t>(payload.size()));
    const auto byte = static_cast<std::uint8_t>(kind);
    return send_all(socket, &byte, sizeof(byte)) &&
           send_all(socket, &length, sizeof(length)) &&
           send_all(socket, payload.data(), payload.size());
}

std::optional<std::pair<Message, std::string>> receive_frame(Socket socket) {
    std::uint8_t kind{};
    std::uint32_t network_length{};
    if (!receive_all(socket, &kind, sizeof(kind)) ||
        !receive_all(socket, &network_length, sizeof(network_length))) return std::nullopt;
    const auto length = ntohl(network_length);
    if (length > kMaxFrame) return std::nullopt;
    std::string payload(length, '\0');
    if (length && !receive_all(socket, payload.data(), length)) return std::nullopt;
    return std::pair{static_cast<Message>(kind), std::move(payload)};
}

std::vector<std::string> split(std::string_view input, char delimiter) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= input.size()) {
        const auto end = input.find(delimiter, start);
        result.emplace_back(input.substr(start, end == std::string_view::npos ? input.size() - start : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

std::string safe_field(std::string value) {
    std::replace(value.begin(), value.end(), '\t', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    return value;
}

std::string hostname() {
    std::array<char, 256> value{};
    if (gethostname(value.data(), static_cast<int>(value.size() - 1)) == 0) return value.data();
    return "Mac";
}

fs::path executable_directory(const char* argv0) {
    std::error_code error;
#ifdef __APPLE__
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) == 0) {
        return fs::weakly_canonical(path.data(), error).parent_path();
    }
#endif
    const fs::path path = argv0 ? argv0 : "meshllm";
    return fs::weakly_canonical(fs::absolute(path, error), error).parent_path();
}

[[maybe_unused]] std::string find_llama_cli() {
    if (const char* configured = std::getenv("MESHLLM_LLAMA_CLI")) return configured;

    const fs::path installed_sibling = g_executable_directory / "llama-cli";
    std::error_code error;
    if (fs::is_regular_file(installed_sibling, error)) return installed_sibling.string();

#ifdef MESHLLM_BUNDLED_LLAMA_CLI
    if (fs::is_regular_file(MESHLLM_BUNDLED_LLAMA_CLI, error)) return MESHLLM_BUNDLED_LLAMA_CLI;
#endif
    // This fallback keeps custom builds with MESHLLM_BUNDLE_LLAMA_CPP=OFF useful.
    return "llama-cli";
}

#ifdef __APPLE__
std::string sysctl_string(const char* key) {
    std::size_t size = 0;
    if (sysctlbyname(key, nullptr, &size, nullptr, 0) != 0 || size == 0) return {};
    std::string value(size, '\0');
    if (sysctlbyname(key, value.data(), &size, nullptr, 0) != 0) return {};
    while (!value.empty() && value.back() == '\0') value.pop_back();
    return value;
}
#endif

[[maybe_unused]] NodeInfo local_node_info() {
    NodeInfo result;
    result.name = hostname();
#ifdef __APPLE__
    result.device = sysctl_string("machdep.cpu.brand_string");
    if (result.device.empty()) result.device = sysctl_string("hw.model");
    std::uint64_t memory = 0;
    std::size_t size = sizeof(memory);
    if (sysctlbyname("hw.memsize", &memory, &size, nullptr, 0) == 0) result.memory_bytes = memory;
#if defined(__arm64__) || defined(__aarch64__)
    result.backend = "Metal";
#else
    result.backend = "CPU";
#endif
#else
    result.device = "Unsupported worker platform";
    result.backend = "N/A";
#endif
    return result;
}

std::string serialize_node(const NodeInfo& node) {
    return safe_field(node.name) + '\t' + safe_field(node.device) + '\t' +
           std::to_string(node.memory_bytes) + '\t' + safe_field(node.backend);
}

std::vector<fs::path> scan_models(const fs::path& directory) {
    std::vector<fs::path> models;
    std::error_code error;
    if (!fs::is_directory(directory, error)) return models;
    for (const auto& entry : fs::directory_iterator(directory, error)) {
        if (entry.is_regular_file(error) && entry.path().extension() == ".gguf") models.push_back(entry.path());
    }
    std::sort(models.begin(), models.end(), [](const auto& a, const auto& b) {
        return a.filename().string() < b.filename().string();
    });
    return models;
}

std::vector<ModelOption> available_models(const fs::path& directory, bool include_managed_model) {
    std::vector<ModelOption> result;
    for (const auto& path : scan_models(directory)) {
        result.push_back({path.filename().string(), path.string(), false});
    }
    if (include_managed_model) {
        result.push_back({std::string(kRecommendedModelName), std::string(kRecommendedModelRepo), true});
    }
    return result;
}

std::string model_names(const std::vector<ModelOption>& models) {
    std::string result;
    for (const auto& model : models) {
        if (!result.empty()) result += '\n';
        result += model.display_name;
    }
    return result;
}

#ifndef _WIN32
[[maybe_unused]] bool download_recommended_model(const std::string& executable) {
    const pid_t child = fork();
    if (child == -1) return false;
    if (child == 0) {
        // Download progress and errors remain on stderr. Only the one-token
        // warm-up response is hidden from the worker's setup screen.
        const int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) dup2(null_fd, STDOUT_FILENO);
        std::vector<std::string> arguments{executable, "-hf", std::string(kRecommendedModelRepo),
            "-p", "Ready", "-n", "1", "--no-display-prompt", "--no-warmup", "--no-mmproj"};
        std::vector<char*> argv;
        for (auto& argument : arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);
        execvp(executable.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool generate_with_llama(Socket client, const ModelOption& model, const std::string& prompt,
                         const std::string& executable, bool use_metal) {
    int output_pipe[2];
    if (pipe(output_pipe) != 0) return send_frame(client, Message::error, "Could not create llama.cpp output pipe.");
    const pid_t child = fork();
    if (child == -1) {
        close(output_pipe[0]); close(output_pipe[1]);
        return send_frame(client, Message::error, "Could not start llama.cpp.");
    }
    if (child == 0) {
        dup2(output_pipe[1], STDOUT_FILENO);
        const int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) dup2(null_fd, STDERR_FILENO);
        close(output_pipe[0]); close(output_pipe[1]);

        // llama-cli is invoked directly (never through a shell). Its stdout pipe
        // naturally provides streaming chunks without coupling to llama.cpp APIs.
        std::vector<std::string> arguments{executable};
        if (model.from_hugging_face) {
            arguments.insert(arguments.end(), {"-hf", model.source, "--no-mmproj"});
        } else {
            arguments.insert(arguments.end(), {"-m", model.source});
        }
        arguments.insert(arguments.end(), {"-p", prompt, "-n", "512", "--simple-io",
            "--no-display-prompt", "--no-warmup"});
        if (use_metal) { arguments.emplace_back("-ngl"); arguments.emplace_back("99"); }
        std::vector<char*> argv;
        for (auto& argument : arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);
        execvp(executable.c_str(), argv.data());
        _exit(127);
    }

    close(output_pipe[1]);
    std::array<char, 1024> buffer{};
    bool produced_output = false;
    for (;;) {
        const auto count = read(output_pipe[0], buffer.data(), buffer.size());
        if (count <= 0) break;
        produced_output = true;
        if (!send_frame(client, Message::token, std::string_view(buffer.data(), static_cast<std::size_t>(count)))) {
            kill(child, SIGTERM);
            break;
        }
    }
    close(output_pipe[0]);
    int status = 0;
    waitpid(child, &status, 0);
    if (!produced_output && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
        return send_frame(client, Message::error,
            "llama-cli failed. Check MESHLLM_LLAMA_CLI and the GGUF model.");
    }
    return send_frame(client, Message::done);
}
#endif

[[maybe_unused]] void serve_client(Socket client, NodeInfo node, fs::path model_directory,
                                   std::string llama_cli, bool include_managed_model) {
    SocketGuard guard(client);
    if (!send_frame(client, Message::info, serialize_node(node))) return;
    std::optional<ModelOption> selected_model;
    while (const auto frame = receive_frame(client)) {
        const auto& [kind, payload] = *frame;
        if (kind == Message::list_models) {
            if (!send_frame(client, Message::models,
                            model_names(available_models(model_directory, include_managed_model)))) return;
        } else if (kind == Message::select_model) {
            selected_model.reset();
            for (const auto& model : available_models(model_directory, include_managed_model)) {
                if (model.display_name == payload) selected_model = model;
            }
            if (!selected_model) {
                if (!send_frame(client, Message::error, "The selected model is no longer available.")) return;
            } else if (!send_frame(client, Message::selected)) return;
        } else if (kind == Message::prompt) {
            if (!selected_model) {
                if (!send_frame(client, Message::error, "Select a model first.")) return;
                continue;
            }
#ifdef _WIN32
            if (!send_frame(client, Message::error, "Workers are supported on macOS in v0.")) return;
#else
            if (!generate_with_llama(client, *selected_model, payload, llama_cli, node.backend == "Metal")) return;
#endif
        } else if (kind == Message::quit) return;
    }
}

// A worker listens for a fixed discovery datagram and replies directly to the
// sender. This is portable, dependency-free, and sufficient for one LAN subnet.
[[maybe_unused]] void discovery_responder(std::atomic_bool& running, const NodeInfo& node) {
    SocketGuard socket(::socket(AF_INET, SOCK_DGRAM, 0));
    if (socket.value == kInvalidSocket) return;
    int reuse = 1;
    setsockopt(socket.value, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kDiscoveryPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) return;
#ifdef _WIN32
    DWORD timeout = 500;
#else
    timeval timeout{0, 500000};
#endif
    setsockopt(socket.value, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    const std::string reply = std::string(kAdvertise) + '\t' + std::to_string(kWorkerPort) + '\t' + serialize_node(node);
    while (running) {
        std::array<char, 256> buffer{};
        sockaddr_in peer{};
#ifdef _WIN32
        int peer_size = sizeof(peer);
#else
        socklen_t peer_size = sizeof(peer);
#endif
        const auto count = recvfrom(socket.value, buffer.data(), static_cast<int>(buffer.size()), 0,
                                    reinterpret_cast<sockaddr*>(&peer), &peer_size);
        if (count > 0 && std::string_view(buffer.data(), static_cast<std::size_t>(count)) == kDiscover) {
            sendto(socket.value, reply.data(), static_cast<int>(reply.size()), 0,
                   reinterpret_cast<sockaddr*>(&peer), peer_size);
        }
    }
}

void run_worker() {
#ifndef __APPLE__
    std::cout << "\nSharing a device is supported on macOS only in MeshLLM v0.\n";
    return;
#else
    const NodeInfo node = local_node_info();
    const fs::path models = std::getenv("MESHLLM_MODELS") ? std::getenv("MESHLLM_MODELS") : "models";
    const std::string llama_cli = find_llama_cli();
    bool include_managed_model = false;
    if (scan_models(models).empty()) {
        std::cout << "\nNo local GGUF models were found in " << fs::absolute(models).string() << ".\n\n"
                  << "MeshLLM can download its recommended starter model:\n"
                  << "  Qwen3.5 0.8B, Q4_0 (Apache 2.0, approximately 600 MB)\n\n"
                  << "It will be stored in llama.cpp's user cache. Download now? [y/N]: " << std::flush;
        std::string answer;
        if (!std::getline(std::cin, answer) || (answer != "y" && answer != "Y")) {
            std::cout << "Worker startup cancelled. Add a .gguf file or accept the managed model.\n";
            return;
        }
        std::cout << "\nDownloading and checking the model. This can take a few minutes...\n";
        if (!download_recommended_model(llama_cli)) {
            std::cout << "\nThe model download failed. Check the internet connection and available disk space.\n";
            return;
        }
        include_managed_model = true;
        std::cout << "\nModel is ready.\n";
    }
    SocketGuard listener(::socket(AF_INET, SOCK_STREAM, 0));
    if (listener.value == kInvalidSocket) throw std::runtime_error("could not create worker socket");
    int reuse = 1;
    setsockopt(listener.value, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kWorkerPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener.value, 8) != 0) {
        throw std::runtime_error("could not listen on TCP port " + std::to_string(kWorkerPort));
    }
    std::atomic_bool running{true};
    std::thread(discovery_responder, std::ref(running), node).detach();
    std::cout << "\nMeshLLM Worker\n\n"
              << "Node: " << node.name << '\n' << "Device: " << node.device << '\n'
              << "Backend: " << node.backend << '\n'
              << "Memory: " << (node.memory_bytes / (1024ull * 1024ull * 1024ull)) << " GB\n"
              << "Models: " << available_models(models, include_managed_model).size() << " available\n\n"
              << "Device is now available on the local network.\n\nWaiting for connections...\n";
    for (;;) {
        const Socket client = accept(listener.value, nullptr, nullptr);
        if (client != kInvalidSocket) {
            std::thread(serve_client, client, node, models, llama_cli, include_managed_model).detach();
        }
    }
#endif
}

std::vector<WorkerInfo> discover_workers() {
    SocketGuard socket(::socket(AF_INET, SOCK_DGRAM, 0));
    if (socket.value == kInvalidSocket) throw std::runtime_error("could not create discovery socket");
    int enabled = 1;
    setsockopt(socket.value, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(kDiscoveryPort);
    destination.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(socket.value, kDiscover.data(), static_cast<int>(kDiscover.size()), 0,
           reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
    // Also probe localhost so the complete workflow can be used and tested on
    // one Mac; some network stacks do not reflect limited broadcasts locally.
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(socket.value, kDiscover.data(), static_cast<int>(kDiscover.size()), 0,
           reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
#ifdef _WIN32
    DWORD timeout = 400;
#else
    timeval timeout{0, 400000};
#endif
    setsockopt(socket.value, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::map<std::string, WorkerInfo> workers;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        std::array<char, 1024> buffer{};
        sockaddr_in peer{};
#ifdef _WIN32
        int peer_size = sizeof(peer);
#else
        socklen_t peer_size = sizeof(peer);
#endif
        const auto count = recvfrom(socket.value, buffer.data(), static_cast<int>(buffer.size() - 1), 0,
                                    reinterpret_cast<sockaddr*>(&peer), &peer_size);
        if (count <= 0) continue;
        const auto fields = split(std::string_view(buffer.data(), static_cast<std::size_t>(count)), '\t');
        if (fields.size() != 6 || fields[0] != kAdvertise) continue;
        WorkerInfo worker;
        try {
            worker.port = static_cast<std::uint16_t>(std::stoul(fields[1]));
            worker.memory_bytes = std::stoull(fields[4]);
        } catch (...) { continue; }
        worker.name = fields[2]; worker.device = fields[3]; worker.backend = fields[5];
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        worker.address = ip;
        workers[worker.address + ':' + std::to_string(worker.port)] = std::move(worker);
    }
    std::vector<WorkerInfo> result;
    for (auto& [key, worker] : workers) { (void)key; result.push_back(std::move(worker)); }
    return result;
}

std::optional<std::size_t> choose(std::size_t count, std::string_view label) {
    std::cout << label;
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    try {
        const auto number = std::stoul(line);
        if (number >= 1 && number <= count) return number - 1;
    } catch (...) {}
    std::cout << "Invalid selection.\n";
    return std::nullopt;
}

Socket connect_to(const WorkerInfo& worker) {
    Socket socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket == kInvalidSocket) return kInvalidSocket;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(worker.port);
    if (inet_pton(AF_INET, worker.address.c_str(), &address.sin_addr) != 1 ||
        connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close_socket(socket); return kInvalidSocket;
    }
    return socket;
}

void chat_with_worker(const WorkerInfo& worker) {
    SocketGuard socket(connect_to(worker));
    if (socket.value == kInvalidSocket) { std::cout << "Could not connect to " << worker.address << ".\n"; return; }
    const auto info = receive_frame(socket.value);
    if (!info || info->first != Message::info) { std::cout << "The device did not speak the MeshLLM protocol.\n"; return; }
    std::cout << "\nConnected to " << worker.name << ".\n\nAvailable models:\n\n";
    if (!send_frame(socket.value, Message::list_models)) return;
    const auto response = receive_frame(socket.value);
    if (!response || response->first != Message::models) return;
    const auto models = response->second.empty() ? std::vector<std::string>{} : split(response->second, '\n');
    if (models.empty()) { std::cout << "No .gguf models were found on the worker.\n"; return; }
    for (std::size_t i = 0; i < models.size(); ++i) std::cout << '[' << i + 1 << "] " << models[i] << '\n';
    std::cout << '\n';
    const auto selection = choose(models.size(), "Select model: ");
    if (!selection || !send_frame(socket.value, Message::select_model, models[*selection])) return;
    const auto selected = receive_frame(socket.value);
    if (!selected || selected->first != Message::selected) {
        std::cout << (selected ? selected->second : "Connection closed.") << '\n'; return;
    }
    std::cout << "\nModel selected. Type /quit to leave.\n\n";
    for (;;) {
        std::cout << "You: " << std::flush;
        std::string prompt;
        if (!std::getline(std::cin, prompt) || prompt == "/quit") break;
        if (prompt.empty()) continue;
        if (!send_frame(socket.value, Message::prompt, prompt)) break;
        std::cout << "\nAssistant: " << std::flush;
        bool finished = false;
        while (!finished) {
            const auto frame = receive_frame(socket.value);
            if (!frame) { std::cout << "\nConnection closed.\n"; return; }
            if (frame->first == Message::token) std::cout << frame->second << std::flush;
            else if (frame->first == Message::done) finished = true;
            else if (frame->first == Message::error) { std::cout << "\nError: " << frame->second; finished = true; }
        }
        std::cout << "\n\n";
    }
    send_frame(socket.value, Message::quit);
}

void find_devices() {
    std::cout << "\nSearching for MeshLLM devices...\n" << std::flush;
    const auto workers = discover_workers();
    if (workers.empty()) {
        std::cout << "\nNo devices found. Check that both computers are on the same LAN and allow UDP port "
                  << kDiscoveryPort << ".\n"; return;
    }
    std::cout << '\n';
    for (std::size_t i = 0; i < workers.size(); ++i) {
        const auto& worker = workers[i];
        std::cout << '[' << i + 1 << "] " << worker.name << '\n' << "    " << worker.device << '\n'
                  << "    " << (worker.memory_bytes / (1024ull * 1024ull * 1024ull)) << " GB memory\n"
                  << "    " << worker.address << "\n\n";
    }
    const auto selection = choose(workers.size(), "Select device: ");
    if (selection) chat_with_worker(workers[*selection]);
}

} // namespace

int main(int argc, char** argv) {
    try {
        (void)argc;
        g_executable_directory = executable_directory(argv[0]);
        initialize_sockets();
#ifndef _WIN32
        signal(SIGPIPE, SIG_IGN);
#endif
        for (;;) {
            std::cout << "\nMeshLLM\n\n1. Share this device\n2. Find available devices\n3. Exit\n\nSelect: ";
            std::string choice;
            if (!std::getline(std::cin, choice) || choice == "3") break;
            if (choice == "1") run_worker();
            else if (choice == "2") find_devices();
            else std::cout << "Invalid selection.\n";
        }
#ifdef _WIN32
        WSACleanup();
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MeshLLM error: " << error.what() << '\n';
        return 1;
    }
}
