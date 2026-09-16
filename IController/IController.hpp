/**
 * @file    IController.hpp
 * @author  syhanjin
 * @date    2026-09-15
 */
#pragma once
#include <array>
#include <atomic>
#include <cassert>

namespace core::control
{

/**
 * 控制链的生命周期节点基类。
 *
 * 节点状态与不变式：
 * - Disabled   未使能，也不持有任何控制关系；
 * - Protected  已使能并处于保护态，不接受直接控制命令；
 * - Standalone 已使能、无控制者的直接控制态；
 * - Controlled 控制权已被 parent_ 指向的控制器取得。
 *
 * parent_ 非空当且仅当本节点为 Controlled；失能的节点不再持有子节点。
 *
 * 并发模型（单核，任务与 ISR 之间可相互抢占）：
 * 每个节点持有一把三态节点锁 Idle / Enabling / Cancelled，它同时承担“本轮使能是否持有
 * 本节点”和“本节点是否已被失能取消”两个含义：
 * - enable() 先按“自身优先”的顺序锁住整个候选子树（lockTree），再自上而下推进，收尾时
 *   统一释放并检查本轮是否被取消（unlockTree）；只有整棵树都没被取消，本轮才算成功。
 * - disable() 优先级更高：它先取消本节点（以及控制链上各节点）在途的使能标记，再执行
 *   失能。被取消的在途使能不会提交成功，其已提交的部分由该事务自身的回滚撤销，而失能
 *   产生的状态保持有效：被失能的节点保持 Disabled，借用的既有使能节点只失去本轮获得的
 *   控制权。
 * - 失能只对“尚未取得节点锁”的使能让步：节点锁取得之前的失能按先后顺序先于本轮使能，
 *   节点锁取得之后的失能则取消本轮使能。
 *
 * 抢占点保证的是最终状态一致，不是 selfEnable() 内部没有重新打开硬件的窗口：取消检查
 * 位于每个节点的推进、自身使能之后与状态提交之前，取消发生后本节点不再提交状态，其自身
 * 硬件由该检查或外层回滚关闭。
 *
 * 使能是可回滚事务：失败的使能撤销本轮建立的状态与控制关系。enable() 返回 false 表示
 * 本轮失败或存在竞争（例如候选子树正被其他操作锁定），调用者可以稍后重试。
 */
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
        Disabled,
        Protected,
        Standalone,
        Controlled
    };

    /**
     * 使能当前节点及其候选子树。
     *
     * 已使能时幂等返回 true，不遍历子树。返回 false 表示本轮失败（自身或后代使能失败、
     * 控制权冲突、候选节点处于 Standalone）或存在竞争，本轮的改动已被回滚。
     */
    virtual bool enable() noexcept = 0;

    /**
     * 失能当前节点。
     *
     * reverse=false：失能本节点并释放其直接持有的子节点（它们保持使能并成为保护态根），
     * 随后沿控制链向上失能为其服务的各级控制者。reverse=true：要求本节点无控制者，取消本
     * 节点及其活动子树各节点在途的使能，再自顶向下关闭整棵活动子树。两种方向都会取消本节点
     * 在途的使能，因此返回 true 只表示本次失能请求已执行，不表示在途事务已经退出。
     */
    virtual bool disable(bool reverse) noexcept = 0;

    /**
     * 无控制者的 Protected -> Standalone；已 Standalone 时幂等。
     * 只改变逻辑权限，不使能硬件、不提交业务命令。
     */
    virtual bool standalone() noexcept = 0;

    /**
     * 仅 Standalone -> Protected；先提交自身保护行为，再改变状态。
     */
    virtual bool protect() noexcept = 0;

    [[nodiscard]] State state() const noexcept { return state_; }

    bool isEnabled() const noexcept { return state_ != State::Disabled; }
    bool isStandalone() const noexcept { return state_ == State::Standalone; }
    bool isProtected() const noexcept { return state_ == State::Protected; }
    bool isControlled() const noexcept { return state_ == State::Controlled; }

    ControllerNode* currentController() const noexcept { return parent_; }

protected:
    // 所有 self* 钩子都在生命周期事务的推进路径上运行：必须短时、非阻塞、ISR 可用、
    // 不抛异常，且不得重入任何生命周期操作或传播控制关系。
    // selfEnable() 返回 false 时必须自行清理本次已经完成的使能动作：框架只对成功的使能
    // 调用 selfDisable()。
    // selfDisable() 可能被调用在节点并未观察到使能的情况下（失能允许打断在途的使能），
    // 因此它必须可重复调用，并在节点本就未使能时保持安全。

    virtual bool selfEnable() { return true; }
    virtual void selfDisable() {}
    virtual void selfProtect() {}

