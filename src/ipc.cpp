#include "ipc.h"

#include <Geode/platform/cplatform.h>

#if defined(GEODE_IS_WINDOWS)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(GEODE_IS_MACOS)
    #include <cerrno>
    #include <cstddef>
    #include <fcntl.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <sys/un.h>
    #include <unistd.h>
#else
    #error "AutoDeafen Discord IPC is only available for Geode desktop targets"
#endif

#include <Geode/Geode.hpp>
#include <Geode/ui/Notification.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "linux_setup.h"

namespace {

using Duration = std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;

constexpr Duration kIdlePollTimeout {250};
constexpr Duration kWriteTimeout {2000};
constexpr Duration kResponseTimeout {5000};
constexpr Duration kReconnectDelay {250};
constexpr Duration kFailureRetryDelay {2000};
constexpr Duration kConnectTimeout {2000};
constexpr int kReconnectAttempts = 3;
constexpr std::uint32_t kMaximumFrameSize = 1024 * 1024;

#if defined(GEODE_IS_WINDOWS)
constexpr Duration kPipeBusyTimeout {5000};

bool runningUnderWine() {
    static const bool detected = geode::utils::platform::isWine();
    return detected;
}
#endif

enum class IoResult {
    Complete,
    TimedOut,
    Failed,
};

struct IoTransfer {
    IoResult result = IoResult::Failed;
    std::size_t bytes = 0;
    std::uint32_t platformError = 0;
};

enum class Opcode : std::uint32_t {
    Handshake = 0,
    Frame = 1,
    Close = 2,
    Ping = 3,
    Pong = 4,
};

struct Frame {
    Opcode opcode = Opcode::Close;
    std::string payload;
};

Duration timeRemaining(Clock::time_point deadline) {
    const auto remaining = std::chrono::duration_cast<Duration>(deadline - Clock::now());
    return std::max(remaining, Duration::zero());
}

#if defined(GEODE_IS_MACOS)
std::vector<std::string> discordRuntimeDirectories() {
    std::vector<std::string> directories;

    for (const auto* variable : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
        const auto* value = std::getenv(variable);
        if (!value || !*value) continue;

        if (std::find(directories.begin(), directories.end(), value) == directories.end()) {
            directories.emplace_back(value);
        }
    }

    if (std::find(directories.begin(), directories.end(), "/tmp") == directories.end()) {
        directories.emplace_back("/tmp");
    }
    return directories;
}
#endif

#if defined(GEODE_IS_WINDOWS)

class DiscordTransport {
public:
    DiscordTransport() = default;
    DiscordTransport(const DiscordTransport&) = delete;
    DiscordTransport& operator=(const DiscordTransport&) = delete;

    ~DiscordTransport() {
        close();
    }

