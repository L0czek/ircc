#ifndef UI_MENU_HPP
#define UI_MENU_HPP

#include "ui/ui.hpp"
#include <cstddef>
#include <span>

namespace ui {

enum class ItemKind : uint8_t {
    Submenu,     // activate() opens something
    Action,      // activate() on click
    HoldAction,  // activate() after holding select for kHoldMs (starting power stages)
    Number,      // edited with the encoder / joystick
    Toggle,      // click flips 0/1
    Choice,      // edited, cycles through spec.choices
    Info,        // read-only text on the right (info_text)
};

struct ValueSpec {
    float min = 0.0f;
    float max = 0.0f;
    float step = 1.0f;
    uint8_t decimals = 0;
    const char* unit = "";
    std::span<const char* const> choices{};
    bool live = false;     // applied while editing (rate limited)
};

struct MenuItem {
    ItemKind kind = ItemKind::Info;
    const char* label = "";
    ValueSpec spec{};
    bool enabled = true;
};

/** @brief Content of a MenuScreen (static objects, items built on demand) */
class MenuModel {
public:
    virtual const char* title(Ui& ui) noexcept = 0;
    virtual size_t count(Ui& ui) noexcept = 0;
    virtual MenuItem item(Ui& ui, size_t index) noexcept = 0;
    /** @brief Current value of a Number / Toggle / Choice item */
    virtual float value(Ui& ui, size_t index) noexcept { (void)ui; (void)index; return 0.0f; }
    /** @brief Apply a value; false on failure (show a toast) */
    virtual bool set(Ui& ui, size_t index, float value) noexcept { (void)ui; (void)index; (void)value; return false; }
    virtual void activate(Ui& ui, size_t index) noexcept { (void)ui; (void)index; }
    /** @brief Right-hand text of an Info / Submenu item ("" for none) */
    virtual void info_text(Ui& ui, size_t index, char* buf, size_t n) noexcept {
        (void)ui; (void)index;
        if (n) buf[0] = '\0';
    }
    virtual void on_enter(Ui& ui) noexcept { (void)ui; }

protected:
    ~MenuModel() = default;
};

/**
 * @brief List of items with selection, scrolling and in-place value editing.
 *
 * Navigation: encoder / up / down move, centre or push activates, left goes back.
 * Editing a number: encoder changes by the cursor step (x10 / x100 when spun fast),
 * up/down +-1 cursor step, left/right move the cursor (coarser / finer), click commits,
 * long press cancels (restores the old value).
 */
class MenuScreen final : public Screen {
public:
    static constexpr uint32_t kHoldMs = 1000;
    static constexpr uint32_t kLiveApplyMs = 100;
    static constexpr int kRowHeight = 11;

    explicit MenuScreen(MenuModel& model) noexcept : model_(model) {}

    bool on_event(Ui& ui, const Event& e) noexcept override;
    void on_enter(Ui& ui) noexcept override;
    void tick(Ui& ui) noexcept override;
    void draw(Ui& ui, Canvas& c, const Rect& body) noexcept override;
    const char* hints() const noexcept override;

    void select(size_t index) noexcept { selected_ = index; }
    size_t selected() const noexcept { return selected_; }

private:
    void move(Ui& ui, int delta) noexcept;
    void begin_edit(Ui& ui) noexcept;
    void end_edit(Ui& ui, bool commit) noexcept;
    void edit_by(Ui& ui, float steps) noexcept;
    bool on_edit_event(Ui& ui, const Event& e) noexcept;
    void format_value(Ui& ui, size_t index, const MenuItem& item, float value, char* buf, size_t n) noexcept;

    MenuModel& model_;
    size_t selected_ = 0;
    size_t scroll_ = 0;

    bool editing_ = false;
    float edit_value_ = 0.0f;
    float original_ = 0.0f;
    float cursor_ = 1.0f;          // step of one encoder detent / up-down press
    bool live_pending_ = false;
    uint32_t live_applied_at_ = 0;
    float model_at_edit_ = 0.0f;   // to notice a change made by the host while editing

    bool holding_ = false;         // hold-to-start in progress
    bool hold_fired_ = false;      // swallow events until the key is released
    uint32_t hold_since_ = 0;
};

} // namespace ui

#endif