private:
    ControllerNode() = default;

    /**
     * 使能本节点并让 controller 取得其控制权，由 enable() 与父节点驱动。
     *
     * 已经由同一 controller 持有的节点幂等成功（同一轮内的共享候选节点）；已使能但不是
     * Standalone 的节点只被借用一次，不重建其子树；本轮内新使能的节点先推进候选子节点，
     * 再执行自身使能、保护与状态提交。任一时刻发现本节点已被失能取消，立即失败并由调用者
     * 回滚。
     */
    virtual bool enableAndAcquireController(ControllerNode* controller) noexcept = 0;

    /**
     * 回滚本轮使能对本节点状态与控制权的改动。
     *
     * 本轮新使能的节点关闭自身并向下回滚；本轮借用的既有使能节点只释放本轮获得的控制权，
     * 回到 Protected 并保留其原有子树；已被失能取消的节点保持 Disabled。
     */
    virtual void rollbackLastEnablement() noexcept = 0;

    /// 以本节点优先的顺序锁住候选子树；失败时释放已锁住的部分。
    virtual bool lockTree() noexcept = 0;

    /// 释放本节点及其候选子树的锁；返回本轮的所有节点是否都没有被取消。
    virtual bool unlockTree() noexcept = 0;

    /// 关闭本节点及其活动子树（活动子节点全部失能，控制关系解除）。
    virtual void idleTree() noexcept = 0;

    /// 释放 controller 通过 parent_ 对本节点持有的控制边。
    virtual void releaseController(ControllerNode* controller) noexcept = 0;

private:
    template <std::size_t> friend class IController;

    State state_{ State::Disabled };

    ControllerNode* parent_{ nullptr };
};

