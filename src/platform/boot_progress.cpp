// A small standalone renderer shared by initramfs and the live environment.
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <linux/kd.h>
#include <linux/vt.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
const std::string directory = getenv("WINSTALLER_BOOT_STATE_DIR") ? getenv("WINSTALLER_BOOT_STATE_DIR") : "/run/winstaller-boot";
volatile std::sig_atomic_t interrupted = 0;
void signal_stop(int) { interrupted = 1; }
std::string read_file(const std::string& path, size_t limit = 16384) {
    std::ifstream in(path);
    std::string data(limit, '\0');
    in.read(data.data(), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<size_t>(in.gcount()));
    return data;
}
bool exists(const std::string& name) { return access((directory + "/" + name).c_str(), F_OK) == 0; }
bool write_file(const std::string& name, const std::string& value) {
    const std::string path = directory + "/" + name;
    const std::string temporary = path + "." + std::to_string(getpid());
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    size_t offset = 0;
    while (offset < value.size()) {
        const auto n = write(fd, value.data() + offset, value.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        offset += static_cast<size_t>(n);
    }
    const bool closed = close(fd) == 0;
    const bool ok = offset == value.size() && closed && rename(temporary.c_str(), path.c_str()) == 0;
    if (!ok) unlink(temporary.c_str());
    return ok;
}
std::string printable(std::string value) {
    for (auto& c : value) if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126) c = ' ';
    return value;
}
int number(const char* value) {
    char* end = nullptr;
    const long n = strtol(value, &end, 10);
    return end != value && *end == '\0' && n >= 0 && n <= 100 ? static_cast<int>(n) : -1;
}
bool state(int percent, const std::string& message) {
    int lock = open((directory + "/state.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0) return false;
    if (flock(lock, LOCK_EX) != 0) { close(lock); return false; }
    const auto old = read_file(directory + "/state");
    const int previous = atoi(old.c_str());
    const int next = std::max(previous, percent);
    const auto text = std::to_string(next) + "\n" + printable(message.substr(0, 140)) + "\n";
    const bool ok = write_file("state", text);
    if (ok && old != text) {
        std::ofstream log(directory + "/events.log", std::ios::app);
        log << next << "% " << printable(message) << '\n';
    }
    if (exists("text-only")) {
        int console = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
        if (console >= 0) { dprintf(console, "Starting Winstaller ... %d%% %s\n", next, printable(message).c_str()); close(console); }
    }
    flock(lock, LOCK_UN); close(lock);
    return ok;
}
std::string journal_tail() {
    if (access("/usr/bin/journalctl", X_OK) != 0) return {};
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return {};
    const pid_t child = fork();
    if (child == 0) {
        dup2(fds[1], STDOUT_FILENO);
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) dup2(null, STDERR_FILENO);
        close(fds[0]); close(fds[1]);
        execl("/usr/bin/journalctl", "journalctl", "-b", "--no-pager", "-n", "16", "-o", "cat", nullptr);
        _exit(127);
    }
    close(fds[1]);
    if (child < 0) { close(fds[0]); return {}; }
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    std::string result; char buffer[2048];
    const auto deadline = Clock::now() + std::chrono::milliseconds(350);
    bool done = false;
    while (Clock::now() < deadline && result.size() < 16384) {
        auto n = read(fds[0], buffer, sizeof(buffer));
        if (n > 0) result.append(buffer, static_cast<size_t>(n));
        else if (n == 0) break;
        else if (errno != EAGAIN && errno != EINTR) break;
        pollfd pfd{fds[0], POLLIN, 0}; poll(&pfd, 1, 20);
    }
    done = waitpid(child, nullptr, WNOHANG) == child;
    if (!done) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
    close(fds[0]);
    return result;
}
void activate(int vt) {
    int fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (fd >= 0) { ioctl(fd, VT_ACTIVATE, vt); close(fd); }
}
std::string version() {
    auto value = printable(read_file("/etc/winstaller-version", 40));
    while (!value.empty() && value.back() == ' ') value.pop_back();
    return value.empty() ? "development" : value;
}
void draw(int fd, bool details) {
    winsize size{};
    if (ioctl(fd, TIOCGWINSZ, &size) != 0 || size.ws_col < 20 || size.ws_row < 6) {
        const auto current = read_file(directory + "/state");
        dprintf(fd, "\rStarting Winstaller (%s) ... %s\n", version().c_str(), printable(current).c_str());
        return;
    }
    const auto current = read_file(directory + "/state");
    const int percent = std::clamp(atoi(current.c_str()), 0, 100);
    const auto split = current.find('\n');
    const auto message = split == std::string::npos ? "Preparing" : printable(current.substr(split + 1));
    const int width = std::min(88, static_cast<int>(size.ws_col) - 8);
    const int col = std::max(1, (static_cast<int>(size.ws_col) - width) / 2);
    const int row = std::max(2, static_cast<int>(size.ws_row) - 6);
    std::string cells;
    for (int i = 0; i < width - 2; ++i) cells += i * 100 < percent * (width - 2) ? "█" : "░";
    const std::string title = "Starting Winstaller (" + version() + ") ... " + message;
    dprintf(fd, "\033%%G\033[?25l\033[0;37;40m\033[2J\033[H");
    if (details) {
        std::string data = read_file("/run/initramfs/init.log") + "\n" + read_file(directory + "/events.log") + "\n" + journal_tail();
        std::vector<std::string> lines; std::istringstream in(data); std::string line;
        while (std::getline(in, line)) lines.push_back(printable(line));
        const int count = std::min(22, row - 3);
        const size_t first = lines.size() > static_cast<size_t>(count) ? lines.size() - count : 0;
        int y = 2;
        for (size_t i = first; i < lines.size(); ++i)
            dprintf(fd, "\033[%d;2H%s", y++, lines[i].substr(0, size.ws_col - 4).c_str());
    }
    dprintf(fd, "\033[%d;%dH%s  %d%%\033[%d;%dH[%s]\033[%d;%dHF2: %s",
            row, col, title.substr(0, width - 6).c_str(), percent,
            row + 1, col, cells.c_str(), row + 3, col, details ? "Return to progress" : "Boot details");
}
int serve() {
    int lock = open((directory + "/renderer.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0) return 1;
    if (flock(lock, LOCK_EX | LOCK_NB) != 0) { close(lock); return 0; }
    int fd = open("/dev/tty8", O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    int mode = -1;
    if (fd < 0 || ioctl(fd, KDGETMODE, &mode) != 0 || mode != KD_TEXT) {
        if (fd >= 0) close(fd);
        write_file("text-only", "yes\n");
        close(lock); activate(1); return 1;
    }
    termios original{};
    const bool have_termios = tcgetattr(fd, &original) == 0;
    if (have_termios) { auto raw = original; cfmakeraw(&raw); tcsetattr(fd, TCSANOW, &raw); }
    signal(SIGTERM, signal_stop); signal(SIGINT, signal_stop);
    activate(8);
    bool details = false;
    std::string keys, previous;
    winsize old_size{};
    auto last = Clock::time_point{};
    draw(fd, false);
    while (!interrupted && !exists("pause") && !exists("handoff")) {
        char input[64]; const auto n = read(fd, input, sizeof(input));
        if (n > 0) {
            keys.append(input, static_cast<size_t>(n));
            if (keys.find("\033[[B") != std::string::npos || keys.find("\033OQ") != std::string::npos) {
                details = !details; keys.clear(); last = {};
            }
            if (keys.size() > 24) keys.erase(0, keys.size() - 8);
        }
        if ((n < 0 && errno == EIO) || n == 0) {
            const int replacement = open("/dev/tty8", O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
            if (replacement >= 0) {
                close(fd); fd = replacement;
                termios raw{};
                if (tcgetattr(fd, &raw) == 0) { cfmakeraw(&raw); tcsetattr(fd, TCSANOW, &raw); }
                last = {};
            }
        }
        const auto current = read_file(directory + "/state");
        winsize size{}; ioctl(fd, TIOCGWINSZ, &size);
        const bool resized = size.ws_col != old_size.ws_col || size.ws_row != old_size.ws_row;
        const auto now = Clock::now();
        if (now - last >= std::chrono::milliseconds(250) &&
            (current != previous || resized || now - last >= std::chrono::seconds(details ? 1 : 2))) {
            draw(fd, details); previous = current; old_size = size; last = now;
        }
        // A VT hangup can report readiness continuously. Keep the loop bounded.
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    const bool handoff = exists("handoff");
    if (have_termios) tcsetattr(fd, TCSANOW, &original);
    close(fd);
    if (handoff || interrupted) activate(1);
    write_file(handoff ? "handed-off" : "paused", "yes\n");
    close(lock);
    return 0;
}
int stop(bool handoff) {
    if (!write_file(handoff ? "handoff" : "pause", "yes\n")) return 1;
    const auto end = Clock::now() + std::chrono::seconds(2);
    while (!exists(handoff ? "handed-off" : "paused") && Clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (handoff) activate(1);
    return 0;
}
// The copy child uses the original cp and its exit code, including ENOSPC.
int copy_image(const char* source, const char* target) {
    struct stat src{};
    if (stat(source, &src) != 0 || !S_ISREG(src.st_mode) || src.st_size <= 0) return 1;
    state(15, "Copying system image to RAM");
    const pid_t child = fork();
    if (child == 0) { execlp("cp", "cp", "--", source, target, nullptr); _exit(127); }
    if (child < 0) return 1;
    int status = 0; bool failed = false;
    while (true) {
        const auto result = waitpid(child, &status, WNOHANG);
        if (result == child) break;
        if (result < 0) {
            if (errno == EINTR) continue;
            state(15, "RAM copy failed - cannot wait for copier");
            return 1;
        }
        struct stat dst{};
        if (stat(target, &dst) == 0) {
            const auto copied = std::clamp<off_t>(dst.st_size, 0, src.st_size);
            const int percent = 15 + static_cast<int>(64.0L * copied / src.st_size);
            state(percent, "Copying system image to RAM");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    struct stat dst{};
    failed = !WIFEXITED(status) || WEXITSTATUS(status) != 0 || stat(target, &dst) != 0 || dst.st_size != src.st_size;
    if (failed) { state(15, "RAM copy failed - see boot details"); return 1; }
    state(80, "Mounting the live system");
    return 0;
}
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string command = argv[1];
    if (command == "--start") {
        if (mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) return 1;
        if (!write_file("enabled", "1\n") || !state(10, "Detecting boot media")) return 1;
        const auto child = fork();
        if (child < 0) return 1;
        if (child == 0) { setsid(); _exit(serve()); }
        return 0;
    }
    if (command == "--copy" && argc == 4) return copy_image(argv[2], argv[3]);
    if (!exists("enabled")) return 0;
    if (command == "--resume") {
        unlink((directory + "/pause").c_str());
        unlink((directory + "/paused").c_str());
        return 0;
    }
    if (command == "--serve") return serve();
    if (command == "--pause") return stop(false);
    if (command == "--handoff") return stop(true);
    if (command == "--emergency") {
        stop(true);
        int fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
        if (fd >= 0) {
            dprintf(fd, "\nWinstaller boot recovery: %s\nDetails: /run/initramfs/init.log and /run/winstaller-boot/events.log\n",
                printable(read_file(directory + "/state")).c_str());
            close(fd);
        }
        return 0;
    }
    if (command == "--state" && argc == 4) {
        const int percent = number(argv[2]);
        return percent < 0 ? 2 : !state(percent, argv[3]);
    }
    if (command == "--ready") {
        state(100, "Ready");
        int fd = open("/dev/tty1", O_WRONLY | O_NOCTTY | O_CLOEXEC);
        if (fd >= 0) { draw(fd, false); close(fd); }
        return 0;
    }
    return 2;
}
