#include "oauth.h"

#include <Geode/platform/cplatform.h>

#if defined(GEODE_IS_WINDOWS)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
#elif defined(GEODE_IS_MACOS)
    #include <cerrno>
    #include <netinet/in.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <unistd.h>
#else
    #error "AutoDeafen's OAuth callback server supports Geode's Windows and macOS targets only"
#endif

#include <Geode/Geode.hpp>
#include <Geode/utils/random.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "helpers.h"

namespace {

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

constexpr auto kCallbackTimeout = std::chrono::minutes(5);
constexpr auto kRequestTimeout = std::chrono::seconds(10);
constexpr auto kListenerPollInterval = std::chrono::milliseconds(200);
constexpr auto kShutdownDrainTimeout = std::chrono::seconds(1);
constexpr std::size_t kMaximumRequestSize = 16 * 1024;

#if defined(GEODE_IS_WINDOWS)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

int socketError() {
#if defined(GEODE_IS_WINDOWS)
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool socketErrorWasInterrupted() {
#if defined(GEODE_IS_WINDOWS)
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

bool socketErrorWouldBlock() {
#if defined(GEODE_IS_WINDOWS)
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

void closeNativeSocket(NativeSocket socket) {
    if (socket == kInvalidSocket) return;

#if defined(GEODE_IS_WINDOWS)
    closesocket(socket);
#else
    close(socket);
#endif
}

class SocketRuntime {
public:
    SocketRuntime() {
#if defined(GEODE_IS_WINDOWS)
        WSADATA data {};
        m_ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
        m_ready = true;
#endif
    }

    ~SocketRuntime() {
        reset();
    }

    SocketRuntime(const SocketRuntime&) = delete;
    SocketRuntime& operator=(const SocketRuntime&) = delete;

    SocketRuntime(SocketRuntime&& other) noexcept
      : m_ready(std::exchange(other.m_ready, false)) {}

    SocketRuntime& operator=(SocketRuntime&& other) noexcept {
        if (this != &other) {
            reset();
            m_ready = std::exchange(other.m_ready, false);
        }
        return *this;
    }

    explicit operator bool() const {
        return m_ready;
    }

    void reset() {
        if (!m_ready) return;

#if defined(GEODE_IS_WINDOWS)
        WSACleanup();
#endif
        m_ready = false;
    }

private:
    bool m_ready = false;
};

class SocketHandle {
public:
    SocketHandle() = default;
    explicit SocketHandle(NativeSocket socket) : m_socket(socket) {}

    ~SocketHandle() {
        reset();
    }

    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    SocketHandle(SocketHandle&& other) noexcept
      : m_socket(std::exchange(other.m_socket, kInvalidSocket)) {}

    SocketHandle& operator=(SocketHandle&& other) noexcept {
        if (this != &other) {
            reset();
            m_socket = std::exchange(other.m_socket, kInvalidSocket);
        }
        return *this;
    }

    explicit operator bool() const {
        return m_socket != kInvalidSocket;
    }

    NativeSocket get() const {
        return m_socket;
    }

    void reset() {
        closeNativeSocket(std::exchange(m_socket, kInvalidSocket));
    }

private:
    NativeSocket m_socket = kInvalidSocket;
};

bool configureListener(NativeSocket socket) {
    int enabled = 1;

#if defined(GEODE_IS_WINDOWS)
    return setsockopt(
        socket,
        SOL_SOCKET,
        SO_EXCLUSIVEADDRUSE,
        reinterpret_cast<const char*>(&enabled),
        sizeof(enabled)
    ) == 0;
#else
    return setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) == 0;
#endif
}

bool configureConnectedSocket(NativeSocket socket) {
#if defined(GEODE_IS_MACOS)
    int enabled = 1;
    return setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0;
#else
    static_cast<void>(socket);
    return true;
#endif
}

enum class WaitResult {
    Ready,
    TimedOut,
    Failed,
};

WaitResult waitForSocket(NativeSocket socket, bool readable, Deadline deadline) {
    while (true) {
        const auto now = Clock::now();
        if (now >= deadline) return WaitResult::TimedOut;

        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(remaining);

        timeval timeout {};
        timeout.tv_sec = static_cast<long>(seconds.count());
        timeout.tv_usec = static_cast<long>((remaining - seconds).count());

        fd_set readSet;
        fd_set writeSet;
        FD_ZERO(&readSet);
        FD_ZERO(&writeSet);

        if (readable) {
            FD_SET(socket, &readSet);
        } else {
            FD_SET(socket, &writeSet);
        }

        const auto selected = select(
#if defined(GEODE_IS_WINDOWS)
            0,
#else
            socket + 1,
#endif
            readable ? &readSet : nullptr,
            readable ? nullptr : &writeSet,
            nullptr,
            &timeout
        );

        if (selected > 0) return WaitResult::Ready;
        if (selected == 0) return WaitResult::TimedOut;
        if (!socketErrorWasInterrupted()) return WaitResult::Failed;
    }
}

std::ptrdiff_t receiveSome(NativeSocket socket, char* buffer, std::size_t size) {
    const auto amount = std::min(size, static_cast<std::size_t>(std::numeric_limits<int>::max()));
#if defined(GEODE_IS_WINDOWS)
    return static_cast<std::ptrdiff_t>(recv(socket, buffer, static_cast<int>(amount), 0));
#else
    return static_cast<std::ptrdiff_t>(recv(socket, buffer, amount, 0));
#endif
}

std::ptrdiff_t sendSome(NativeSocket socket, const char* buffer, std::size_t size) {
    const auto amount = std::min(size, static_cast<std::size_t>(std::numeric_limits<int>::max()));

#if defined(GEODE_IS_WINDOWS)
    return static_cast<std::ptrdiff_t>(send(socket, buffer, static_cast<int>(amount), 0));
#else
    return static_cast<std::ptrdiff_t>(send(socket, buffer, amount, 0));
#endif
}

std::optional<std::string> receiveRequest(NativeSocket socket, Deadline deadline) {
    std::string request;
    request.reserve(2048);

    std::array<char, 2048> buffer {};
    while (request.size() < kMaximumRequestSize) {
        const auto ready = waitForSocket(socket, true, deadline);
        if (ready != WaitResult::Ready) return std::nullopt;

        const auto remaining = kMaximumRequestSize - request.size();
        const auto received = receiveSome(socket, buffer.data(), std::min(buffer.size(), remaining));
        if (received > 0) {
            request.append(buffer.data(), static_cast<std::size_t>(received));
            if (request.find("\r\n\r\n") != std::string::npos) return request;
            continue;
        }

        if (received == 0) return std::nullopt;
        if (socketErrorWasInterrupted() || socketErrorWouldBlock()) continue;
        return std::nullopt;
    }

    return std::nullopt;
}

bool sendAll(NativeSocket socket, std::string_view response, Deadline deadline) {
    std::size_t sent = 0;
    while (sent < response.size()) {
        const auto ready = waitForSocket(socket, false, deadline);
        if (ready != WaitResult::Ready) return false;

        const auto result = sendSome(socket, response.data() + sent, response.size() - sent);
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }

        if (result < 0 && (socketErrorWasInterrupted() || socketErrorWouldBlock())) continue;
        return false;
    }

    return true;
}

void finishSending(NativeSocket socket) {
#if defined(GEODE_IS_WINDOWS)
    shutdown(socket, SD_SEND);
#else
    shutdown(socket, SHUT_WR);
#endif
}

void drainAfterShutdown(NativeSocket socket, Deadline deadline) {
    std::array<char, 256> buffer {};

    while (Clock::now() < deadline) {
        if (waitForSocket(socket, true, deadline) != WaitResult::Ready) return;

        const auto received = receiveSome(socket, buffer.data(), buffer.size());
        if (received == 0) return;
        if (received > 0 || socketErrorWasInterrupted() || socketErrorWouldBlock()) continue;
        return;
    }
}

std::string percentDecode(std::string_view value) {
    const auto hexValue = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        return -1;
    };

    std::string decoded;
    decoded.reserve(value.size());

    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '+') {
            decoded += ' ';
            continue;
        }

