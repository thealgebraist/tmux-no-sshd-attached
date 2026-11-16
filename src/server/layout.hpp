#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tmux2::server {

enum class SplitType {
    Horizontal, // Splits left and right (tmux -h divides width)
    Vertical    // Splits top and bottom (tmux -v divides height)
};

enum class ResizeDirection {
    Up,
    Down,
    Left,
    Right
};

struct Rect {
    uint16_t x{0};
    uint16_t y{0};
    uint16_t width{0};
    uint16_t height{0};

    constexpr bool operator==(const Rect&) const = default;
};

class LayoutNode {
public:
    virtual ~LayoutNode() = default;

    [[nodiscard]] virtual bool is_leaf() const noexcept = 0;
    [[nodiscard]] virtual std::string to_json() const = 0;
    virtual void calculate(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t zoomed_pane_id) = 0;
    virtual void collect_pane_ids(std::vector<uint32_t>& out) const = 0;
    virtual std::optional<Rect> find_pane_rect(uint32_t pane_id) const = 0;
};

class LayoutLeaf : public LayoutNode {
public:
    explicit LayoutLeaf(uint32_t pane_id) : pane_id_(pane_id) {}

    [[nodiscard]] bool is_leaf() const noexcept override { return true; }
    [[nodiscard]] uint32_t pane_id() const noexcept { return pane_id_; }
    [[nodiscard]] const Rect& rect() const noexcept { return rect_; }
    void set_rect(const Rect& r) noexcept { rect_ = r; }

    [[nodiscard]] std::string to_json() const override;
    void calculate(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t zoomed_pane_id) override;
    void collect_pane_ids(std::vector<uint32_t>& out) const override;
    std::optional<Rect> find_pane_rect(uint32_t pane_id) const override;

private:
    uint32_t pane_id_{0};
    Rect rect_{};
};

class LayoutSplit : public LayoutNode {
public:
    LayoutSplit(SplitType type, std::unique_ptr<LayoutNode> first, std::unique_ptr<LayoutNode> second, double ratio = 0.5)
        : type_(type), first_(std::move(first)), second_(std::move(second)), ratio_(ratio) {}

    [[nodiscard]] bool is_leaf() const noexcept override { return false; }
    [[nodiscard]] SplitType type() const noexcept { return type_; }
    [[nodiscard]] double ratio() const noexcept { return ratio_; }
    void set_ratio(double r) noexcept { ratio_ = r; }

    LayoutNode* first() noexcept { return first_.get(); }
    LayoutNode* second() noexcept { return second_.get(); }
    const LayoutNode* first() const noexcept { return first_.get(); }
    const LayoutNode* second() const noexcept { return second_.get(); }
    std::unique_ptr<LayoutNode>& first_ptr() noexcept { return first_; }
    std::unique_ptr<LayoutNode>& second_ptr() noexcept { return second_; }

    [[nodiscard]] std::string to_json() const override;
    void calculate(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t zoomed_pane_id) override;
    void collect_pane_ids(std::vector<uint32_t>& out) const override;
    std::optional<Rect> find_pane_rect(uint32_t pane_id) const override;

private:
    SplitType type_;
    std::unique_ptr<LayoutNode> first_;
    std::unique_ptr<LayoutNode> second_;
    double ratio_{0.5};
};

class LayoutTree {
public:
    LayoutTree() = default;
    explicit LayoutTree(uint32_t initial_pane_id);

    void reset(uint32_t pane_id);
    [[nodiscard]] bool empty() const noexcept { return root_ == nullptr; }
    [[nodiscard]] LayoutNode* root() noexcept { return root_.get(); }
    [[nodiscard]] const LayoutNode* root() const noexcept { return root_.get(); }

    void calculate(uint16_t cols, uint16_t rows, uint32_t zoomed_pane_id = 0);
    [[nodiscard]] std::optional<Rect> get_pane_rect(uint32_t pane_id) const;
    [[nodiscard]] std::vector<uint32_t> all_pane_ids() const;
    [[nodiscard]] std::string to_json() const;

    bool split_pane(uint32_t target_pane_id, uint32_t new_pane_id, SplitType type, double ratio = 0.5);
    bool remove_pane(uint32_t pane_id);
    bool resize_pane(uint32_t pane_id, ResizeDirection dir, int amount, uint16_t total_cols, uint16_t total_rows);

private:
    std::unique_ptr<LayoutNode> root_;
};

} // namespace tmux2::server