template <std::size_t N = 0> class IController : public ControllerNode
{
public:
    /**
     * 构造叶控制器。
     *
     * 只有 IController<0> 允许使用该构造函数；非叶控制器必须通过子节点数组构造。
     * 子节点由外部拥有，IController 不负责其生命周期。
     */
    IController()
    {
        static_assert(N == 0, "default constructor is only valid for leaf controllers");
    }

    /**
     * 构造带有 N 个候选子节点的控制器。
     *
     * 数组只描述可能的控制链，不会在构造时建立 parent_。实际控制边在 enable() 成功
     * 传播并取得控制权后建立。调用者必须保证引用非空、无自身引用、无直接重复项，并保证
     * 整个候选图无环。
     */
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

    bool enable() noexcept final
    {
        if (isEnabled())
            return true;

        if (!lockTree())
            return false;

        const bool enabled     = enableAndAcquireController(nullptr);
        const bool uncancelled = unlockTree();

        // 本轮使能成功地提交了状态，但中途有节点被失能：整轮回滚
        if (enabled && !uncancelled)
            rollbackLastEnablement();

        return enabled && uncancelled;
    }

    bool disableTree() noexcept { return disable(true); }

    bool disable(const bool reverse = false) noexcept final
    {
        // 向下失能只能由无控制者的节点发起
        if (reverse && isControlled())
            return false;

        // 取消本节点在途的使能；本节点空闲时由本次失能临时持有，直到状态提交完成
        const bool held = enable_state_.cancel();

        ControllerNode* const up = parent_;
        parent_                  = nullptr;
        selfDisable();
        state_ = State::Disabled;

        if (reverse)
        {
            for (std::size_t i = 0; i < N; ++i)
                if (children_[i]->parent_ == this)
                    children_[i]->idleTree();
        }
        else
        {
            for (std::size_t i = 0; i < N; ++i)
                children_[i]->releaseController(this);
        }

        if (held)
            enable_state_.idle();

        // 控制链上的节点只是为了控制本节点才被使能，随本节点一同失能
        if (up != nullptr)
            up->disable(false);

        return true;
    }

    bool standalone() noexcept final
    {
        if (!isProtected())
            return false;
        state_ = State::Standalone;
        return true;
    }

    bool protect() noexcept final
    {
        if (!isStandalone())
            return false;
        state_ = State::Protected;
        selfProtect();
        return true;
    }

private:
    bool acquireControllerWithoutCheckingState(ControllerNode* controller) noexcept
    {
        if (parent_ == nullptr)
        {
            parent_ = controller;
            return true;
        }
        return parent_ == controller;
    }

    bool enableAndAcquireController(ControllerNode* controller) noexcept final
    {
        // 本节点在途的使能已被失能取消：立即失败，不再推进
        if (enable_state_.state_.load() != EnableState::State::Enabling)
            return false;

        // 同一轮内再次到达（共享候选节点）：控制关系已经建立，幂等成功
        if (state_ == State::Controlled && parent_ == controller)
            return true;

        if (isEnabled())
        {
            // Standalone 不接受被间接接管
            if (state_ == State::Standalone)
                return false;

            // 已经使能的节点：本轮只借用，不重建其子树
            enabled_before_last_ = true;
            if (!acquireControllerWithoutCheckingState(controller))
                return false;

            // 收权过程中被失能：失能优先，放弃本次借用
            if (enable_state_.state_.load() != EnableState::State::Enabling)
            {
                parent_ = nullptr;
                state_  = State::Disabled;
                return false;
            }

            state_ = State::Controlled;
            return true;
        }

        enabled_before_last_ = false;

        // 先向下传递使能关系
        for (std::size_t i = 0; i < N; ++i)
        {
            if (enable_state_.state_.load() != EnableState::State::Enabling ||
                !children_[i]->enableAndAcquireController(this))
            {
                // 回滚本轮已经使能成功的子节点
                for (std::size_t j = i; j-- > 0;)
                    children_[j]->rollbackLastEnablement();
                return false;
            }
        }

        if (!selfEnable())
        {
            for (std::size_t j = N; j-- > 0;)
                children_[j]->rollbackLastEnablement();
            return false;
        }

        // 自身使能之后才被失能：收回本次自身使能，保持 Disabled
        if (enable_state_.state_.load() != EnableState::State::Enabling)
        {
            selfDisable();
            for (std::size_t j = N; j-- > 0;)
                children_[j]->rollbackLastEnablement();
            return false;
        }

        selfProtect();
        if (controller != nullptr)
        {
            parent_ = controller;
            state_  = State::Controlled;
        }
        else
        {
            state_ = State::Protected;
        }
        return true;
    }

    void rollbackLastEnablement() noexcept final
    {
        if (!enabled_before_last_)
        {
            // 本轮新使能的节点：关闭自身并继续向下回滚；已被失能取消的节点保持 Disabled
            parent_ = nullptr;
            if (state_ != State::Disabled)
            {
                selfDisable();
                state_ = State::Disabled;
            }

            for (std::size_t j = N; j-- > 0;)
                children_[j]->rollbackLastEnablement();
        }
        else if (state_ == State::Controlled)
        {
            // 本轮借用的既有使能节点：只释放本轮取得的控制权
            selfProtect();
            parent_ = nullptr;
            state_  = State::Protected;
        }
    }

    bool lockTree() noexcept final
    {
        if (!enable_state_.lock())
            return false;

        for (std::size_t i = 0; i < N; ++i)
        {
            if (!children_[i]->lockTree())
            {
                for (std::size_t j = i; j-- > 0;)
                    children_[j]->unlockTree();
                (void)enable_state_.unlock();
                return false;
            }
        }
        return true;
    }

    bool unlockTree() noexcept final
    {
        bool uncancelled = enable_state_.unlock();
        for (std::size_t i = 0; i < N; ++i)
            if (!children_[i]->unlockTree())
                uncancelled = false;
        return uncancelled;
    }

    void idleTree() noexcept final
    {
        // 调用者已经确认本节点是活动子节点
        const bool held = enable_state_.cancel();

        parent_ = nullptr;
        selfDisable();
        state_ = State::Disabled;

        for (std::size_t i = 0; i < N; ++i)
            if (children_[i]->parent_ == this)
                children_[i]->idleTree();

        if (held)
            enable_state_.idle();
    }

    void releaseController(ControllerNode* controller) noexcept final
    {
        if (state_ != State::Controlled || parent_ != controller)
            return;

        selfProtect();
        parent_ = nullptr;
        state_  = State::Protected;
    }

    std::array<ControllerNode*, N> children_{};

    /**
     * 节点锁：由使能事务与失能共同使用。
     *
     * Idle -> Enabling 表示某一轮使能持有本节点；Enabling -> Cancelled 表示该轮使能已被
     * 失能取消，标记保持到该轮收尾才被释放；Idle -> Cancelled 表示本次失能临时持有本节点，
     * 由发起者自行释放。重复取消是幂等的。
     */
    class EnableState
    {
    public:
        enum class State
        {
            Idle,
            Enabling,
            Cancelled,
        };

        std::atomic<State> state_{ State::Idle };

        /// 使能事务取得本节点；失败表示本节点已被其他事务或失能持有。
        [[nodiscard]] bool lock() noexcept
        {
            State expected = State::Idle;
            return state_.compare_exchange_strong(expected,
                                                  State::Enabling,
                                                  std::memory_order_acquire,
                                                  std::memory_order_relaxed);
        }

        /// 使能事务释放本节点；返回本节点在本轮中是否没有被取消。
        [[nodiscard]] bool unlock() noexcept
        {
            return state_.exchange(State::Idle, std::memory_order_acq_rel) == State::Enabling;
        }

        /// 取消在途的使能；返回本次调用是否取得了本节点（取得者需要自行 idle() 释放）。
        [[nodiscard]] bool cancel() noexcept
        {
            State expected = State::Enabling;
            if (state_.compare_exchange_strong(expected,
                                               State::Cancelled,
                                               std::memory_order_acq_rel,
                                               std::memory_order_relaxed))
                return false; // 在途使能仍持有本节点，它会自己观察到取消

            expected = State::Idle;
            return state_.compare_exchange_strong(expected,
                                                  State::Cancelled,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_relaxed);
        }

        /// 释放由本次失能持有的节点。
        void idle() noexcept { state_.store(State::Idle, std::memory_order_release); }
    } enable_state_;

    /// 本轮使能开始时本节点是否已经使能；只对本轮访问过的节点有效。
    bool enabled_before_last_{ false };
};

}; // namespace core::control