        if (value[index] == '%' && index + 2 < value.size()) {
            const auto high = hexValue(value[index + 1]);
            const auto low = hexValue(value[index + 2]);
            if (high >= 0 && low >= 0) {
                decoded += static_cast<char>((high << 4) | low);
                index += 2;
                continue;
            }
        }

        decoded += value[index];
    }

    return decoded;
}

std::optional<std::string> queryParameter(std::string_view target, std::string_view name) {
    const auto queryStart = target.find('?');
    if (queryStart == std::string_view::npos) return std::nullopt;

    auto query = target.substr(queryStart + 1);
    while (!query.empty()) {
        const auto separator = query.find('&');
        const auto parameter = query.substr(0, separator);
        const auto equals = parameter.find('=');

        if (parameter.substr(0, equals) == name) {
            return percentDecode(equals == std::string_view::npos
                ? std::string_view {}
                : parameter.substr(equals + 1));
        }

        if (separator == std::string_view::npos) break;
        query.remove_prefix(separator + 1);
    }

    return std::nullopt;
}

std::string makeState() {
    static constexpr char hex[] = "0123456789abcdef";
    const std::array<std::uint64_t, 2> entropy {
        geode::utils::random::secureU64(),
        geode::utils::random::secureU64(),
    };

    std::string state;
    state.reserve(32);
    for (const auto value : entropy) {
        for (int shift = 60; shift >= 0; shift -= 4) {
            state += hex[(value >> shift) & 0x0f];
        }
    }
    return state;
}