    bool connect() {
        close();
        m_lastConnectError = ERROR_SUCCESS;
        m_lastIoError = ERROR_SUCCESS;
        m_selectedEndpoint = -1;
        m_probedEndpoints = 0;
        const auto pipePrefix = runningUnderWine()
            ? L"\\\\.\\pipe\\discord-ipc-"
            : L"\\\\?\\pipe\\discord-ipc-";

        // rpc-bridge can expose more than one Discord IPC endpoint. Reserve
        // endpoint 0 for clients such as Eclipse that always try it first, so
        // both mods can stay connected when Geometry Dash is running in Wine.
        // Native Windows keeps Discord's conventional 0-to-9 probe order.
        const std::array wineEndpointOrder{1, 2, 3, 4, 5, 6, 7, 8, 9, 0};
        const std::array nativeEndpointOrder{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
        const auto& endpointOrder = runningUnderWine()
            ? wineEndpointOrder
            : nativeEndpointOrder;

        for (const int index : endpointOrder) {
            const auto pipeName = pipePrefix + std::to_wstring(index);
            ++m_probedEndpoints;

            for (int attempt = 0; attempt < 2; ++attempt) {
                m_pipe = CreateFileW(
                    pipeName.c_str(),
                    GENERIC_READ | GENERIC_WRITE,
                    0,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED,
                    nullptr
                );

                if (valid()) {
                    m_selectedEndpoint = index;
                    return true;
                }

                m_lastConnectError = GetLastError();
                if (m_lastConnectError == ERROR_FILE_NOT_FOUND) break;
                if (m_lastConnectError != ERROR_PIPE_BUSY) return false;
                if (attempt != 0) break;
                if (!WaitNamedPipeW(
                        pipeName.c_str(),
                        static_cast<DWORD>(kPipeBusyTimeout.count())
                    )) {
                    m_lastConnectError = GetLastError();
                    break;
                }
            }
        }

        return false;
    }

    bool valid() const {
        return m_pipe != INVALID_HANDLE_VALUE;
    }

    std::uint32_t lastConnectError() const {
        return static_cast<std::uint32_t>(m_lastConnectError);
    }

    std::uint32_t lastIoError() const {
        return static_cast<std::uint32_t>(m_lastIoError);
    }

    int selectedEndpoint() const {
        return m_selectedEndpoint;
    }

    int probedEndpoints() const {
        return m_probedEndpoints;
    }

    void close() {
        if (!valid()) return;

        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }

    IoTransfer readSome(void* data, std::size_t size, Duration timeout) {
        return transfer(true, data, size, timeout);
    }

    IoTransfer writeSome(const void* data, std::size_t size, Duration timeout) {
        return transfer(false, const_cast<void*>(data), size, timeout);
    }

private:
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
    DWORD m_lastConnectError = ERROR_SUCCESS;
    DWORD m_lastIoError = ERROR_SUCCESS;
    int m_selectedEndpoint = -1;
    int m_probedEndpoints = 0;

    IoTransfer transfer(bool reading, void* data, std::size_t size, Duration timeout) {
        if (!valid() || size == 0) return {size == 0 ? IoResult::Complete : IoResult::Failed, 0};

        m_lastIoError = ERROR_SUCCESS;

        OVERLAPPED overlapped {};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) {
            m_lastIoError = GetLastError();
            return {IoResult::Failed, 0, static_cast<std::uint32_t>(m_lastIoError)};
        }

        const auto amount = static_cast<DWORD>(std::min<std::size_t>(size, MAXDWORD));
        DWORD transferred = 0;
        const auto started = reading
            ? ReadFile(m_pipe, data, amount, nullptr, &overlapped)
            : WriteFile(m_pipe, data, amount, nullptr, &overlapped);

        if (started) {
            const auto complete = GetOverlappedResult(m_pipe, &overlapped, &transferred, FALSE);
            if (!complete) m_lastIoError = GetLastError();
            CloseHandle(overlapped.hEvent);
            return {
                complete && transferred > 0 ? IoResult::Complete : IoResult::Failed,
                complete ? static_cast<std::size_t>(transferred) : 0,
                static_cast<std::uint32_t>(m_lastIoError),
            };
        }

        m_lastIoError = GetLastError();
        if (m_lastIoError != ERROR_IO_PENDING) {
            CloseHandle(overlapped.hEvent);
            return {IoResult::Failed, 0, static_cast<std::uint32_t>(m_lastIoError)};
        }
        m_lastIoError = ERROR_SUCCESS;

        const auto waitResult = WaitForSingleObject(
            overlapped.hEvent,
            static_cast<DWORD>(timeout.count())
        );

        if (waitResult == WAIT_OBJECT_0) {
            const auto complete = GetOverlappedResult(m_pipe, &overlapped, &transferred, FALSE);
            if (!complete) m_lastIoError = GetLastError();
            CloseHandle(overlapped.hEvent);
            return {
                complete && transferred > 0 ? IoResult::Complete : IoResult::Failed,
                complete ? static_cast<std::size_t>(transferred) : 0,
                static_cast<std::uint32_t>(m_lastIoError),
            };
        }

        const auto waitError = waitResult == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;

        // OVERLAPPED and its event must stay alive until the kernel has completed
        // or cancelled the request, even when waiting itself fails.
        CancelIoEx(m_pipe, &overlapped);
        WaitForSingleObject(overlapped.hEvent, INFINITE);
        const auto completedDuringCancellation =
            GetOverlappedResult(m_pipe, &overlapped, &transferred, FALSE) && transferred > 0;
        CloseHandle(overlapped.hEvent);

        if (completedDuringCancellation) {
            return {IoResult::Complete, static_cast<std::size_t>(transferred)};
        }
        if (waitResult != WAIT_TIMEOUT) m_lastIoError = waitError;
        return {
            waitResult == WAIT_TIMEOUT ? IoResult::TimedOut : IoResult::Failed,
            0,
            static_cast<std::uint32_t>(m_lastIoError),
        };
    }
};

#elif defined(GEODE_IS_MACOS)

class DiscordTransport {
public:
    DiscordTransport() = default;
    DiscordTransport(const DiscordTransport&) = delete;
    DiscordTransport& operator=(const DiscordTransport&) = delete;

    ~DiscordTransport() {
        close();
    }

    bool connect() {
        close();
        m_lastConnectError = 0;
        m_lastIoError = 0;
        m_selectedEndpoint = -1;
        m_probedEndpoints = 0;

        for (const auto& directory : discordRuntimeDirectories()) {
            for (int index = 0; index < 10; ++index) {
                const auto path = directory + "/discord-ipc-" + std::to_string(index);
                if (path.size() >= sizeof(sockaddr_un::sun_path)) continue;
                ++m_probedEndpoints;

                m_socket = socket(AF_UNIX, SOCK_STREAM, 0);
                if (!valid()) {
                    m_lastConnectError = errno;
                    continue;
                }

                int noSigpipe = 1;
                setsockopt(m_socket, SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof(noSigpipe));

                const auto flags = fcntl(m_socket, F_GETFL, 0);
                if (flags < 0 || fcntl(m_socket, F_SETFL, flags | O_NONBLOCK) != 0) {
                    m_lastConnectError = errno;
                    close();
                    continue;
                }

                sockaddr_un address {};
                address.sun_family = AF_UNIX;
                std::copy(path.begin(), path.end(), address.sun_path);
                address.sun_path[path.size()] = '\0';

                const auto addressSize = static_cast<socklen_t>(
                    offsetof(sockaddr_un, sun_path) + path.size() + 1
                );
                address.sun_len = static_cast<std::uint8_t>(addressSize);

                const auto connected = ::connect(
                    m_socket,
                    reinterpret_cast<const sockaddr*>(&address),
                    addressSize
                );

                if (connected == 0) {
                    m_selectedEndpoint = index;
                    return true;
                }

                auto connectError = errno;
                if (connectError == EINPROGRESS) {
                    connectError = finishConnect();
                }

                if (connectError == 0) {
                    m_selectedEndpoint = index;
                    return true;
                }
                m_lastConnectError = connectError;
                close();
            }
        }

        return false;
    }

