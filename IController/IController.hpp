#pragma once

#include <array>
#include <cassert>
#include <cstddef>

namespace core::controller
{

enum class State
{
    Disabled,
    Protected,
    Running
};

class ControllerNode
{
public:
    virtual bool enable() = 0;

    virtual void disable() = 0;

    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] bool  enabled() const { return state_ != State::Disabled; }

    [[nodiscard]] ControllerNode* currentController() const { return parent_; }

protected:
    /**
     * 进入 protect 状态
     */
    virtual void protect() = 0;

    /**
     * 节点的自身使能，该函数定义 *自身* 使能过程中的动作，请勿在本函数传递使能关系
     * @return 是否使能成功
     */
    virtual bool selfEnable() { return true; }

    /**
     * 节点自身失能，该函数定义 *自身* 失能过程中的动作，请勿在本函数传递失能关系
     */
    virtual void selfDisable() {}

private:
    ControllerNode() = default;
    virtual ~ControllerNode() { assert(state_ == State::Disabled); }
    ControllerNode(const ControllerNode&)            = delete;
    ControllerNode& operator=(const ControllerNode&) = delete;
    ControllerNode(ControllerNode&&)                 = delete;
    ControllerNode& operator=(ControllerNode&&)      = delete;

    // 状态和关系必须由 IController 处理
    State state_{ State::Disabled };

    ControllerNode* parent_{ nullptr };

    void enterProtectedState()
    {
        protect();
        state_ = State::Protected;
    }

    virtual bool acquireController(ControllerNode* controller)
    {
        if (controller == nullptr || !enabled())
            return false;
        if (parent_ == controller)
            // 可以被重复获取控制权
            return true;
        if (parent_ != nullptr)
            // 已经被其他控制器控制
            return false;
        parent_ = controller;

        enterProtectedState();
        return true;
    }

    virtual void releaseController(ControllerNode* controller)
    {
        if (parent_ != controller)
            // 本身没有控制权，当然是释放了
            return;
        parent_ = nullptr;
        enterProtectedState();
    }

    virtual std::size_t childCount() const { return 0; }

    virtual ControllerNode* childAt(std::size_t) const { return nullptr; }

    template <std::size_t> friend class IController;
};

template <std::size_t N = 0> class IController : public ControllerNode
{
public:
    IController()
    {
        static_assert(N == 0, "default constructor is only valid for leaf controllers");
    }

    explicit IController(const std::array<ControllerNode*, N>& children) : children_(children)
    {
        static_assert(N > 0, "child constructor requires at least one child");
        for (std::size_t i = 0; i < N; ++i)
        {
            assert(children_[i] != nullptr && children_[i] != this);
            for (std::size_t j = 0; j < i; ++j)
                assert(children_[i] != children_[j]);
        }
    }

    /**
     * 为保证 controller 类的使能及其传递关系不被破坏，该函数不支持覆写
     * @return 是否使能成功
     */
    virtual bool enable() final
    {
        // 非 Disabled 即已经使能，本函数允许重复使能
        if (this->state_ != State::Disabled)
            return true;

        std::array<bool, N> child_enabled_map{};

        /**
         * 如果失败，则必然需要释放前面已经成功获取到控制权的节点
         * 对于之前处于失能状态下的节点，还需要重新将其失能
         *
         * 这里我们有一处假定：由于使能是向下传递的，不可能存在获取一半控制权的情况，故可以直接全部释放控制权
         */
        auto rollback = [&](const int i)
        {
            for (int j = i - 1; j >= 0; --j)
            {
                auto* previous = children_[j];
                previous->releaseController(this);
                if (!child_enabled_map[j])
                    // 如果之前节点未使能，重新失能它
                    previous->disable();
            }
        };

        for (std::size_t i = 0; i < N; ++i)
        {
            auto* child = children_[i];

            child_enabled_map[i] = child->enabled();
            // 尝试使能并获取控制权
            if (!child->enable() || !child->acquireController(this))
            {
                // AI 报告问题：如果使能成功，但是 acquire 失败，并未回滚自身。我认为该问题不存在
                // 如果原本是使能态，acquire 失败就不需要回滚自身
                // 如果原本不是使能态，使能后 parent_ 必然是 nullptr，acquire 必然成功
                rollback(i);
                return false;
            }
        }

        if (!selfEnable())
        {
            selfDisable();
            // 如果自身节点使能失败，则回滚全部
            rollback(N);
            return false;
        }

        enterProtectedState();
        return true;
    }

    virtual void disable() final
    {
        // 改变自身状态为失能态
        selfDisable();
        state_ = State::Disabled;

        // 向下释放控制权，child 会默认进入 protected
        for (auto& child : children_)
            child->releaseController(this);

        // 向上传递失能
        if (parent_ != nullptr)
            parent_->disable();
    }

protected:
    std::size_t     childCount() const final { return N; }
    ControllerNode* childAt(std::size_t i) const final { return children_[i]; }

private:
    std::array<ControllerNode*, N> children_{};
};

} // namespace core::controller