std::string callbackResponse(
    std::string_view status,
    std::string_view heading,
    std::string_view message
) {
    const auto body =
        "<main style='font-family: sans-serif; max-width: 40rem; margin: 4rem auto'>"
        "<h2>" + std::string(heading) + "</h2><p>" + std::string(message) + "</p></main>";

    return "HTTP/1.1 " + std::string(status) + "\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;
}

void respond(
    NativeSocket socket,
    std::string_view status,
    std::string_view heading,
    std::string_view message,
    Deadline serverDeadline
) {
    const auto response = callbackResponse(status, heading, message);
    const auto responseDeadline = std::min(serverDeadline, Clock::now() + kRequestTimeout);
    sendAll(socket, response, responseDeadline);
    finishSending(socket);
    drainAfterShutdown(
        socket,
        std::min(serverDeadline, Clock::now() + kShutdownDrainTimeout)
    );
}

enum class CallbackResult {
    KeepListening,
    Finished,
};

CallbackResult handleCallback(
    NativeSocket socket,
    std::string_view expectedState,
    std::string_view clientId,
    std::string_view clientSecret,
    Deadline serverDeadline
) {
    const auto requestDeadline = std::min(serverDeadline, Clock::now() + kRequestTimeout);
    const auto request = receiveRequest(socket, requestDeadline);
    if (!request) {
        respond(
            socket,
            "400 Bad Request",
            "Authorization failed",
            "The authorization response was not valid.",
            serverDeadline
        );
        return CallbackResult::KeepListening;
    }

    const auto methodEnd = request->find(' ');
    const auto targetEnd = methodEnd == std::string::npos
        ? std::string::npos
        : request->find(' ', methodEnd + 1);

    if (methodEnd == std::string::npos || targetEnd == std::string::npos ||
        request->substr(0, methodEnd) != "GET") {
        respond(
            socket,
            "400 Bad Request",
            "Authorization failed",
            "The authorization response was not valid.",
            serverDeadline
        );
        return CallbackResult::KeepListening;
    }

    const auto target = std::string_view(*request).substr(methodEnd + 1, targetEnd - methodEnd - 1);
    const auto callbackState = queryParameter(target, "state");
    if (!callbackState || *callbackState != expectedState) {
        geode::prelude::log::warn("Rejected an OAuth callback with an invalid state");
        respond(
            socket,
            "403 Forbidden",
            "Authorization failed",
            "Please return to Geometry Dash and try again.",
            serverDeadline
        );
        return CallbackResult::KeepListening;
    }

    if (queryParameter(target, "error")) {
        respond(
            socket,
            "200 OK",
            "Authorization cancelled",
            "Discord did not authorize AutoDeafen.",
            serverDeadline
        );
        return CallbackResult::Finished;
    }

    const auto code = queryParameter(target, "code");
    if (!code || code->empty()) {
        respond(
            socket,
            "400 Bad Request",
            "Authorization failed",
            "Discord did not provide an authorization code.",
            serverDeadline
        );
        return CallbackResult::KeepListening;
    }

    helpers::sendTokenRequest(
        helpers::tokenRequestBody(
            clientId,
            clientSecret,
            "authorization_code",
            "code",
            *code
        ),
        std::string(clientId),
        std::string(clientSecret),
        helpers::TokenRequestKind::AuthorizationCode
    );

    respond(
        socket,
        "200 OK",
        "Authorization callback received",
        "Return to Geometry Dash while AutoDeafen exchanges the code and connects to Discord.",
        serverDeadline
    );
    return CallbackResult::Finished;
}

