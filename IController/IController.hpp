#pragma once

#include <array>
#include <cassert>
#include <cstddef>

namespace core::controller
{

class ControllerNode
{
public:
    virtual ~ControllerNode() { assert(state_ == State::Disabled); }
    ControllerNode(const ControllerNode&)            = delete;
    ControllerNode& operator=(const ControllerNode&) = delete;
    ControllerNode(ControllerNode&&)                 = delete;
    ControllerNode& operator=(ControllerNode&&)      = delete;

    enum class State
    {
        /// 自身未使能
        Disabled,
        /// 已使能但不接受直接控制命令
        Protected,
        /// 独立运行态
        Standalone,
        /// 受控运行态
        Controlled,
    };

    // ControllerNode 只定义节点接口；状态和父子控制关系由 IController 维护。
    virtual bool enable() = 0;

    virtual void disable() = 0;

    virtual bool standalone() final
    {
        if (state_ == State::Standalone)
            return true;
        if (state_ == State::Protected)
        {
            state_ = State::Standalone;
            return true;
        }
        return false;
    }

    virtual bool protect() final
    {
        if (state_ == State::Protected)
            return true;
        // 失能状态下需要先使能；受控态不能自己切换状态
        if (state_ == State::Disabled || state_ == State::Controlled)
            return false;
        selfProtect();
        state_ = State::Protected;
        return true;
    }

    /**
     * @return 当前节点状态
     */
    [[nodiscard]] State state() const { return state_; }

    /** @return 节点是否处于非 Disabled 状态。 */
    [[nodiscard]] bool enabled() const { return state_ != State::Disabled; }

    [[nodiscard]] bool isStandalone() const { return state_ == State::Standalone; }

    [[nodiscard]] bool isProtected() const { return state_ == State::Protected; }

    [[nodiscard]] bool isControlled() const { return state_ == State::Controlled; }

    /** @return 当前持有本节点控制权的父控制器；没有控制器时返回 nullptr。 */
    [[nodiscard]] ControllerNode* currentController() const { return parent_; }

protected:
    /**
     * 使节点进入硬件保护状态。
     *
     * 该函数只执行节点自身的保护动作不负责修改 state_，也不负责传递控制关系。
     */
    virtual void selfProtect() = 0;

    /**
     * 节点的自身使能动作。
     *
     * 该函数只处理本节点硬件或本节点内部资源，禁止在这里调用 child 的
     * enable() 或 acquireController()；
     * 父子使能关系统一由 IController::enable()
     * 传递。返回 false 时，节点自身不得留下未清理的部分使能状态。
     *
     * @return 是否使能成功
     */
    virtual bool selfEnable() { return true; }

    /**
     * 节点自身失能动作。
     *
     * 该函数只处理本节点硬件或本节点内部资源，禁止在这里传递父子失能关系。
     */
    virtual void selfDisable() {}

    // 状态和关系必须由 IController 处理；上层只能继承 IController。
private:
    ControllerNode() = default;

    // 状态和关系必须由 IController 处理
    State state_{ State::Disabled };

    ControllerNode* parent_{ nullptr };

    /**
     * 获取本节点控制权，仅供 IController::enable() 调用。
     *
     * 同一个 controller 可以重复获取；已被其他 controller 持有时失败。
     * 节点必须先处于 enabled 状态才能被获取控制权。
     */
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

        // 进入受控运行态
        selfProtect();
        state_ = State::Controlled;
        return true;
    }

    /**
     * 释放本节点控制权，仅释放指定 controller 持有的控制权。
     *
     * 释放后节点仍保持 enabled，并重新执行保护动作；真正的失能由 disable()
     * 负责。
     */
    virtual void releaseController(ControllerNode* controller)
    {
        if (state_ != State::Controlled)
            return;
        if (parent_ != controller)
            // 本身没有控制权，当然是释放了
            return;
        parent_ = nullptr;

        // 保护自身，并进入保护态
        selfProtect();
        state_ = State::Protected;
    }

    virtual void rollbackEnable() = 0;

    [[nodiscard]] virtual std::size_t childCount() const { return 0; }

    [[nodiscard]] virtual ControllerNode* childAt(std::size_t) const { return nullptr; }

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
    bool enable() final
    {
        // 非 Disabled 即已经使能，本函数允许重复使能
        if (enabled())
        {
            old_enabled_ = true;
            return true;
        }
        old_enabled_ = false;

        /**
         * 如果失败，则必然需要释放前面已经成功获取到控制权的节点
         * 对于之前处于失能状态下的节点，还需要重新将其失能
         *
         * 这里我们有一处假定：由于使能是向下传递的，不可能存在获取一半控制权的情况，故可以直接全部释放控制权
         */
        auto rollback = [&](const int i)
        {
            for (int j = i; j >= 0; --j)
            {
                auto* previous = children_[j];
                previous->releaseController(this);
                previous->rollbackEnable();
            }
        };

        for (std::size_t i = 0; i < N; ++i)
        {
            auto* child = children_[i];

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

        // 使能之后立即进入保护状态
        state_ = State::Protected;
        selfProtect();
        return true;
    }

    void disable() final
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

        parent_ = nullptr;
    }

protected:
    [[nodiscard]] std::size_t     childCount() const final { return N; }
    [[nodiscard]] ControllerNode* childAt(std::size_t i) const final { return children_[i]; }

private:
    std::array<ControllerNode*, N> children_{};

    bool old_enabled_{ false };

    void rollbackEnable() final
    {
        if (!old_enabled_)
        {
            for (auto& child : children_)
                child->rollbackEnable();

            selfDisable();
            state_ = State::Disabled;
        }
    }
};

} // namespace core::controller