    bool valid() const {
        return m_socket >= 0;
    }

    std::uint32_t lastConnectError() const {
        return static_cast<std::uint32_t>(m_lastConnectError);
    }

    std::uint32_t lastIoError() const {
        return static_cast<std::uint32_t>(m_lastIoError);
    }

    int selectedEndpoint() const {
        return m_selectedEndpoint;
    }

    int probedEndpoints() const {
        return m_probedEndpoints;
    }

    void close() {
        if (!valid()) return;

        shutdown(m_socket, SHUT_RDWR);
        ::close(m_socket);
        m_socket = -1;
    }

    IoTransfer readSome(void* data, std::size_t size, Duration timeout) {
        return transfer(true, data, size, timeout);
    }

    IoTransfer writeSome(const void* data, std::size_t size, Duration timeout) {
        return transfer(false, const_cast<void*>(data), size, timeout);
    }

private:
    int m_socket = -1;
    int m_lastConnectError = 0;
    int m_lastIoError = 0;
    int m_selectedEndpoint = -1;
    int m_probedEndpoints = 0;

    IoResult waitFor(short events, Duration timeout) {
        const auto deadline = Clock::now() + timeout;

        while (true) {
            pollfd descriptor {m_socket, events, 0};
            const auto remaining = timeRemaining(deadline);
            const auto result = poll(&descriptor, 1, static_cast<int>(remaining.count()));

            if (result > 0) {
                if (descriptor.revents & POLLNVAL) {
                    m_lastIoError = EBADF;
                    return IoResult::Failed;
                }
                if (descriptor.revents & POLLERR) {
                    m_lastIoError = EIO;
                    return IoResult::Failed;
                }
                if (descriptor.revents & events) return IoResult::Complete;
                if (descriptor.revents & POLLHUP) {
                    m_lastIoError = ECONNRESET;
                    return IoResult::Failed;
                }
                continue;
            }
            if (result == 0) return IoResult::TimedOut;
            if (errno != EINTR) {
                m_lastIoError = errno;
                return IoResult::Failed;
            }
            if (Clock::now() >= deadline) return IoResult::TimedOut;
        }
    }

    int finishConnect() {
        const auto ready = waitFor(POLLOUT, kConnectTimeout);
        if (ready == IoResult::TimedOut) return ETIMEDOUT;

        int error = 0;
        socklen_t errorSize = sizeof(error);
        if (getsockopt(m_socket, SOL_SOCKET, SO_ERROR, &error, &errorSize) != 0) {
            return errno;
        }
        if (error != 0) return error;
        return ready == IoResult::Complete ? 0 : EIO;
    }

    IoTransfer transfer(bool reading, void* data, std::size_t size, Duration timeout) {
        if (!valid() || size == 0) return {size == 0 ? IoResult::Complete : IoResult::Failed, 0};

        m_lastIoError = 0;

        const auto deadline = Clock::now() + timeout;
        while (true) {
            const auto ready = waitFor(reading ? POLLIN : POLLOUT, timeRemaining(deadline));
            if (ready != IoResult::Complete) {
                return {ready, 0, static_cast<std::uint32_t>(m_lastIoError)};
            }

            const auto result = reading
                ? recv(m_socket, data, size, 0)
                : send(m_socket, data, size, 0);
            if (result > 0) return {IoResult::Complete, static_cast<std::size_t>(result)};
            if (result == 0) return {};
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                m_lastIoError = errno;
                return {IoResult::Failed, 0, static_cast<std::uint32_t>(m_lastIoError)};
            }
            if (Clock::now() >= deadline) return {IoResult::TimedOut, 0};
        }
    }
};

#endif

IoResult readExact(
    DiscordTransport& transport,
    void* data,
    std::size_t size,
    Clock::time_point deadline
) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t total = 0;

    while (total < size) {
        const auto remaining = timeRemaining(deadline);
        if (remaining <= Duration::zero()) {
            return total == 0 ? IoResult::TimedOut : IoResult::Failed;
        }

        const auto transfer = transport.readSome(bytes + total, size - total, remaining);
        if (transfer.result != IoResult::Complete || transfer.bytes == 0) {
            return transfer.result == IoResult::TimedOut && total == 0
                ? IoResult::TimedOut
                : IoResult::Failed;
        }
        total += transfer.bytes;
    }

    return IoResult::Complete;
}

IoResult writeExact(
    DiscordTransport& transport,
    const void* data,
    std::size_t size,
    Clock::time_point deadline
) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t total = 0;

    while (total < size) {
        const auto remaining = timeRemaining(deadline);
        if (remaining <= Duration::zero()) return IoResult::TimedOut;

        const auto transfer = transport.writeSome(bytes + total, size - total, remaining);
        if (transfer.result != IoResult::Complete || transfer.bytes == 0) return transfer.result;
        total += transfer.bytes;
    }

    return IoResult::Complete;
}

std::uint32_t decodeUint32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
        (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) |
        (static_cast<std::uint32_t>(bytes[3]) << 24);
}