void serveCallbacks(
    SocketRuntime runtime,
    SocketHandle listener,
    std::string expectedState,
    std::string clientId,
    std::string clientSecret,
    const std::atomic_bool& stopRequested
) {
    const auto deadline = Clock::now() + kCallbackTimeout;

    while (!stopRequested.load(std::memory_order_acquire) && Clock::now() < deadline) {
        const auto pollDeadline = std::min(deadline, Clock::now() + kListenerPollInterval);
        const auto ready = waitForSocket(listener.get(), true, pollDeadline);
        if (ready == WaitResult::TimedOut) continue;
        if (ready == WaitResult::Failed) {
            geode::prelude::log::warn(
                "The OAuth callback listener failed while waiting: {}",
                socketError()
            );
            break;
        }

        if (stopRequested.load(std::memory_order_acquire)) break;

        SocketHandle client(accept(listener.get(), nullptr, nullptr));
        if (!client) {
            if (socketErrorWasInterrupted() || socketErrorWouldBlock()) continue;
            geode::prelude::log::warn("Could not accept an OAuth callback: {}", socketError());
            break;
        }

        if (!configureConnectedSocket(client.get())) {
            geode::prelude::log::warn("Could not configure an OAuth callback socket: {}", socketError());
            continue;
        }

        if (handleCallback(
            client.get(),
            expectedState,
            clientId,
            clientSecret,
            deadline
        ) == CallbackResult::Finished) {
            break;
        }
    }

    listener.reset();
    runtime.reset();
}

class CallbackServer {
public:
    CallbackServer() = default;
    CallbackServer(const CallbackServer&) = delete;
    CallbackServer& operator=(const CallbackServer&) = delete;

    ~CallbackServer() {
        std::lock_guard lock(m_startMutex);
        m_stopRequested.store(true, std::memory_order_release);
        if (m_thread.joinable()) m_thread.join();
    }

    std::optional<std::string> start(std::string clientId, std::string clientSecret) {
        if (clientId.empty() || clientSecret.empty()) return std::nullopt;

        std::lock_guard lock(m_startMutex);
        if (m_running.load(std::memory_order_acquire)) {
            geode::prelude::log::warn("An OAuth callback server is already running");
            return std::nullopt;
        }
        if (m_thread.joinable()) m_thread.join();

        SocketRuntime runtime;
        if (!runtime) {
            geode::prelude::log::warn("Could not initialize the OAuth callback server");
            return std::nullopt;
        }

        SocketHandle listener(socket(AF_INET, SOCK_STREAM, 0));
        if (!listener) {
            geode::prelude::log::warn(
                "Could not create the OAuth callback socket: {}",
                socketError()
            );
            return std::nullopt;
        }

        if (!configureListener(listener.get())) {
            geode::prelude::log::warn(
                "Could not configure the OAuth callback listener: {}",
                socketError()
            );
            return std::nullopt;
        }

        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(oauth::kCallbackPort);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (bind(
            listener.get(),
            reinterpret_cast<sockaddr*>(&address),
            static_cast<int>(sizeof(address))
        ) != 0) {
            geode::prelude::log::warn(
                "Could not bind the OAuth callback server: {}",
                socketError()
            );
            return std::nullopt;
        }

        if (listen(listener.get(), SOMAXCONN) != 0) {
            geode::prelude::log::warn(
                "Could not start the OAuth callback server: {}",
                socketError()
            );
            return std::nullopt;
        }

        const auto state = makeState();
        m_stopRequested.store(false, std::memory_order_release);
        m_running.store(true, std::memory_order_release);

        try {
            m_thread = std::thread([
                this,
                runtime = std::move(runtime),
                listener = std::move(listener),
                expectedState = state,
                clientId = std::move(clientId),
                clientSecret = std::move(clientSecret)
            ]() mutable {
                try {
                    serveCallbacks(
                        std::move(runtime),
                        std::move(listener),
                        std::move(expectedState),
                        std::move(clientId),
                        std::move(clientSecret),
                        m_stopRequested
                    );
                } catch (const std::exception& error) {
                    geode::prelude::log::warn(
                        "The OAuth callback server stopped unexpectedly: {}",
                        error.what()
                    );
                } catch (...) {
                    geode::prelude::log::warn("The OAuth callback server stopped unexpectedly");
                }
                m_running.store(false, std::memory_order_release);
            });
        } catch (const std::system_error& error) {
            m_running.store(false, std::memory_order_release);
            geode::prelude::log::warn(
                "Could not start the OAuth callback thread: {}",
                error.what()
            );
            return std::nullopt;
        }

        return state;
    }

private:
    std::mutex m_startMutex;
    std::atomic_bool m_running = false;
    std::atomic_bool m_stopRequested = false;
    std::thread m_thread;
};

CallbackServer& callbackServer() {
    static CallbackServer server;
    return server;
}

}

std::optional<std::string> oauth::startServer(std::string clientId, std::string clientSecret) {
    return callbackServer().start(std::move(clientId), std::move(clientSecret));
}
