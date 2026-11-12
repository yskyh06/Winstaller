#include "ui/console_mouse.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <string>

namespace litewin {

namespace {

constexpr size_t kMaxDevices = 32;
constexpr size_t kMaxOutputQueueSize = 256;
constexpr size_t kMaxDecoderQueueSize = 256;

inline bool test_bit_array(int bit, const unsigned long* array) {
    constexpr size_t bits_per_long = sizeof(unsigned long) * 8;
    return (array[bit / bits_per_long] & (1UL << (bit % bits_per_long))) != 0;
}

uint64_t get_current_time_ms() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

} // namespace

// -----------------------------------------------------------------------------
// MouseDecoder
// -----------------------------------------------------------------------------

MouseDecoder::MouseDecoder(int rows, int cols) {
    set_bounds(rows, cols);
}

void MouseDecoder::set_bounds(int rows, int cols) {
    int new_rows = std::max(1, rows);
    int new_cols = std::max(1, cols);

    // Do not reset on every same-size call
    if (new_rows == rows_ && new_cols == cols_) {
        return;
    }

    rows_ = new_rows;
    cols_ = new_cols;
    clamp_cursor();

    // Screen resize clamps cursor, clears pending decoded events and staging
    while (!event_queue_.empty()) {
        event_queue_.pop();
    }
    staged_rel_x_ = 0;
    staged_rel_y_ = 0;
    staged_wheel_ = 0;
    staged_abs_x_.reset();
    staged_abs_y_.reset();
    rel_accum_x_ = 0;
    rel_accum_y_ = 0;

    // Suppress held presses until physical release
    if (button_down_ || staged_btn_left_.value_or(false)) {
        suppress_clicks_until_release_ = true;
    }
    button_down_ = false;
    staged_btn_left_.reset();
    if (right_button_down_ || staged_btn_right_.value_or(false)) {
        suppress_right_clicks_until_release_ = true;
    }
    right_button_down_ = false;
    staged_btn_right_.reset();
    staged_button_edges_.clear();
}

void MouseDecoder::set_position(int row, int col) {
    current_row_ = std::clamp(row, 1, rows_);
    current_col_ = std::clamp(col, 1, cols_);
}

void MouseDecoder::set_abs_x_info(int32_t minimum, int32_t maximum) {
    abs_x_.minimum = minimum;
    abs_x_.maximum = std::max(minimum, maximum);
    has_abs_x_ = true;
}

void MouseDecoder::set_abs_y_info(int32_t minimum, int32_t maximum) {
    abs_y_.minimum = minimum;
    abs_y_.maximum = std::max(minimum, maximum);
    has_abs_y_ = true;
}

void MouseDecoder::set_rel_step(int step_x, int step_y) {
    rel_step_x_ = std::max(1, step_x);
    rel_step_y_ = std::max(1, step_y);
}

void MouseDecoder::clamp_cursor() {
    current_row_ = std::clamp(current_row_, 1, rows_);
    current_col_ = std::clamp(current_col_, 1, cols_);
}

void MouseDecoder::resync_state(bool is_btn_left_down,
                                std::optional<int32_t> abs_x,
                                std::optional<int32_t> abs_y,
                                bool is_btn_right_down) {
    needs_resync_ = false;

    // Resync hardware state without inventing edge clicks
    if (is_btn_left_down) {
        button_down_ = true;
        suppress_clicks_until_release_ = true;
    } else {
        button_down_ = false;
        suppress_clicks_until_release_ = false;
    }

    if (is_btn_right_down) {
        right_button_down_ = true;
        suppress_right_clicks_until_release_ = true;
    } else {
        right_button_down_ = false;
        suppress_right_clicks_until_release_ = false;
    }

    if (abs_x.has_value() && has_abs_x_) {
        int64_t min_x = abs_x_.minimum;
        int64_t max_x = abs_x_.maximum;
        int64_t span_x = max_x - min_x;
        int64_t clamped_x = std::clamp<int64_t>(*abs_x, min_x, max_x);
        if (span_x > 0 && cols_ > 1) {
            int64_t scaled = (clamped_x - min_x) * static_cast<int64_t>(cols_);
            current_col_ = 1 + static_cast<int>(std::min<int64_t>(cols_ - 1, scaled / span_x));
        } else {
            current_col_ = 1;
        }
    }

    if (abs_y.has_value() && has_abs_y_) {
        int64_t min_y = abs_y_.minimum;
        int64_t max_y = abs_y_.maximum;
        int64_t span_y = max_y - min_y;
        int64_t clamped_y = std::clamp<int64_t>(*abs_y, min_y, max_y);
        if (span_y > 0 && rows_ > 1) {
            int64_t scaled = (clamped_y - min_y) * static_cast<int64_t>(rows_);
            current_row_ = 1 + static_cast<int>(std::min<int64_t>(rows_ - 1, scaled / span_y));
        } else {
            current_row_ = 1;
        }
    }

    clamp_cursor();
}

void MouseDecoder::reset(bool left, bool right) {
    while (!event_queue_.empty()) {
        event_queue_.pop();
    }
    staged_rel_x_ = 0;
    staged_rel_y_ = 0;
    staged_wheel_ = 0;
    staged_abs_x_.reset();
    staged_abs_y_.reset();
    staged_btn_left_.reset();
    staged_btn_right_.reset();
    staged_button_edges_.clear();
    rel_accum_x_ = 0;
    rel_accum_y_ = 0;
    dropping_ = false;
    needs_resync_ = false;

    if (left) {
        button_down_ = true;
        suppress_clicks_until_release_ = true;
    } else {
        button_down_ = false;
        suppress_clicks_until_release_ = false;
    }

    if (right) {
        right_button_down_ = true;
        suppress_right_clicks_until_release_ = true;
    } else {
        right_button_down_ = false;
        suppress_right_clicks_until_release_ = false;
    }
}

void MouseDecoder::feed(const struct input_event& ev) {
    if (ev.type == EV_SYN) {
        if (ev.code == SYN_DROPPED) {
            dropping_ = true;
            needs_resync_ = false;
            suppress_clicks_until_release_ = true;
            suppress_right_clicks_until_release_ = true;
            button_down_ = false;
            right_button_down_ = false;
            staged_btn_left_.reset();
            staged_btn_right_.reset();
            staged_button_edges_.clear();
            staged_rel_x_ = 0;
            staged_rel_y_ = 0;
            staged_wheel_ = 0;
            staged_abs_x_.reset();
            staged_abs_y_.reset();
            rel_accum_x_ = 0;
            rel_accum_y_ = 0;
            // Clear pending decoded queue to avoid phantom clicks
            while (!event_queue_.empty()) {
                event_queue_.pop();
            }
            event_queue_.push(MouseEvent{MouseKind::Cancel, current_row_, current_col_, 0});
            return;
        }
        if (ev.code == SYN_REPORT) {
            if (dropping_) {
                dropping_ = false;
                needs_resync_ = true;
                return;
            }
            commit_packet();
            return;
        }
        return;
    }

    if (dropping_) {
        // Evdev protocol: ignore all events up to next SYN_REPORT
        return;
    }

    if (ev.type == EV_REL) {
        if (ev.code == REL_X) {
            staged_rel_x_ = std::clamp<int64_t>(staged_rel_x_ + ev.value, -1'000'000, 1'000'000);
        } else if (ev.code == REL_Y) {
            staged_rel_y_ = std::clamp<int64_t>(staged_rel_y_ + ev.value, -1'000'000, 1'000'000);
        } else if (ev.code == REL_WHEEL) {
            staged_wheel_ = static_cast<int>(std::clamp<int64_t>(static_cast<int64_t>(staged_wheel_) + ev.value, -1000000, 1000000));
        }
    } else if (ev.type == EV_ABS) {
        if (ev.code == ABS_X) {
            staged_abs_x_ = ev.value;
        } else if (ev.code == ABS_Y) {
            staged_abs_y_ = ev.value;
        }
    } else if (ev.type == EV_KEY && (ev.code == BTN_LEFT || ev.code == BTN_RIGHT) && (ev.value == 0 || ev.value == 1)) {
        if (staged_button_edges_.size() >= 64) {
            reset(true, true);
            event_queue_.push(MouseEvent{MouseKind::Cancel, current_row_, current_col_, 0});
            dropping_ = true; // Query actual state after the next SYN_REPORT.
            return;
        }
        if (ev.code == BTN_LEFT) staged_btn_left_ = ev.value != 0;
        else staged_btn_right_ = ev.value != 0;
        staged_button_edges_.emplace_back(ev.code, ev.value != 0);
    }

}

void MouseDecoder::commit_packet() {
    int orig_row = current_row_;
    int orig_col = current_col_;

    // 1. Process relative movement with 64-bit safe accumulation
    if (staged_rel_x_ != 0 || staged_rel_y_ != 0) {
        rel_accum_x_ = std::clamp<int64_t>(rel_accum_x_ + staged_rel_x_, -1'000'000, 1'000'000);
        rel_accum_y_ = std::clamp<int64_t>(rel_accum_y_ + staged_rel_y_, -1'000'000, 1'000'000);
        staged_rel_x_ = 0;
        staged_rel_y_ = 0;

        int64_t delta_cols = rel_accum_x_ / rel_step_x_;
        int64_t delta_rows = rel_accum_y_ / rel_step_y_;
        rel_accum_x_ %= rel_step_x_;
        rel_accum_y_ %= rel_step_y_;

        int64_t next_col = static_cast<int64_t>(current_col_) + delta_cols;
        int64_t next_row = static_cast<int64_t>(current_row_) + delta_rows;

        current_col_ = static_cast<int>(std::clamp<int64_t>(next_col, 1, cols_));
        current_row_ = static_cast<int>(std::clamp<int64_t>(next_row, 1, rows_));
    }

    // 2. Process absolute movement avoiding signed overflow
    if (staged_abs_x_.has_value() && has_abs_x_) {
        int64_t min_x = abs_x_.minimum;
        int64_t max_x = abs_x_.maximum;
        int64_t span_x = max_x - min_x;
        int64_t clamped_x = std::clamp<int64_t>(*staged_abs_x_, min_x, max_x);
        if (span_x > 0 && cols_ > 1) {
            int64_t scaled = (clamped_x - min_x) * static_cast<int64_t>(cols_);
            current_col_ = 1 + static_cast<int>(std::min<int64_t>(cols_ - 1, scaled / span_x));
        } else {
            current_col_ = 1;
        }
        staged_abs_x_.reset();
    }

    if (staged_abs_y_.has_value() && has_abs_y_) {
        int64_t min_y = abs_y_.minimum;
        int64_t max_y = abs_y_.maximum;
        int64_t span_y = max_y - min_y;
        int64_t clamped_y = std::clamp<int64_t>(*staged_abs_y_, min_y, max_y);
        if (span_y > 0 && rows_ > 1) {
            int64_t scaled = (clamped_y - min_y) * static_cast<int64_t>(rows_);
            current_row_ = 1 + static_cast<int>(std::min<int64_t>(rows_ - 1, scaled / span_y));
        } else {
            current_row_ = 1;
        }
        staged_abs_y_.reset();
    }

    clamp_cursor();

    // Emit Move event if position changed and queue has capacity
    if ((current_row_ != orig_row || current_col_ != orig_col) && event_queue_.size() < kMaxDecoderQueueSize) {
        event_queue_.push(MouseEvent{MouseKind::Move, current_row_, current_col_, 0});
    }

    // 3. Process Wheel event
    if (staged_wheel_ != 0) {
        if (event_queue_.size() < kMaxDecoderQueueSize) {
            event_queue_.push(MouseEvent{MouseKind::Wheel, current_row_, current_col_, staged_wheel_});
        }
        staged_wheel_ = 0;
    }

    // Preserve every transition in the packet, including a quick press/release.
    for (const auto& [code, next] : staged_button_edges_) {
        const bool right = code == BTN_RIGHT;
        bool& down = right ? right_button_down_ : button_down_;
        bool& suppressed = right ? suppress_right_clicks_until_release_ : suppress_clicks_until_release_;
        if (suppressed) {
            down = next;
            if (!next) suppressed = false;
            continue;
        }
        if (next == down) continue;
        down = next;
        if (event_queue_.size() < kMaxDecoderQueueSize) {
            event_queue_.push(MouseEvent{
                right ? (next ? MouseKind::RightPress : MouseKind::RightRelease)
                      : (next ? MouseKind::LeftPress : MouseKind::LeftRelease),
                current_row_, current_col_, 0});
        }
    }
    staged_button_edges_.clear();
    staged_btn_left_.reset();
    staged_btn_right_.reset();
}

std::optional<MouseEvent> MouseDecoder::pop_event() {
    if (event_queue_.empty()) {
        return std::nullopt;
    }
    MouseEvent ev = event_queue_.front();
    event_queue_.pop();
    return ev;
}

bool MouseDecoder::has_events() const {
    return !event_queue_.empty();
}

// -----------------------------------------------------------------------------
// ConsoleMouse
// -----------------------------------------------------------------------------

ConsoleMouse::ConsoleMouse() = default;

ConsoleMouse::~ConsoleMouse() {
    close_all_devices();
}

ConsoleMouse::ConsoleMouse(ConsoleMouse&& other) noexcept
    : rows_(other.rows_),
      cols_(other.cols_),
      shared_row_(other.shared_row_),
      shared_col_(other.shared_col_),
      enabled_(other.enabled_),
      devices_(std::move(other.devices_)),
      output_queue_(std::move(other.output_queue_)),
      last_rescan_time_ms_(other.last_rescan_time_ms_) {
    other.enabled_ = false;
}

ConsoleMouse& ConsoleMouse::operator=(ConsoleMouse&& other) noexcept {
    if (this != &other) {
        close_all_devices();
        rows_ = other.rows_;
        cols_ = other.cols_;
        shared_row_ = other.shared_row_;
        shared_col_ = other.shared_col_;
        enabled_ = other.enabled_;
        devices_ = std::move(other.devices_);
        output_queue_ = std::move(other.output_queue_);
        last_rescan_time_ms_ = other.last_rescan_time_ms_;
        other.enabled_ = false;
    }
    return *this;
}

void ConsoleMouse::set_bounds(int rows, int cols) {
    int new_rows = std::max(1, rows);
    int new_cols = std::max(1, cols);
    if (new_rows == rows_ && new_cols == cols_) {
        return;
    }
    rows_ = new_rows;
    cols_ = new_cols;
    shared_row_ = std::clamp(shared_row_, 1, rows_);
    shared_col_ = std::clamp(shared_col_, 1, cols_);
    while (!output_queue_.empty()) {
        output_queue_.pop();
    }
    for (auto& dev : devices_) {
        dev.decoder.set_bounds(rows_, cols_);
    }
}

bool ConsoleMouse::check_active_vt_and_text_mode() const {
    int kd_mode = 0;
    if (::ioctl(STDIN_FILENO, KDGETMODE, &kd_mode) < 0) {
        return false;
    }
    if (kd_mode != KD_TEXT) {
        return false;
    }

    const char* stdin_tty = ::ttyname(STDIN_FILENO);
    if (!stdin_tty) {
        return false;
    }
    std::string stdin_tty_str(stdin_tty);
    if (stdin_tty_str.rfind("/dev/tty", 0) != 0) {
        return false;
    }
    std::string suffix = stdin_tty_str.substr(8);
    if (suffix.empty() || !std::all_of(suffix.begin(), suffix.end(), ::isdigit)) {
        return false;
    }

    // Check /sys/class/tty/tty0/active
    std::ifstream active_file("/sys/class/tty/tty0/active");
    if (active_file.is_open()) {
        std::string active_tty;
        if (active_file >> active_tty) {
            return active_tty == ("tty" + suffix);
        }
    }

    // Fallback to VT_GETSTATE ioctl
    struct vt_stat vstate{};
    if (::ioctl(STDIN_FILENO, VT_GETSTATE, &vstate) == 0) {
        try {
            unsigned short current_vt = static_cast<unsigned short>(std::stoi(suffix));
            return vstate.v_active == current_vt;
        } catch (...) {
            return false;
        }
    }

    return false;
}

void ConsoleMouse::set_enabled(bool enabled) {
    if (enabled == enabled_) {
        return;
    }

    if (enabled) {
        if (!check_active_vt_and_text_mode()) {
            enabled_ = false;
            return;
        }
        enabled_ = true;
        rescan_devices();
    } else {
        enabled_ = false;
        close_all_devices();
        while (!output_queue_.empty()) {
            output_queue_.pop();
        }
    }
}

void ConsoleMouse::close_all_devices() {
    for (auto& dev : devices_) {
        if (dev.fd >= 0) {
            ::close(dev.fd);
            dev.fd = -1;
        }
    }
    devices_.clear();
}

void ConsoleMouse::drain_device_input(int fd, int max_events) {
    if (fd < 0) return;
    struct input_event evs[32];
    int drained = 0;
    while (drained < max_events) {
        ssize_t n = ::read(fd, evs, sizeof(evs));
        if (n <= 0) break;
        drained += static_cast<int>(n / sizeof(struct input_event));
    }
}

void ConsoleMouse::reset() {
    while (!output_queue_.empty()) {
        output_queue_.pop();
    }
    for (auto& dev : devices_) {
        drain_device_input(dev.fd, 256);
        bool btn_left_down = false;
        bool btn_right_down = false;
        if (dev.fd >= 0) {
            unsigned long key_bits[(KEY_MAX + 1) / (sizeof(unsigned long) * 8) + 1] = {0};
            if (::ioctl(dev.fd, EVIOCGKEY(sizeof(key_bits)), key_bits) >= 0) {
                btn_left_down = test_bit_array(BTN_LEFT, key_bits);
                btn_right_down = test_bit_array(BTN_RIGHT, key_bits);
            }
        }
        dev.decoder.reset(btn_left_down, btn_right_down);
        if (dev.has_abs && dev.fd >= 0) {
            struct input_absinfo ax{}, ay{};
            std::optional<int32_t> abs_x, abs_y;
            if (::ioctl(dev.fd, EVIOCGABS(ABS_X), &ax) >= 0) abs_x = ax.value;
            if (::ioctl(dev.fd, EVIOCGABS(ABS_Y), &ay) >= 0) abs_y = ay.value;
            dev.decoder.resync_state(btn_left_down, abs_x, abs_y, btn_right_down);
        }
    }
}

void ConsoleMouse::rescan_devices() {
    DIR* dir = ::opendir("/dev/input");
    if (!dir) {
        return;
    }

    struct dirent* entry = nullptr;
    while ((entry = ::readdir(dir)) != nullptr) {
        if (devices_.size() >= kMaxDevices) {
            break;
        }

        if (std::strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        std::string full_path = std::string("/dev/input/") + entry->d_name;

        bool already_open = false;
        for (const auto& dev : devices_) {
            if (dev.path == full_path && dev.fd >= 0) {
                already_open = true;
                break;
            }
        }
        if (already_open) {
            continue;
        }

        int fd = ::open(full_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        unsigned long ev_bits[(EV_MAX + 1) / (sizeof(unsigned long) * 8) + 1] = {0};
        if (::ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
            ::close(fd);
            continue;
        }

        if (!test_bit_array(EV_KEY, ev_bits)) {
            ::close(fd);
            continue;
        }

        unsigned long key_bits[(KEY_MAX + 1) / (sizeof(unsigned long) * 8) + 1] = {0};
        if (::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
            ::close(fd);
            continue;
        }

        if (!test_bit_array(BTN_LEFT, key_bits)) {
            ::close(fd);
            continue;
        }

        bool has_rel = false;
        if (test_bit_array(EV_REL, ev_bits)) {
            unsigned long rel_bits[(REL_MAX + 1) / (sizeof(unsigned long) * 8) + 1] = {0};
            if (::ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits) >= 0) {
                has_rel = test_bit_array(REL_X, rel_bits) && test_bit_array(REL_Y, rel_bits);
            }
        }

        bool has_abs = false;
        struct input_absinfo abs_x_info{};
        struct input_absinfo abs_y_info{};
        if (test_bit_array(EV_ABS, ev_bits)) {
            unsigned long abs_bits[(ABS_MAX + 1) / (sizeof(unsigned long) * 8) + 1] = {0};
            if (::ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) >= 0) {
                if (test_bit_array(ABS_X, abs_bits) && test_bit_array(ABS_Y, abs_bits)) {
                    if (::ioctl(fd, EVIOCGABS(ABS_X), &abs_x_info) >= 0 &&
                        ::ioctl(fd, EVIOCGABS(ABS_Y), &abs_y_info) >= 0) {
                        if (abs_x_info.maximum > abs_x_info.minimum &&
                            abs_y_info.maximum > abs_y_info.minimum) {
                            has_abs = true;
                        }
                    }
                }
            }
        }

        if (!has_rel && !has_abs) {
            ::close(fd);
            continue;
        }

        // Drain initial queue of newly opened device
        drain_device_input(fd, 256);

        DeviceContext dev;
        dev.fd = fd;
        dev.path = std::move(full_path);
        dev.has_rel = has_rel;
        dev.has_abs = has_abs;
        dev.decoder.set_bounds(rows_, cols_);
        dev.decoder.set_rel_step(8, 16);
        if (has_abs) {
            dev.decoder.set_abs_x_info(abs_x_info.minimum, abs_x_info.maximum);
            dev.decoder.set_abs_y_info(abs_y_info.minimum, abs_y_info.maximum);
        }

        // Resync initial button state and ABS coordinates
        resync_device(dev);

        devices_.push_back(std::move(dev));
    }

    ::closedir(dir);
}

void ConsoleMouse::resync_device(DeviceContext& dev) {
    if (dev.fd < 0) return;

    unsigned long key_bits[(KEY_MAX + 1) / (sizeof(unsigned long) * 8) + 1] = {0};
    bool btn_left_down = false;
    bool btn_right_down = false;
    if (::ioctl(dev.fd, EVIOCGKEY(sizeof(key_bits)), key_bits) >= 0) {
        btn_left_down = test_bit_array(BTN_LEFT, key_bits);
        btn_right_down = test_bit_array(BTN_RIGHT, key_bits);
    }

    std::optional<int32_t> abs_x;
    std::optional<int32_t> abs_y;
    if (dev.has_abs) {
        struct input_absinfo ax{};
        struct input_absinfo ay{};
        if (::ioctl(dev.fd, EVIOCGABS(ABS_X), &ax) >= 0) abs_x = ax.value;
        if (::ioctl(dev.fd, EVIOCGABS(ABS_Y), &ay) >= 0) abs_y = ay.value;
    }

    dev.decoder.resync_state(btn_left_down, abs_x, abs_y, btn_right_down);
}

std::optional<MouseEvent> ConsoleMouse::poll_event() {
    if (!enabled_) {
        return std::nullopt;
    }

    // Check active VT and text mode before returning buffered events
    if (!check_active_vt_and_text_mode()) {
        close_all_devices();
        while (!output_queue_.empty()) {
            output_queue_.pop();
        }
        return std::nullopt;
    }

    if (!output_queue_.empty()) {
        MouseEvent ev = output_queue_.front();
        output_queue_.pop();
        return ev;
    }

    uint64_t now_ms = get_current_time_ms();
    if (now_ms - last_rescan_time_ms_ >= 1000) {
        last_rescan_time_ms_ = now_ms;
        rescan_devices();
    }

    if (devices_.empty()) {
        return std::nullopt;
    }

    struct pollfd pfds[kMaxDevices];
    nfds_t nfds = 0;
    for (const auto& dev : devices_) {
        if (dev.fd >= 0 && nfds < kMaxDevices) {
            pfds[nfds].fd = dev.fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            ++nfds;
        }
    }

    int poll_res = ::poll(pfds, nfds, 0);
    if (poll_res <= 0) {
        return std::nullopt;
    }

    for (size_t i = 0; i < devices_.size() && i < nfds; ++i) {
        auto& dev = devices_[i];
        const auto& pfd = pfds[i];

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            ::close(dev.fd);
            dev.fd = -1;
            continue;
        }

        if (pfd.revents & POLLIN) {
            struct input_event evs[64];
            ssize_t n = ::read(dev.fd, evs, sizeof(evs));
            if (n > 0) {
                dev.decoder.set_position(shared_row_, shared_col_);
                size_t count = static_cast<size_t>(n) / sizeof(struct input_event);
                for (size_t k = 0; k < count; ++k) {
                    dev.decoder.feed(evs[k]);
                    if (dev.decoder.needs_resync()) {
                        resync_device(dev);
                    }
                    if (evs[k].type == EV_SYN && evs[k].code == SYN_REPORT) {
                        while (dev.decoder.has_events()) {
                            if (output_queue_.size() >= kMaxOutputQueueSize) {
                                while (!output_queue_.empty()) output_queue_.pop();
                                output_queue_.push(MouseEvent{MouseKind::Cancel, shared_row_, shared_col_, 0});
                                dev.decoder.reset();
                                resync_device(dev);
                                break;
                            }
                            auto ev = *dev.decoder.pop_event();
                            if (ev.kind == MouseKind::Move) {
                                shared_row_ = ev.row;
                                shared_col_ = ev.col;
                            } else {
                                ev.row = shared_row_;
                                ev.col = shared_col_;
                            }
                            output_queue_.push(ev);
                        }
                    }
                }
            } else if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    ::close(dev.fd);
                    dev.fd = -1;
                }
            }
        }
    }

    devices_.erase(
        std::remove_if(devices_.begin(), devices_.end(), [](const DeviceContext& d) { return d.fd < 0; }),
        devices_.end());

    if (!output_queue_.empty()) {
        MouseEvent ev = output_queue_.front();
        output_queue_.pop();
        return ev;
    }

    return std::nullopt;
}

} // namespace litewin
