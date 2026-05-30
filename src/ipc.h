#pragma once

#include <Geode/Geode.hpp>
#include "globals.h"

#include <thread>
#include <atomic>
#include <string>
#include <mutex>
#include <cstdint>

// fuck the ws2 library bro
#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <unistd.h>
    #include <fcntl.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <sys/un.h>
    #include <cstring>
    #include <cerrno>
    #include <cstdlib>
    #include <unordered_map>
    #define HANDLE int
    #define INVALID_HANDLE_VALUE -1
    #define GENERIC_READ O_RDONLY
    #define GENERIC_WRITE O_WRONLY
    #define DWORD std::uint32_t
    #define INFINITE -1
    #define TRUE true
    #define FALSE false
    #define WAIT_OBJECT_0 0
    #define WAIT_TIMEOUT 1
    #define ERROR_IO_PENDING EAGAIN
    #define OPEN_EXISTING 0
    #define FILE_FLAG_OVERLAPPED 0 

    struct OVERLAPPED {
        int hEvent = -1;
        int fd = INVALID_HANDLE_VALUE;
        void* buffer = nullptr;
        DWORD size = 0;
        DWORD transferred = 0;
        bool pending = false;
    };

    inline std::atomic<int> nextEvent = -2;
    inline std::mutex eventMutex;
    inline std::unordered_map<int, int> eventFds;
    inline thread_local int lastError = 0;

    inline void setLastError(int err) {
        lastError = err;
        errno = err;
    }

    inline int CreateEvent(void*, bool, bool, void*) {
        return nextEvent--;
    }

    inline void CloseHandle(int handle) {
        if (handle < -1) {
            std::lock_guard lock(eventMutex);
            eventFds.erase(handle);
            return;
        }
        if (handle >= 0) close(handle);
    }

    inline void ResetEvent(int event) {
        if (event < -1) {
            std::lock_guard lock(eventMutex);
            eventFds.erase(event);
        }
    }

    inline bool waitForFd(int fd, short events, DWORD timeout) {
        int pollTimeout = timeout == static_cast<DWORD>(INFINITE) ? -1 : static_cast<int>(timeout);
        pollfd pfd{fd, events, 0};

        while (true) {
            int result = poll(&pfd, 1, pollTimeout);
            if (result > 0) return (pfd.revents & (events | POLLHUP | POLLERR)) != 0;
            if (result == 0) {
                setLastError(EAGAIN);
                return false;
            }
            if (errno != EINTR) {
                setLastError(errno);
                return false;
            }
        }
    }

    inline bool finishRead(OVERLAPPED* overlapped, DWORD timeout = 0) {
        auto* bytes = static_cast<char*>(overlapped->buffer);

        while (overlapped->transferred < overlapped->size) {
            ssize_t result = read(
                overlapped->fd,
                bytes + overlapped->transferred,
                overlapped->size - overlapped->transferred
            );

            if (result > 0) {
                overlapped->transferred += static_cast<DWORD>(result);
                continue;
            }
            if (result == 0) {
                overlapped->pending = false;
                setLastError(0);
                return false;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                overlapped->pending = true;
                setLastError(EAGAIN);
                if (timeout == 0 || !waitForFd(overlapped->fd, POLLIN, timeout)) return false;
                continue;
            }

            overlapped->pending = false;
            setLastError(errno);
            return false;
        }

        overlapped->pending = false;
        setLastError(0);
        return true;
    }

    inline bool WriteFile(int fd, const void* buf, DWORD size, DWORD* written, OVERLAPPED* overlapped) {
        DWORD total = 0;
        auto* bytes = static_cast<const char*>(buf);

        while (total < size) {
            ssize_t result = write(fd, bytes + total, size - total);
            if (result > 0) {
                total += static_cast<DWORD>(result);
                continue;
            }
            if (result < 0 && errno == EINTR) continue;
            if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (waitForFd(fd, POLLOUT, static_cast<DWORD>(INFINITE))) continue;
            }

            if (written) *written = total;
            if (overlapped) {
                overlapped->fd = fd;
                overlapped->size = size;
                overlapped->transferred = total;
                overlapped->pending = false;
            }
            setLastError(result < 0 ? errno : EIO);
            return false;
        }

        if (written) *written = total;
        if (overlapped) {
            overlapped->fd = fd;
            overlapped->size = size;
            overlapped->transferred = total;
            overlapped->pending = false;
        }
        setLastError(0);
        return true;
    }

    inline bool ReadFile(int fd, void* buf, DWORD size, DWORD* bytesRead, OVERLAPPED* overlapped) {
        if (!overlapped) {
            OVERLAPPED local;
            return ReadFile(fd, buf, size, bytesRead, &local);
        }

        overlapped->fd = fd;
        overlapped->buffer = buf;
        overlapped->size = size;
        overlapped->transferred = 0;
        overlapped->pending = false;

        if (overlapped->hEvent < -1) {
            std::lock_guard lock(eventMutex);
            eventFds[overlapped->hEvent] = fd;
        }

        bool complete = finishRead(overlapped);
        if (bytesRead) *bytesRead = overlapped->transferred;
        return complete;
    }

    inline DWORD WaitForSingleObject(int event, DWORD timeout) {
        int fd = event;
        if (event < -1) {
            std::lock_guard lock(eventMutex);
            auto it = eventFds.find(event);
            if (it == eventFds.end()) return WAIT_OBJECT_0;
            fd = it->second;
        }

        return waitForFd(fd, POLLIN, timeout) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    }

    inline bool GetOverlappedResult(int, OVERLAPPED* overlapped, DWORD* transferred, bool wait) {
        bool complete = !overlapped->pending || finishRead(overlapped, wait ? static_cast<DWORD>(INFINITE) : 1000);
        if (transferred) *transferred = overlapped->transferred;
        return complete;
    }

    inline DWORD GetLastError() {
        return static_cast<DWORD>(lastError);
    }

    inline void CancelIo(int) {} // no-op

    // replaces CreateFileW for Discord Unix socket
    inline HANDLE CreateFileW(const wchar_t* path, int, int, void*, int, int, void*) {
        // convert wchar path to narrow string
        std::string narrow;
        for (int i = 0; path[i]; i++) narrow += (char)path[i];

        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return INVALID_HANDLE_VALUE;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, narrow.c_str(), sizeof(addr.sun_path) - 1);

        int result = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
        if (result != 0 && errno != EINPROGRESS) {
            close(fd);
            return INVALID_HANDLE_VALUE;
        }
        if (result != 0) {
            if (!waitForFd(fd, POLLOUT, 1000)) {
                close(fd);
                return INVALID_HANDLE_VALUE;
            }

            int error = 0;
            socklen_t errorLength = sizeof(error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &errorLength) != 0 || error != 0) {
                close(fd);
                setLastError(error ? error : errno);
                return INVALID_HANDLE_VALUE;
            }
        }
        return fd;
    }
