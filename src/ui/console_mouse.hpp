#pragma once

#include <linux/input.h>
#include <cstdint>
#include <optional>
#include <queue>
#include <string>
#include <vector>
#include <utility>

namespace litewin {

enum class MouseKind {
    Move,
    LeftPress,
    LeftRelease,
    Wheel,
    RightPress,
    RightRelease,
    Cancel
};

struct MouseEvent {
    MouseKind kind = MouseKind::Move;
    int row = 1;
    int col = 1;
    int wheel = 0;
};

struct AbsAxisInfo {
    int32_t minimum = 0;
    int32_t maximum = 1;
};

// Pure decoder for evdev input streams. Decodes Linux input_event packets into MouseEvents.
// Exposed separately for synthetic unit tests without requiring real devices or root access.
class MouseDecoder {
public:
    explicit MouseDecoder(int rows = 24, int cols = 80);

    void set_bounds(int rows, int cols);
    void set_position(int row, int col);
    void set_abs_x_info(int32_t minimum, int32_t maximum);
    void set_abs_y_info(int32_t minimum, int32_t maximum);
    void set_rel_step(int step_x, int step_y);

    void feed(const struct input_event& ev);
    bool needs_resync() const { return needs_resync_; }
    void clear_needs_resync() { needs_resync_ = false; }

    void resync_state(bool is_btn_left_down,
                      std::optional<int32_t> abs_x = std::nullopt,
                      std::optional<int32_t> abs_y = std::nullopt,
                      bool is_btn_right_down = false);
    void reset(bool left = false, bool right = false);

    std::optional<MouseEvent> pop_event();
    bool has_events() const;

    int current_row() const { return current_row_; }
    int current_col() const { return current_col_; }
    bool is_button_down() const { return button_down_; }
    bool is_right_button_down() const { return right_button_down_; }

private:
    void commit_packet();
    void clamp_cursor();

    int rows_ = 24;
    int cols_ = 80;

    int current_row_ = 1;
    int current_col_ = 1;

    AbsAxisInfo abs_x_{0, 1};
    AbsAxisInfo abs_y_{0, 1};
    bool has_abs_x_ = false;
    bool has_abs_y_ = false;

    int rel_step_x_ = 1;
    int rel_step_y_ = 1;
    int64_t rel_accum_x_ = 0;
    int64_t rel_accum_y_ = 0;

    bool dropping_ = false;
    bool needs_resync_ = false;
    bool suppress_clicks_until_release_ = false;
    bool suppress_right_clicks_until_release_ = false;

    int64_t staged_rel_x_ = 0;
    int64_t staged_rel_y_ = 0;
    int staged_wheel_ = 0;
    std::optional<int32_t> staged_abs_x_;
    std::optional<int32_t> staged_abs_y_;
    std::optional<bool> staged_btn_left_;
    std::optional<bool> staged_btn_right_;

    std::vector<std::pair<int, bool>> staged_button_edges_;
    bool button_down_ = false;
    bool right_button_down_ = false;
    std::queue<MouseEvent> event_queue_;
};

class ConsoleMouse {
public:
    ConsoleMouse();
    ~ConsoleMouse();

    ConsoleMouse(const ConsoleMouse&) = delete;
    ConsoleMouse& operator=(const ConsoleMouse&) = delete;
    ConsoleMouse(ConsoleMouse&& other) noexcept;
    ConsoleMouse& operator=(ConsoleMouse&& other) noexcept;

    void set_bounds(int rows, int cols);
    void set_enabled(bool enabled);
    bool is_enabled() const { return enabled_; }
    void reset();

    std::optional<MouseEvent> poll_event();

private:
    struct DeviceContext {
        int fd = -1;
        std::string path;
        MouseDecoder decoder;
        bool has_rel = false;
        bool has_abs = false;
    };

    bool check_active_vt_and_text_mode() const;
    void rescan_devices();
    void close_all_devices();
    void drain_device_input(int fd, int max_events = 256);
    void resync_device(DeviceContext& dev);

    int rows_ = 24;
    int cols_ = 80;
    int shared_row_ = 1;
    int shared_col_ = 1;
    bool enabled_ = false;

    std::vector<DeviceContext> devices_;
    std::queue<MouseEvent> output_queue_;
    uint64_t last_rescan_time_ms_ = 0;
};

} // namespace litewin
