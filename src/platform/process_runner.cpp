#include "platform/process_runner.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace litewin {
namespace {
struct ScopedFd {
    int fd = -1;
    explicit ScopedFd(int f = -1) noexcept : fd(f) {}
    ~ScopedFd() { reset(); }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    void reset() noexcept {
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
};

// Keep redirected standard descriptors distinct from their original endpoints,
// even when the caller has closed stdin, stdout, or stderr.
bool make_channel(ScopedFd& read_end, ScopedFd& write_end, bool socket) {
    int fds[2];
    const int rc = socket ? ::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds)
                          : ::pipe2(fds, O_CLOEXEC);
    if (rc < 0) return false;
    read_end.fd = fds[0];
    write_end.fd = fds[1];
    for (auto* end : {&read_end, &write_end}) {
        if (end->fd < 3) {
            const int replacement = ::fcntl(end->fd, F_DUPFD_CLOEXEC, 3);
            if (replacement < 0) return false;
            end->reset();
            end->fd = replacement;
        }
    }
    return true;
}
} // namespace

ProcessResult ProcessRunner::run(const std::string& executable,
                                const std::vector<std::string>& arguments,
                                const std::string& stdin_input) {
    return run_streaming(executable, arguments, nullptr, stdin_input);
}

ProcessResult ProcessRunner::run_streaming(
    const std::string& executable,
    const std::vector<std::string>& arguments,
    const std::function<void(std::string_view)>& on_output_chunk,
    const std::string& stdin_input) {
    ProcessResult result;
    ScopedFd input_read, input_write, output_read, output_write, error_read, error_write;
    // A stream socket supports ordinary child stdin reads while MSG_NOSIGNAL
    // suppresses SIGPIPE for this write only, without changing signal handlers.
    if (!make_channel(input_read, input_write, true) ||
        !make_channel(output_read, output_write, false) ||
        !make_channel(error_read, error_write, false)) {
        result.error_message = "Failed to create channels: " + std::string(std::strerror(errno));
        return result;
    }
    const int flags = ::fcntl(input_write.fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(input_write.fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        result.error_message = "Failed to configure stdin: " + std::string(std::strerror(errno));
        return result;
    }
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const auto& arg : arguments) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        result.error_message = "Fork failed: " + std::string(std::strerror(errno));
        return result;
    }
    if (pid == 0) {
        if (::dup2(input_read.fd, STDIN_FILENO) < 0 ||
            ::dup2(output_write.fd, STDOUT_FILENO) < 0 ||
            ::dup2(error_write.fd, STDERR_FILENO) < 0) ::_exit(127);
        input_read.reset(); input_write.reset();
        output_read.reset(); output_write.reset();
        error_read.reset(); error_write.reset();
        ::execvp(executable.c_str(), argv.data());
        ::_exit(127);
    }
    input_read.reset(); output_write.reset(); error_write.reset();
    if (stdin_input.empty()) input_write.reset();
    std::size_t written = 0;
    std::array<char, 4096> buffer{};
    while (input_write.fd >= 0 || output_read.fd >= 0 || error_read.fd >= 0) {
        std::array<pollfd, 3> pfds{{
            {input_write.fd, POLLOUT, 0},
            {output_read.fd, POLLIN, 0},
            {error_read.fd, POLLIN, 0},
        }};
        if (::poll(pfds.data(), pfds.size(), -1) < 0) {
            if (errno == EINTR) continue;
            result.error_message = "poll failed: " + std::string(std::strerror(errno));
            break;
        }
        if (input_write.fd >= 0 && pfds[0].revents) {
            const auto count = ::send(input_write.fd, stdin_input.data() + written,
                                      stdin_input.size() - written, MSG_NOSIGNAL);
            if (count > 0) {
                written += static_cast<std::size_t>(count);
                if (written == stdin_input.size()) input_write.reset();
            } else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                result.error_message = "stdin write failed: " + std::string(std::strerror(errno));
                input_write.reset();
            }
        }
        for (int i = 1; i <= 2; ++i) {
            auto& source = i == 1 ? output_read : error_read;
            auto& output = i == 1 ? result.stdout_output : result.stderr_output;
            if (source.fd < 0 || !pfds[i].revents) continue;
            const auto count = ::read(source.fd, buffer.data(), buffer.size());
            if (count > 0) {
                output.append(buffer.data(), static_cast<std::size_t>(count));
                if (on_output_chunk) {
                    on_output_chunk(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
                }
            }
            else if (count == 0) source.reset();
            else if (errno != EINTR && errno != EAGAIN) {
                result.error_message = "output read failed: " + std::string(std::strerror(errno));
                source.reset();
            }
        }
    }
    input_write.reset(); output_read.reset(); error_read.reset();
    int status = 0;
    pid_t waited;
    do { waited = ::waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        result.error_message = "waitpid failed: " + std::string(std::strerror(errno));
        return result;
    }
    result.executed = result.error_message.empty();
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
        if (result.exit_code == 127) {
            result.executed = false;
            result.error_message = "Failed to execute: " + executable;
        }
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
        result.error_message = "Process terminated by signal";
    }
    return result;
}
} // namespace litewin