void appendUint32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16));
    bytes.push_back(static_cast<std::uint8_t>(value >> 24));
}

std::string ioErrorDetail(const DiscordTransport& transport) {
    if (transport.lastIoError() == 0) return {};
#if defined(GEODE_IS_WINDOWS)
    return " (Windows error " + std::to_string(transport.lastIoError()) + ")";
#else
    return " (system error " + std::to_string(transport.lastIoError()) + ")";
#endif
}

IoResult readFrame(
    DiscordTransport& transport,
    Frame& frame,
    Duration timeout,
    std::string* diagnostic = nullptr
) {
    const auto deadline = Clock::now() + timeout;
    std::array<std::uint8_t, 8> header {};

    const auto headerResult = readExact(transport, header.data(), header.size(), deadline);
    if (headerResult != IoResult::Complete) {
        if (diagnostic) {
            *diagnostic = headerResult == IoResult::TimedOut
                ? "frame header timed out"
                : "frame header read failed" + ioErrorDetail(transport);
        }
        return headerResult;
    }

    const auto opcode = decodeUint32(header.data());
    const auto length = decodeUint32(header.data() + 4);
    if (opcode > static_cast<std::uint32_t>(Opcode::Pong)) {
        if (diagnostic) {
            *diagnostic = "invalid frame opcode " + std::to_string(opcode);
        }
        return IoResult::Failed;
    }
    if (length > kMaximumFrameSize) {
        if (diagnostic) {
            *diagnostic = "frame length " + std::to_string(length) +
                " exceeds the 1048576-byte safety limit";
        }
        return IoResult::Failed;
    }

    frame.opcode = static_cast<Opcode>(opcode);
    frame.payload.assign(length, '\0');
    if (frame.payload.empty()) return IoResult::Complete;

    // The header has already been consumed. A payload timeout must invalidate the
    // connection so the next frame cannot be parsed from the middle of this one.
    const auto payloadResult = readExact(
        transport,
        frame.payload.data(),
        frame.payload.size(),
        deadline
    );
    if (payloadResult == IoResult::Complete) return IoResult::Complete;

    if (diagnostic) {
        *diagnostic = "frame payload read failed after a valid header" + ioErrorDetail(transport);
    }
    return IoResult::Failed;
}

bool writeFrame(
    DiscordTransport& transport,
    Opcode opcode,
    std::string_view payload,
    std::string* diagnostic = nullptr
) {
    if (payload.size() > kMaximumFrameSize ||
        payload.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        if (diagnostic) *diagnostic = "outgoing frame exceeds the 1048576-byte safety limit";
        return false;
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(8 + payload.size());
    appendUint32(bytes, static_cast<std::uint32_t>(opcode));
    appendUint32(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());

    const auto result = writeExact(
        transport,
        bytes.data(),
        bytes.size(),
        Clock::now() + kWriteTimeout
    );
    if (result == IoResult::Complete) return true;

    if (diagnostic) {
        *diagnostic = result == IoResult::TimedOut
            ? "frame write timed out"
            : "frame write failed" + ioErrorDetail(transport);
    }
    return false;
}

std::string jsonString(std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";

    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped += '"';

    for (const auto byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        switch (character) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\b': escaped += "\\b"; break;
            case '\f': escaped += "\\f"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (character < 0x20) {
                    escaped += "\\u00";
                    escaped += hex[character >> 4];
                    escaped += hex[character & 0x0f];
                } else {
                    escaped += static_cast<char>(character);
                }
        }
    }

    escaped += '"';
    return escaped;
}

std::optional<matjson::Value> parsePayload(const Frame& frame) {
    if (frame.opcode != Opcode::Frame) return std::nullopt;

    auto parsed = matjson::parse(frame.payload);
    if (!parsed) return std::nullopt;
    return std::move(parsed).unwrap();
}

std::string jsonField(const matjson::Value& json, std::string_view field) {
    return json[std::string(field)].asString().unwrapOr("");
}

void logRpcError(const matjson::Value& json, std::string_view context) {
    const auto code = json["data"]["code"].asInt().unwrapOr(0);
    const auto message = json["data"]["message"].asString().unwrapOr("unknown Discord RPC error");
    if (code != 0) {
        geode::log::warn(
            "[ADIPC-RPC-REJECT] area=logRpcError command={} code={} message={}",
            context,
            code,
            message
        );
    } else {
        geode::log::warn(
            "[ADIPC-RPC-REJECT] area=logRpcError command={} code=missing message={}",
            context,
            message
        );
    }
}

void logCloseFrame(const Frame& frame, std::string_view context) {
    const auto parsed = matjson::parse(frame.payload);
    if (!parsed) {
        geode::log::warn(
            "[ADIPC-CLOSE] area=logCloseFrame command={} reason=unreadable",
            context
        );
        return;
    }

    const auto json = parsed.unwrap();
    const auto code = json["code"].asInt().unwrapOr(0);
    const auto message = json["message"].asString().unwrapOr("no reason returned");
    geode::log::warn(
        "[ADIPC-CLOSE] area=logCloseFrame command={} code={} message={}",
        context,
        code,
        message
    );
}

