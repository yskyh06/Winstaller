#pragma once

#include "ui/console_mouse.hpp"
#include "ui/tui_layout.hpp"
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace litewin {

enum class HitType { None, Hover, Click, Wheel, Context };

struct HitResult {
    HitType type = HitType::None;
    int control_id = -1;
    int data = 0;
    int wheel_delta = 0;
    [[nodiscard]] bool is_click() const noexcept { return type == HitType::Click; }
    [[nodiscard]] bool is_hover() const noexcept { return type == HitType::Hover; }
    [[nodiscard]] bool is_wheel() const noexcept { return type == HitType::Wheel; }
    [[nodiscard]] bool is_context() const noexcept { return type == HitType::Context; }
};

struct RegisteredHitArea {
    int id = 0;
    Rect bounds;
    int data = 0;
    std::string details_key;
};

class MouseControls {
public:
    void clear() { areas_.clear(); }
    void register_area(int id, const Rect& bounds, int data = 0, std::string details_key = {}) {
        if (bounds.empty()) return;
        if (details_key.empty()) details_key = context_ + ".control." + std::to_string(id);
        areas_.push_back({id, bounds, data, std::move(details_key)});
    }
    void register_area(int id, int row, int col, int width, int height, int data = 0, std::string details_key = {}) {
        register_area(id, Rect{row, col, width, height}, data, std::move(details_key));
    }
    [[nodiscard]] const std::vector<RegisteredHitArea>& areas() const noexcept { return areas_; }
    [[nodiscard]] const RegisteredHitArea* find_area_at(int row, int col) const {
        for (const auto& a : areas_) if (a.bounds.contains(row, col)) return &a;
        return nullptr;
    }
    [[nodiscard]] const RegisteredHitArea* find_area(int id) const {
        for (const auto& a : areas_) if (a.id == id) return &a;
        return nullptr;
    }
    [[nodiscard]] int find_control_at(int row, int col) const {
        const auto* a = find_area_at(row, col);
        return a ? a->id : -1;
    }
    [[nodiscard]] std::optional<Rect> hover_area() const { return hover_; }
    [[nodiscard]] int last_hover_id() const noexcept { return hover_id_; }
    [[nodiscard]] int armed_id() const noexcept { return left_.id; }
    [[nodiscard]] int armed_data() const noexcept { return left_.data; }
    [[nodiscard]] unsigned generation() const noexcept { return generation_; }

    void reset() {
        left_ = {}; right_ = {};
        left_down_ = right_down_ = suppress_ = false;
        hover_.reset(); hover_id_ = -1;
    }
    void advance_generation() { ++generation_; reset(); }
    void on_resize() { advance_generation(); clear(); }
    void suppress_until_release() { suppress_ = true; left_ = {}; right_ = {}; }

    HitResult process_event(const MouseEvent& ev) {
        if (ev.kind == MouseKind::Cancel) { reset(); return {}; }
        const auto* area = find_area_at(ev.row, ev.col);
        if (ev.kind == MouseKind::Move) {
            hover_id_ = area ? area->id : -1;
            hover_ = area ? std::optional<Rect>{area->bounds} : std::nullopt;
            cancel_if_left(left_, area); cancel_if_left(right_, area);
            return area ? HitResult{HitType::Hover, area->id, area->data, 0} : HitResult{};
        }
        if (ev.kind == MouseKind::Wheel) {
            left_ = {}; right_ = {};
            return {HitType::Wheel, area ? area->id : -1, area ? area->data : 0, ev.wheel};
        }
        const bool right = ev.kind == MouseKind::RightPress || ev.kind == MouseKind::RightRelease;
        const bool press = ev.kind == MouseKind::LeftPress || ev.kind == MouseKind::RightPress;
        const bool release = ev.kind == MouseKind::LeftRelease || ev.kind == MouseKind::RightRelease;
        if (!press && !release) return {};
        bool& down = right ? right_down_ : left_down_;
        Arm& arm = right ? right_ : left_;
        if (press) {
            if (down) return {};
            down = true;
            if (left_down_ && right_down_) suppress_until_release();
            if (!suppress_ && area) arm = {area->id, area->data, generation_, area->details_key};
            return {};
        }
        const bool was_down = down;
        down = false;
        HitResult result;
        if (!suppress_ && was_down && same(arm, area)) {
            result = {right ? HitType::Context : HitType::Click, area->id, area->data, 0};
        }
        arm = {};
        if (!left_down_ && !right_down_) suppress_ = false;
        return result;
    }

    class SceneGuard {
    public:
        explicit SceneGuard(MouseControls& mc, ConsoleMouse* mouse = nullptr, std::string context = {})
            : mc_(mc), mouse_(mouse), previous_context_(mc.context_) {
            mc_.advance_generation(); mc_.clear();
            if (!context.empty()) mc_.context_ = std::move(context);
            if (mouse_) mouse_->reset();
        }
        ~SceneGuard() {
            mc_.advance_generation(); mc_.clear();
            mc_.context_ = std::move(previous_context_);
            if (mouse_) mouse_->reset();
        }
        SceneGuard(const SceneGuard&) = delete;
        SceneGuard& operator=(const SceneGuard&) = delete;
    private:
        MouseControls& mc_;
        ConsoleMouse* mouse_;
        std::string previous_context_;
    };
private:
    struct Arm { int id = -1; int data = 0; unsigned generation = 0; std::string key; };
    bool same(const Arm& arm, const RegisteredHitArea* area) const {
        return area && arm.id == area->id && arm.data == area->data && arm.generation == generation_ && arm.key == area->details_key;
    }
    void cancel_if_left(Arm& arm, const RegisteredHitArea* area) {
        if (!same(arm, area)) arm = {};
    }
    std::vector<RegisteredHitArea> areas_;
    Arm left_, right_;
    bool left_down_ = false, right_down_ = false, suppress_ = false;
    int hover_id_ = -1;
    std::optional<Rect> hover_;
    unsigned generation_ = 0;
    std::string context_ = "screen";
};

} // namespace litewin