#endif

namespace ipc {

    inline std::atomic authenticated = false;

    inline HANDLE discordPipe = INVALID_HANDLE_VALUE;
    inline std::thread drainThread;
    inline std::mutex pipeMutex;

    // discord has 10 possible ipc pipes (for some reason??) so gotta check them all lmao
    inline HANDLE connectToDiscord() {
    for (int i = 0; i < 10; i++) {
    #ifdef _WIN32
        std::wstring pipe = L"\\\\?\\pipe\\discord-ipc-" + std::to_wstring(i);
        HANDLE hPipe = CreateFileW(pipe.c_str(), GENERIC_READ|GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    #else
        const char* tmp = std::getenv("XDG_RUNTIME_DIR");
        if (!tmp) tmp = std::getenv("TMPDIR");
        if (!tmp) tmp = "/tmp";
        std::wstring pipe = std::wstring(tmp, tmp + strlen(tmp)) + L"/discord-ipc-" + std::to_wstring(i);
        HANDLE hPipe = CreateFileW(pipe.c_str(), 0, 0, nullptr, 0, 0, nullptr);
    #endif
        if (hPipe != INVALID_HANDLE_VALUE) return hPipe;
    }
    return INVALID_HANDLE_VALUE;
}

    inline void sendFrame(int opcode, const std::string& json) {

        std::lock_guard lock(pipeMutex);

        // prevents the drain pipe reads racing with my frame sending
        CancelIo(discordPipe);

        OVERLAPPED overlapped = {0};
        overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        DWORD written;

        int32_t op = opcode;
        auto len = (int32_t)json.size();

        WriteFile(discordPipe, &op, 4, &written, &overlapped);
        WaitForSingleObject(overlapped.hEvent, INFINITE);
        GetOverlappedResult(discordPipe, &overlapped, &written, FALSE);

        ResetEvent(overlapped.hEvent);
        WriteFile(discordPipe, &len, 4, &written, &overlapped);
        WaitForSingleObject(overlapped.hEvent, INFINITE);
        GetOverlappedResult(discordPipe, &overlapped, &written, FALSE);

        ResetEvent(overlapped.hEvent);
        WriteFile(discordPipe, json.data(), len, &written, &overlapped);
        WaitForSingleObject(overlapped.hEvent, INFINITE);
        GetOverlappedResult(discordPipe, &overlapped, &written, FALSE);

        CloseHandle(overlapped.hEvent);
    }

    // reads/discards one frame, I dont actually care about the contents but need to make sure packets are sent in order
    inline bool drainFrame(HANDLE pipe) {
        int32_t opcode = 0, length = 0;
        DWORD read = 0;
        OVERLAPPED overlapped = {0};
        overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

        auto cleanup = [&](bool result) {
            CloseHandle(overlapped.hEvent);
            return result;
        };

        if (!ReadFile(pipe, &opcode, 4, &read, &overlapped) && GetLastError() == ERROR_IO_PENDING) WaitForSingleObject(overlapped.hEvent, 5000);
        if (!GetOverlappedResult(pipe, &overlapped, &read, FALSE) || read != 4) return cleanup(false);

        ResetEvent(overlapped.hEvent);
        if (!ReadFile(pipe, &length, 4, &read, &overlapped) && GetLastError() == ERROR_IO_PENDING) WaitForSingleObject(overlapped.hEvent, 5000);
        if (!GetOverlappedResult(pipe, &overlapped, &read, FALSE) || read != 4) return cleanup(false);

        std::string payload(length, '\0');
        ResetEvent(overlapped.hEvent);
        if (!ReadFile(pipe, payload.data(), length, &read, &overlapped) && GetLastError() == ERROR_IO_PENDING) WaitForSingleObject(overlapped.hEvent, 5000);
        if (!GetOverlappedResult(pipe, &overlapped, &read, FALSE) || read != (DWORD)length) return cleanup(false);
        // log::info("rec frame | op={} | len={} | payload={}", opcode, length, payload);

        return cleanup(true);
    }

    // this is necessary cause discord sends a lot of unsolicited shit and it will otherwise eventually start to fail after a while of using it
    inline void drainLoop(HANDLE pipe) {
        while (pipe != INVALID_HANDLE_VALUE && authenticated) {
            int32_t opcode = 0, length = 0;
            DWORD read = 0;
            OVERLAPPED overlapped = {0};
            overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

            // std::lock_guard lock(pipeMutex);

            if (!ReadFile(pipe, &opcode, 4, &read, &overlapped)) {
                if (GetLastError() != ERROR_IO_PENDING) { CloseHandle(overlapped.hEvent); break; }
            }
            DWORD waitResult = WaitForSingleObject(overlapped.hEvent, 100);
            if (waitResult == WAIT_TIMEOUT) { CancelIo(pipe); CloseHandle(overlapped.hEvent); continue; }
            if (!GetOverlappedResult(pipe, &overlapped, &read, FALSE) || read != 4) { CloseHandle(overlapped.hEvent); break; }

            ResetEvent(overlapped.hEvent);
            if (!ReadFile(pipe, &length, 4, &read, &overlapped)) {
                if (GetLastError() != ERROR_IO_PENDING) { CloseHandle(overlapped.hEvent); break; }
            }
            if (WaitForSingleObject(overlapped.hEvent, 1000) != WAIT_OBJECT_0 ||
                !GetOverlappedResult(pipe, &overlapped, &read, FALSE) || read != 4) { CloseHandle(overlapped.hEvent); break; }

            std::string payload(length, '\0');
            ResetEvent(overlapped.hEvent);
            if (!ReadFile(pipe, payload.data(), length, &read, &overlapped)) {
                if (GetLastError() != ERROR_IO_PENDING) { CloseHandle(overlapped.hEvent); break; }
            }
            if (WaitForSingleObject(overlapped.hEvent, 1000) != WAIT_OBJECT_0 ||
                !GetOverlappedResult(pipe, &overlapped, &read, FALSE) || read != (DWORD)length) { CloseHandle(overlapped.hEvent); break; }

            CloseHandle(overlapped.hEvent);
        }

        //log::info("pipe invalidated");
        authenticated = false;
        discordPipe = INVALID_HANDLE_VALUE;

    }

    inline bool initializeDiscordAuth() {
        HANDLE pipe = connectToDiscord();
        if (pipe == INVALID_HANDLE_VALUE) return false;

        discordPipe = pipe;

        // have to make everyone make their own application because IPC has been in a "private beta" for 7 fucking years
        // genuinely so fed up with discord bro I made a whole mod and set up a whole web server to find out I can't use my own oauth for ipc
        sendFrame(0, R"({"v": 1, "client_id": ")" + CLIENT_ID + "\"}");
        if (!drainFrame(pipe)) { CloseHandle(pipe); discordPipe = INVALID_HANDLE_VALUE; return false; }

        sendFrame(1, R"({"cmd": "AUTHENTICATE", "args": {"access_token": ")" + DISCORD_ACCESS_TOKEN + R"("},"nonce": "auth"})");
        if (!drainFrame(pipe)) { CloseHandle(pipe); discordPipe = INVALID_HANDLE_VALUE; return false; }

        authenticated = true;

        drainThread = std::thread(drainLoop, pipe);
        drainThread.detach();

        return true;
    }

    // todo: there's still a fucking lagspike like 40% of the time need to find out why
    inline void deafen(bool deafen) {
        if (!authenticated || discordPipe == INVALID_HANDLE_VALUE) return;

        // async because there's otherwise a lagspike on deafen and people will 100% complain
        // async::spawn(nullptr, [] -> arc::Future<> {
        //
        // });

        // todo replace this with the geode way of async when I figure out how to do that
        std::thread([deafen] {
            sendFrame(1, R"({"cmd": "SET_VOICE_SETTINGS", "args": { "deaf": )" + std::string(deafen ? "true" : "false") + R"( }, "nonce": "deafen" })");
        }).detach();
    }

}