bool handleControlFrame(
    DiscordTransport& transport,
    const Frame& frame,
    std::string* diagnostic = nullptr
) {
    if (frame.opcode == Opcode::Ping) {
        if (writeFrame(transport, Opcode::Pong, frame.payload, diagnostic)) return true;
        if (diagnostic) *diagnostic = "could not reply to Discord PING: " + *diagnostic;
        return false;
    }
    if (frame.opcode == Opcode::Pong || frame.opcode == Opcode::Frame) return true;
    if (diagnostic) {
        *diagnostic = "unexpected control opcode " +
            std::to_string(static_cast<std::uint32_t>(frame.opcode));
    }
    return false;
}

bool waitForReady(DiscordTransport& transport, std::string& failureReason) {
    const auto deadline = Clock::now() + kResponseTimeout;

    while (Clock::now() < deadline) {
        Frame frame;
        std::string detail;
        const auto result = readFrame(transport, frame, timeRemaining(deadline), &detail);
        if (result != IoResult::Complete) {
            failureReason = "failed while waiting for Discord's IPC READY event [ADIPC-READY-READ " +
                detail + "]";
            return false;
        }
        if (frame.opcode == Opcode::Close) {
            logCloseFrame(frame, "the IPC handshake");
            failureReason = "Discord closed the connection during the IPC handshake "
                "[ADIPC-READY-CLOSE area=waitForReady]";
            return false;
        }
        if (!handleControlFrame(transport, frame, &detail)) {
            failureReason = "Discord returned an invalid IPC control frame during the handshake "
                "[ADIPC-READY-CONTROL " + detail + "]";
            return false;
        }
        if (frame.opcode != Opcode::Frame) continue;

        const auto json = parsePayload(frame);
        if (!json) {
            failureReason = "Discord returned invalid JSON during the IPC handshake "
                "[ADIPC-READY-JSON area=waitForReady]";
            return false;
        }
        if (jsonField(*json, "evt") == "ERROR") {
            logRpcError(*json, "the IPC handshake");
            failureReason = "Discord rejected the IPC handshake "
                "[ADIPC-READY-REJECT area=waitForReady]";
            return false;
        }
        if (jsonField(*json, "cmd") == "DISPATCH" && jsonField(*json, "evt") == "READY") {
            geode::log::info(
                "[ADIPC-READY] area=waitForReady result=READY endpoint=discord-ipc-{}",
                transport.selectedEndpoint()
            );
            return true;
        }
    }

    failureReason = "timed out waiting for Discord's IPC READY event "
        "[ADIPC-READY-TIMEOUT area=waitForReady]";
    return false;
}

bool waitForResponse(
    DiscordTransport& transport,
    std::string_view expectedCommand,
    std::string_view expectedNonce,
    std::string& failureReason
) {
    const auto deadline = Clock::now() + kResponseTimeout;

    while (Clock::now() < deadline) {
        Frame frame;
        std::string detail;
        const auto result = readFrame(transport, frame, timeRemaining(deadline), &detail);
        if (result != IoResult::Complete) {
            failureReason = "failed while waiting for Discord's " +
                std::string(expectedCommand) + " response [ADIPC-RESPONSE-READ command=" +
                std::string(expectedCommand) + " detail=" + detail + "]";
            return false;
        }
        if (frame.opcode == Opcode::Close) {
            logCloseFrame(frame, expectedCommand);
            failureReason = "Discord closed the connection during " +
                std::string(expectedCommand) + " [ADIPC-RESPONSE-CLOSE command=" +
                std::string(expectedCommand) + "]";
            return false;
        }
        if (!handleControlFrame(transport, frame, &detail)) {
            failureReason = "Discord returned an invalid IPC control frame during " +
                std::string(expectedCommand) + " [ADIPC-RESPONSE-CONTROL command=" +
                std::string(expectedCommand) + " detail=" + detail + "]";
            return false;
        }
        if (frame.opcode != Opcode::Frame) continue;

        const auto json = parsePayload(frame);
        if (!json) {
            failureReason = "Discord returned invalid JSON during " +
                std::string(expectedCommand) + " [ADIPC-RESPONSE-JSON command=" +
                std::string(expectedCommand) + "]";
            return false;
        }

        const auto nonce = jsonField(*json, "nonce");
        const auto event = jsonField(*json, "evt");
        if (event == "ERROR") {
            if (nonce.empty() || nonce == expectedNonce) {
                logRpcError(*json, expectedCommand);
                failureReason = "Discord rejected " + std::string(expectedCommand) +
                    " [ADIPC-RESPONSE-REJECT command=" + std::string(expectedCommand) + "]";
                return false;
            }
            continue;
        }

        if (nonce == expectedNonce && jsonField(*json, "cmd") == expectedCommand) {
            geode::log::info(
                "[ADIPC-RESPONSE] area=waitForResponse command={} result=accepted",
                expectedCommand
            );
            return true;
        }
    }

    failureReason = "timed out waiting for Discord's " + std::string(expectedCommand) +
        " response [ADIPC-RESPONSE-TIMEOUT command=" + std::string(expectedCommand) + "]";
    return false;
}

struct Credentials {
    std::string clientId;
    std::string accessToken;
};

struct WorkSnapshot {
    Credentials credentials;
    std::uint64_t credentialsVersion = 0;
    std::optional<bool> desiredDeafened;
    std::uint64_t desiredVersion = 0;
};

class DiscordClient {
public:
    DiscordClient()
      : m_worker([this] {
            run();
        }) {}

    DiscordClient(const DiscordClient&) = delete;
    DiscordClient& operator=(const DiscordClient&) = delete;

    ~DiscordClient() {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_wakeup.notify_all();
        if (m_worker.joinable()) m_worker.join();
    }

    void updateCredentials(std::string clientId, std::string accessToken) {
        const auto hasClientId = !clientId.empty();
        const auto hasAccessToken = !accessToken.empty();
        std::uint64_t version = 0;
        {
            std::lock_guard lock(m_mutex);
            m_credentials = {std::move(clientId), std::move(accessToken)};
            version = ++m_credentialsVersion;
        }
        geode::log::info(
            "[ADIPC-CREDENTIALS] area=DiscordClient::updateCredentials version={} "
            "client-id={} access-token={}",
            version,
            hasClientId ? "present" : "missing",
            hasAccessToken ? "present" : "missing"
        );
        m_wakeup.notify_one();
    }

    void setDeafened(bool deafened) {
        {
            std::lock_guard lock(m_mutex);
            m_desiredDeafened = deafened;
            ++m_desiredVersion;
        }
        m_wakeup.notify_one();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_wakeup;
    Credentials m_credentials;
    std::uint64_t m_credentialsVersion = 0;
    std::optional<bool> m_desiredDeafened;
    std::uint64_t m_desiredVersion = 0;
    bool m_stopping = false;
    std::thread m_worker;

    WorkSnapshot snapshot() {
        std::lock_guard lock(m_mutex);
        return {
            m_credentials,
            m_credentialsVersion,
            m_desiredDeafened,
            m_desiredVersion,
        };
    }

    bool workChanged(const WorkSnapshot& work) {
        std::lock_guard lock(m_mutex);
        return m_stopping ||
            m_credentialsVersion != work.credentialsVersion ||
            m_desiredVersion != work.desiredVersion;
    }

    bool stopping() {
        std::lock_guard lock(m_mutex);
        return m_stopping;
    }

    void finishUndeafen(std::uint64_t desiredVersion) {
        std::lock_guard lock(m_mutex);
        if (m_desiredVersion == desiredVersion && m_desiredDeafened == false) {
            m_desiredDeafened.reset();
        }
    }

    bool waitForWork(const WorkSnapshot& work) {
        std::unique_lock lock(m_mutex);
        m_wakeup.wait(lock, [&] {
            return m_stopping ||
                m_credentialsVersion != work.credentialsVersion ||
                m_desiredVersion != work.desiredVersion;
        });
        return !m_stopping;
    }

    bool waitBeforeRetry(const WorkSnapshot& work) {
        std::unique_lock lock(m_mutex);
        m_wakeup.wait_for(lock, kReconnectDelay, [&] {
            return m_stopping ||
                m_credentialsVersion != work.credentialsVersion ||
                m_desiredVersion != work.desiredVersion;
        });
        return !m_stopping &&
            m_credentialsVersion == work.credentialsVersion;
    }

    bool waitAfterFailure(const WorkSnapshot& work) {
        std::unique_lock lock(m_mutex);
        m_wakeup.wait_for(lock, kFailureRetryDelay, [&] {
            return m_stopping ||
                m_credentialsVersion != work.credentialsVersion ||
                m_desiredVersion != work.desiredVersion;
        });
        return !m_stopping;
    }

    static std::string nextNonce(std::uint64_t& nonce) {
        return "autodeafen-" + std::to_string(++nonce);
    }

    static bool authenticate(
        DiscordTransport& transport,
        const Credentials& credentials,
        std::uint64_t& nonce,
        std::string& failureReason
    ) {
        const auto handshake = "{\"v\":1,\"client_id\":" + jsonString(credentials.clientId) + "}";
        std::string detail;
        if (!writeFrame(transport, Opcode::Handshake, handshake, &detail)) {
            failureReason = "could not write the Discord IPC handshake "
                "[ADIPC-HANDSHAKE-WRITE area=authenticate detail=" + detail + "]";
            return false;
        }
        geode::log::info(
            "[ADIPC-HANDSHAKE-WRITE] area=authenticate result=sent endpoint=discord-ipc-{}",
            transport.selectedEndpoint()
        );
        if (!waitForReady(transport, failureReason)) {
            return false;
        }

        const auto requestNonce = nextNonce(nonce);
        const auto authorization =
            "{\"cmd\":\"AUTHENTICATE\",\"args\":{\"access_token\":" +
            jsonString(credentials.accessToken) +
            "},\"nonce\":" + jsonString(requestNonce) + "}";

        detail.clear();
        if (!writeFrame(transport, Opcode::Frame, authorization, &detail)) {
            failureReason = "could not write Discord's AUTHENTICATE request "
                "[ADIPC-AUTH-WRITE area=authenticate detail=" + detail + "]";
            return false;
        }
        geode::log::info(
            "[ADIPC-AUTH-WRITE] area=authenticate result=sent token=redacted"
        );
        return waitForResponse(
            transport,
            "AUTHENTICATE",
            requestNonce,
            failureReason
        );
    }

    bool ensureAuthenticated(
        DiscordTransport& transport,
        bool& authenticated,
        const WorkSnapshot& work,
        std::uint64_t& nonce,
        std::string& failureReason
    ) {
        if (authenticated && transport.valid()) return true;

        // A bridge can accept the first pipe connection and then exit because
        // it cannot reach the host Discord socket. Keep that first exchange
        // failure so a later "pipe not found" retry does not hide the cause.
        std::optional<std::string> firstExchangeFailure;

        for (int attempt = 0; attempt < kReconnectAttempts; ++attempt) {
            if (workChanged(work)) return false;

            transport.close();
            if (!transport.connect()) {
#if defined(GEODE_IS_WINDOWS)
                failureReason = "Discord's named pipe is unavailable (Windows error " +
                    std::to_string(transport.lastConnectError()) + ") [ADIPC-CONNECT "
                    "area=DiscordTransport::connect probed=" +
                    std::to_string(transport.probedEndpoints()) + " endpoints]";
#else
                failureReason = "Discord's IPC socket is unavailable (error " +
                    std::to_string(transport.lastConnectError()) + ") [ADIPC-CONNECT "
                    "area=DiscordTransport::connect probed=" +
                    std::to_string(transport.probedEndpoints()) + " endpoints]";
#endif
            } else {
                geode::log::info(
                    "[ADIPC-CONNECT] area=DiscordTransport::connect result=opened "
                    "endpoint=discord-ipc-{} probed={}",
                    transport.selectedEndpoint(),
                    transport.probedEndpoints()
                );
            }

            if (transport.valid() && authenticate(
                    transport,
                    work.credentials,
                    nonce,
                    failureReason
                )) {
                authenticated = true;
                return true;
            } else if (transport.valid() && !firstExchangeFailure) {
                firstExchangeFailure = failureReason;
            }

            transport.close();
            authenticated = false;
            if (attempt + 1 < kReconnectAttempts && !waitBeforeRetry(work)) return false;
        }

        if (firstExchangeFailure && failureReason != *firstExchangeFailure) {
            failureReason = *firstExchangeFailure + "; last retry: " + failureReason;
        }
        failureReason += " [ADIPC-RETRY attempts=" + std::to_string(kReconnectAttempts) + "]";

        return false;
    }

    bool applyVoiceSetting(
        DiscordTransport& transport,
        bool& authenticated,
        const WorkSnapshot& work,
        std::uint64_t& nonce,
        std::string& failureReason
    ) {
        for (int attempt = 0; attempt < kReconnectAttempts; ++attempt) {
            if (!ensureAuthenticated(
                    transport,
                    authenticated,
                    work,
                    nonce,
                    failureReason
                )) {
                return false;
            }
            if (workChanged(work)) return false;

            const auto requestNonce = nextNonce(nonce);
            const auto payload =
                "{\"cmd\":\"SET_VOICE_SETTINGS\",\"args\":{\"deaf\":" +
                std::string(*work.desiredDeafened ? "true" : "false") +
                "},\"nonce\":" + jsonString(requestNonce) + "}";

            std::string detail;
            if (!writeFrame(transport, Opcode::Frame, payload, &detail)) {
                failureReason = "could not write Discord's SET_VOICE_SETTINGS request "
                    "[ADIPC-VOICE-WRITE area=applyVoiceSetting detail=" + detail + "]";
            } else if (waitForResponse(
                    transport,
                    "SET_VOICE_SETTINGS",
                    requestNonce,
                    failureReason
                )) {
                return true;
            }

            transport.close();
            authenticated = false;
            if (attempt + 1 < kReconnectAttempts && !waitBeforeRetry(work)) return false;
        }

        return false;
    }

    static bool drainOneFrame(DiscordTransport& transport, std::string& failureReason) {
        Frame frame;
        std::string detail;
        const auto result = readFrame(transport, frame, kIdlePollTimeout, &detail);
        if (result == IoResult::TimedOut) return true;
        if (result != IoResult::Complete) {
            failureReason = "idle Discord IPC read failed [ADIPC-IDLE-READ detail=" +
                detail + "]";
            return false;
        }
        if (!handleControlFrame(transport, frame, &detail)) {
            failureReason = "idle Discord IPC control frame failed [ADIPC-IDLE-CONTROL detail=" +
                detail + "]";
            return false;
        }

        if (const auto json = parsePayload(frame); json && jsonField(*json, "evt") == "ERROR") {
            logRpcError(*json, "an asynchronous IPC command");
        }
        return true;
    }

#if defined(GEODE_IS_WINDOWS)
    static bool isNamedPipeFailure(std::string_view reason) {
        return reason.starts_with("Discord's named pipe is unavailable");
    }

    static void showLinuxIpcWarningOnce(std::string_view reason) {
        static std::once_flag pipeWarningShown;
        static std::once_flag exchangeWarningShown;
        auto& shown = isNamedPipeFailure(reason)
            ? pipeWarningShown
            : exchangeWarningShown;

        std::call_once(shown, [reason = std::string(reason)] {
            std::string message;
            if (reason.starts_with("Discord's named pipe is unavailable (Windows error 2)")) {
                message = "Discord/bridge pipe not found (error 2).\nOpen the Linux guide in settings.";
            } else if (reason.starts_with("Discord's named pipe is unavailable (Windows error 3)")) {
                message = "Discord/bridge pipe not found (error 3).\nOpen the Linux guide in settings.";
            } else if (reason.starts_with("Discord's named pipe is unavailable (Windows error 231)")) {
                message = "Discord IPC pipe is busy (error 231).\nOpen the Linux guide in settings.";
            } else if (reason.starts_with("Discord's named pipe is unavailable (Windows error 5)")) {
                message = "Discord IPC access was denied (error 5).\nOpen the Linux guide in settings.";
            } else {
                message = "Discord IPC connection failed on Linux.\nOpen the Linux guide in settings.";
            }

            geode::queueInMainThread([message = std::move(message)] {
                geode::Notification::create(
                    message,
                    geode::NotificationIcon::Warning,
                    geode::NOTIFICATION_LONG_TIME
                )->show();
            });
        });
    }
#endif

    static void logConnectionFailure(std::string_view reason) {
#if defined(GEODE_IS_WINDOWS)
        if (linux_setup::isLinuxHost()) {
            if (isNamedPipeFailure(reason)) {
                geode::log::warn(
                    "Could not reach Discord under Wine/Proton. For a compatible native, Flatpak, "
                    "or Snap Linux client, verify that a Discord IPC bridge is running inside "
                    "Geometry Dash's prefix and can see the client's socket. For Windows Discord, "
                    "run it inside the same prefix. Details: {}",
                    reason
                );
            } else if (reason.starts_with("Discord rejected")) {
                geode::log::warn(
                    "Discord's RPC endpoint rejected a request under Wine/Proton. Verify the "
                    "saved authorization, Discord RPC access, and client support for voice "
                    "settings. Details: {}",
                    reason
                );
            } else {
                geode::log::warn(
                    "Discord's Windows IPC pipe opened under Wine/Proton, but the RPC exchange "
                    "failed. Check the bridge log for a native socket failure and verify that the "
                    "Discord client supports RPC authentication and voice settings. Details: {}",
                    reason
                );
            }
            showLinuxIpcWarningOnce(reason);
            return;
        }
#endif
        geode::log::warn("Could not connect to Discord RPC: {}", reason);
    }

    void run() {
        DiscordTransport transport;
        std::uint64_t activeCredentialsVersion = 0;
        std::uint64_t handledCredentialsVersion = 0;
        std::uint64_t appliedVoiceVersion = 0;
        std::uint64_t nonce = 0;
        bool authenticated = false;
        std::string reportedConnectionFailure;

        while (true) {
            auto work = snapshot();
            if (stopping()) break;

            if (work.credentialsVersion != activeCredentialsVersion) {
                transport.close();
                authenticated = false;
                activeCredentialsVersion = work.credentialsVersion;
                appliedVoiceVersion = 0;
                reportedConnectionFailure.clear();
            }

            const auto connectionRequested = handledCredentialsVersion != work.credentialsVersion;
            const auto voiceRequested = work.desiredDeafened &&
                appliedVoiceVersion != work.desiredVersion;

            if (connectionRequested || voiceRequested) {
                if (work.credentials.clientId.empty() || work.credentials.accessToken.empty()) {
                    handledCredentialsVersion = work.credentialsVersion;
                    if (!waitForWork(work)) break;
                    continue;
                }

                std::string failureReason;
                const auto connected = ensureAuthenticated(
                    transport,
                    authenticated,
                    work,
                    nonce,
                    failureReason
                );
                if (workChanged(work)) continue;

                if (!connected) {
                    if (failureReason != reportedConnectionFailure) {
                        logConnectionFailure(failureReason);
                        reportedConnectionFailure = failureReason;
                    }
                    waitAfterFailure(work);
                    continue;
                }

                if (handledCredentialsVersion != work.credentialsVersion) {
                    geode::log::info("Connected to Discord RPC");
                }
                handledCredentialsVersion = work.credentialsVersion;
                reportedConnectionFailure.clear();

                work = snapshot();
                if (work.credentialsVersion != activeCredentialsVersion) continue;
                if (work.desiredDeafened && appliedVoiceVersion != work.desiredVersion) {
                    failureReason.clear();
                    const auto updated = applyVoiceSetting(
                        transport,
                        authenticated,
                        work,
                        nonce,
                        failureReason
                    );
                    if (!workChanged(work)) {
                        if (updated) {
                            appliedVoiceVersion = work.desiredVersion;
                            if (!*work.desiredDeafened) {
                                finishUndeafen(work.desiredVersion);
                            }
                        } else {
                            geode::log::warn(
                                "Could not update Discord voice settings; retrying: {}",
                                failureReason
                            );
                            waitAfterFailure(work);
                        }
                    }
                }
                continue;
            }

            if (authenticated && transport.valid()) {
                std::string idleFailure;
                if (!drainOneFrame(transport, idleFailure)) {
                    transport.close();
                    authenticated = false;
                    handledCredentialsVersion = 0;
                    appliedVoiceVersion = 0;
                    reportedConnectionFailure.clear();
                    geode::log::warn(
                        "Lost the Discord IPC connection; it will reconnect when needed: {}",
                        idleFailure
                    );
                    waitAfterFailure(work);
                }
                continue;
            }

            if (!waitForWork(work)) break;
        }
    }
};

DiscordClient& discordClient() {
    static DiscordClient client;
    return client;
}

}

void ipc::initializeDiscordAuth(std::string clientId, std::string accessToken) {
    discordClient().updateCredentials(std::move(clientId), std::move(accessToken));
}

void ipc::setDeafened(bool deafened) {
    discordClient().setDeafened(deafened);
}
