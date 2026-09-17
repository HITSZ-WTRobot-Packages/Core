/**
 * @file    IController.hpp
 * @author  syhanjin
 * @date    2026-09-15
 */
#pragma once
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>

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
 * parent_ 非空当且仅当本节点为 Controlled；除正在推进的使能事务之外，失能的节点不持有
 * 子节点（事务中父节点先取得子节点的控制权，最后才提交自身）。
 *
 * 并发模型（单核，任务与 ISR 之间可相互抢占）：
 * 每个节点持有一把四态节点锁 Idle / Enabling / Held / Disabling：
 * - Idle       空闲，任何上下文都可以取得本节点；
 * - Enabling   某一轮使能持有本节点：推进期间（lockTree() 到 unlockTree()）整棵候选子树保持
 *              该标记；
 * - Held       某个状态转移（standalone()/protect()）持有本节点；
 * - Disabling  失能正在接管本节点：失能体取得空闲节点后以该标记持有它，直到失能动作与状态
 *              提交结束；持有者若是使能轮或状态转移，必须在提交状态前观察到该标记、放弃提交
 *              并自行释放本节点锁。
 *
 * - enable() 先按“自身优先”的顺序锁住整个候选子树（lockTree），再自上而下推进；收尾时在
 *   仍持有这些节点锁的情况下检查本轮是否被取消并回滚（disablingTree()），最后统一释放
 *   （unlockTree()）。只有整棵树都没被取消，本轮才算成功；因此取消检查和回滚都不会碰到
 *   其他上下文刚在同一棵树上完成的生命周期操作。
 * - disable() 优先级更高：它先取消本节点（以及控制链上各节点）在途的使能或状态转移，再
 *   执行失能。cancel() 的后置条件是“取消只会发出，不会丢失”：返回 true 表示本节点原为空闲、
 *   已由本次调用临时持有（由调用者释放）；返回 false 表示持有者已被置为 Disabling，它会在
 *   提交状态前观察到取消并自行释放。被取消的在途使能不会提交成功，其已提交的部分由该事务
 *   自身的回滚撤销：本轮新使能的节点被关闭，本轮借用的既有使能节点同样保持失能。失能产生的
 *   状态保持有效：被失能的节点保持 Disabled。
 * - 失能只对“尚未取得节点锁”的使能让步：节点锁取得之前的失能按先后顺序先于本轮使能；
 *   取得之后、本轮取消检查（disablingTree()）之前的失能取消本轮使能；取消检查之后的失能
 *   （包括 unlockTree() 释放节点锁期间的失能）按排在本轮之后处理。
 *
 * 抢占点保证的是最终状态一致，不是 selfEnable() 内部没有重新打开硬件的窗口：取消检查
 * 位于每个节点的推进、自身使能之后与状态提交之前，取消发生后本节点不再提交状态，其自身
 * 硬件由该检查或外层回滚关闭。
 *
 * 状态写入的所有权规则：持有本节点锁的上下文可以直接写 state_；不持有本节点锁、却可能与
 * 失能或控制权释放竞争的写入（回滚释放控制权、releaseController()、standalone()/protect()
 * 的提交）必须用 CAS 复检期望值：CAS 失败表示失能已经接管该节点，此时不再提交，保持失能
 * 结果。
 *
 * 使能是可回滚事务：失败的使能撤销本轮建立的状态与控制关系。enable() 返回 false 表示
 * 本轮失败或存在竞争（例如候选子树正被其他操作锁定），调用者可以稍后重试。
 */
class ControllerNode
{
public:
    virtual ~ControllerNode() { assert(state() == State::Disabled); }
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
    [[nodiscard]] virtual bool enable() noexcept = 0;

    /**
     * 失能当前节点。
     *
     * reverse=false：失能本节点并释放其直接持有的子节点（它们保持使能并成为保护态根），
     * 随后沿控制链向上失能为其服务的各级控制者。reverse=true：要求本节点无控制者，取消本
     * 节点及其活动子树各节点在途的使能或状态转移，再自顶向下关闭整棵活动子树。两种方向都会
     * 取消本节点在途的使能，且本节点自身的失能一定在本次调用内完成；返回 true 只表示本次
     * 失能请求已执行，不表示在途事务已经退出（被取消的事务在观察到取消前可能还在推进）。
     */
    [[nodiscard]] virtual bool disable(bool reverse = false) noexcept = 0;

    /**
     * 无控制者的 Protected -> Standalone；已 Standalone 时幂等。
     * 只改变逻辑权限，不使能硬件、不提交业务命令。与在途的使能轮或其他节点变更竞争时返回
     * false，调用者可以稍后重试。
     */
    [[nodiscard]] virtual bool standalone() noexcept = 0;

    /**
     * 仅 Standalone -> Protected；先提交自身保护行为，再改变状态。提交前被失能取消时不再
     * 改变状态，返回 false（节点保持失能）。
     */
    [[nodiscard]] virtual bool protect() noexcept = 0;

    [[nodiscard]] State state() const noexcept
    {
        return state_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool isEnabled() const noexcept { return state() != State::Disabled; }
    [[nodiscard]] bool isStandalone() const noexcept { return state() == State::Standalone; }
    [[nodiscard]] bool isProtected() const noexcept { return state() == State::Protected; }
    [[nodiscard]] bool isControlled() const noexcept { return state() == State::Controlled; }

    [[nodiscard]] ControllerNode* currentController() const noexcept { return parent_; }

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
     * 已经使能、但不是 Standalone 也没有其他控制者的节点只被借用一次，不重建其子树；
     * 本轮内新使能的节点先推进候选子节点，再执行自身使能、保护与状态提交。任一时刻发现
     * 本节点已被失能取消，立即失败并由调用者回滚。
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

    /// 检查本轮锁定的候选子树上是否有节点已被失能取消；不改变任何节点锁。
    [[nodiscard]] virtual bool disablingTree() const noexcept = 0;

    /// 释放本节点及其候选子树的节点锁。
    virtual void unlockTree() noexcept = 0;

    /// 关闭本节点及其活动子树（活动子节点全部失能，控制关系解除）。
    virtual void idleTree() noexcept = 0;

    /// 释放 controller 通过 parent_ 对本节点持有的控制边。
    virtual void releaseController(ControllerNode* controller) noexcept = 0;

private:
    template <std::size_t> friend class IController;

    std::atomic<State> state_{ State::Disabled };

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

    [[nodiscard]] bool enable() noexcept final
    {
        if (isEnabled())
            return true;

        if (!lockTree())
            return false;

        const bool enabled = enableAndAcquireController(nullptr);

        // 取消检查与回滚都必须在“本轮仍持有节点锁”的区间内完成：该区间内其他上下文无法
        // 取得本树的任何节点，因此回滚只会作用于本轮自己的改动。取消检查之后到达的失能
        // （含 unlockTree() 释放期间的失能）按排在本轮之后处理：它失能成功的节点保持
        // Disabled，但本轮可能已经报成功。
        const bool cancelled = enabled && disablingTree();
        if (cancelled)
            rollbackLastEnablement();

        unlockTree();

        return enabled && !cancelled;
    }

    [[nodiscard]] bool disableTree() noexcept { return disable(true); }

    [[nodiscard]] bool disable(const bool reverse = false) noexcept final
    {
        // 向下失能只能由无控制者的节点发起
        if (reverse && isControlled())
            return false;

        // 取消本节点在途的使能或状态转移；本节点空闲时由本次失能临时持有，直到状态提交完成
        const bool held = enable_state_.cancel();

        ControllerNode* const up = parent_;
        parent_                  = nullptr;
        selfDisable();
        state_.store(State::Disabled, std::memory_order_relaxed);

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
            enable_state_.unlock();

        // 控制链上的节点只是为了控制本节点才被使能，随本节点一同失能
        if (up != nullptr)
            (void)up->disable(false);

        return true;
    }

    [[nodiscard]] bool standalone() noexcept final
    {
        // 状态转移同样是节点范围内的生命周期变更：必须持有节点锁，才能与在途的使能轮、
        // 失能体互斥。取不到锁表示存在竞争，调用者可以稍后重试。
        if (!enable_state_.tryHold())
            return false;

        bool result = false;
        if (isStandalone())
        {
            result = true; // 已 Standalone：幂等
        }
        else if (isProtected())
        {
            // 提交前用 CAS 复检：失能可能在取得节点锁之后接管本节点，此时不再提交
            State expected = State::Protected;
            result = state_.compare_exchange_strong(expected,
                                                    State::Standalone,
                                                    std::memory_order_relaxed);
        }

        enable_state_.unlock();
        return result;
    }

    [[nodiscard]] bool protect() noexcept final
    {
        if (!enable_state_.tryHold())
            return false;

        bool result = false;
        if (isStandalone())
        {
            // 先提交自身保护行为，再改变状态；钩子期间可能被失能接管，因此写状态用 CAS 复检
            selfProtect();

            State expected = State::Standalone;
            result         = state_.compare_exchange_strong(expected,
                                                            State::Protected,
                                                            std::memory_order_relaxed);
            if (!result && state() == State::Disabled)
                selfDisable(); // 失能先落地：撤销刚提交的保护行为
        }

        enable_state_.unlock();
        return result;
    }

private:
    bool acquireControllerWithoutCheckingState(ControllerNode* controller) noexcept
    {
        // 只接受无主的节点；已被其他控制器持有（或已被本轮持有）都视为冲突
        if (parent_ != nullptr)
            return false;

        parent_ = controller;
        return true;
    }

    bool enableAndAcquireController(ControllerNode* controller) noexcept final
    {
        // 本节点在途的使能已被失能取消：立即失败，不再推进
        if (!enable_state_.holdsRound())
            return false;

        if (isEnabled())
        {
            // 只有无主的 Protected 节点可以被借用，Standalone 不接受被间接接管。
            // 状态仍是 Controlled 而 parent_ 已空，说明 releaseController() 正在释放这条控制边，
            // 此时接受借用会让它随后把节点写回使能态，留下悬挂的控制边。
            if (!isProtected())
                return false;

            // 已经使能的节点：本轮只借用，不重建其子树
            enabled_before_last_ = true;
            if (!acquireControllerWithoutCheckingState(controller))
                return false;

            // 收权过程中被失能：失能优先，放弃本次借用
            if (!enable_state_.holdsRound())
            {
                parent_ = nullptr;
                state_.store(State::Disabled, std::memory_order_relaxed);
                return false;
            }

            state_.store(State::Controlled, std::memory_order_relaxed);
            return true;
        }

        enabled_before_last_ = false;

        // 先向下传递使能关系
        for (std::size_t i = 0; i < N; ++i)
        {
            if (!enable_state_.holdsRound() || !children_[i]->enableAndAcquireController(this))
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
        if (!enable_state_.holdsRound())
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
            state_.store(State::Controlled, std::memory_order_relaxed);
        }
        else
        {
            state_.store(State::Protected, std::memory_order_relaxed);
        }
        return true;
    }

    void rollbackLastEnablement() noexcept final
    {
        if (!enabled_before_last_)
        {
            // 本轮新使能的节点：关闭自身并继续向下回滚；已被失能取消的节点保持 Disabled
            parent_ = nullptr;
            if (state() != State::Disabled)
            {
                selfDisable();
                state_.store(State::Disabled, std::memory_order_relaxed);
            }

            for (std::size_t j = N; j-- > 0;)
                children_[j]->rollbackLastEnablement();
            return;
        }

        // 本轮借用的既有使能节点：本轮已被失能取消时，借用的节点也必须保持失能
        if (enable_state_.disabling())
        {
            parent_ = nullptr;
            if (state() != State::Disabled)
            {
                selfDisable();
                state_.store(State::Disabled, std::memory_order_relaxed);
            }
            return;
        }

        // 未取消：控制边仍属于本轮时才释放它，节点保留自己原有的使能
        if (state() != State::Controlled || parent_ == nullptr)
            return;

        selfProtect();
        parent_ = nullptr;

        State expected = State::Controlled;
        if (state_.compare_exchange_strong(expected, State::Protected, std::memory_order_relaxed))
            return;

        // 释放过程中被失能接管：撤销刚提交的保护行为，保持失能
        if (!enable_state_.disabling())
            return;

        selfDisable();
        state_.store(State::Disabled, std::memory_order_relaxed);
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
                enable_state_.unlock();
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool disablingTree() const noexcept final
    {
        if (enable_state_.disabling())
            return true;

        for (std::size_t i = 0; i < N; ++i)
            if (children_[i]->disablingTree())
                return true;
        return false;
    }

    void unlockTree() noexcept final
    {
        enable_state_.unlock();
        for (std::size_t i = 0; i < N; ++i)
            children_[i]->unlockTree();
    }

    void idleTree() noexcept final
    {
        // 调用者已经确认本节点是活动子节点
        const bool held = enable_state_.cancel();

        parent_ = nullptr;
        selfDisable();
        state_.store(State::Disabled, std::memory_order_relaxed);

        for (std::size_t i = 0; i < N; ++i)
            if (children_[i]->parent_ == this)
                children_[i]->idleTree();

        if (held)
            enable_state_.unlock();
    }

    void releaseController(ControllerNode* controller) noexcept final
    {
        if (state() != State::Controlled || parent_ != controller)
            return;

        selfProtect();
        parent_ = nullptr;

        // 只释放这条控制边：并发失能可能已经接管本节点，此时 CAS 失败，保持失能结果
        State expected = State::Controlled;
        if (state_.compare_exchange_strong(expected, State::Protected, std::memory_order_relaxed))
            return;

        if (state() == State::Disabled)
            selfDisable(); // 失能先落地：撤销刚提交的保护行为
    }

    std::array<ControllerNode*, N> children_{};

    /**
     * 节点锁：由使能事务与节点范围内的生命周期变更共同使用。
     *
     * Idle -> Enabling 表示某一轮使能持有本节点；Idle -> Held 表示某个状态转移
     * （standalone()/protect()）持有本节点；Idle -> Disabling 表示失能体取得并临时持有本节点。
     * 持有点被置为 Disabling 表示“失能正在接管本节点，持有者必须在提交状态前观察到它、
     * 放弃提交并自行释放本节点”。重复取消是幂等的。
     */
    class EnableState
    {
    public:
        enum class State
        {
            Idle,
            Enabling,
            Held,
            Disabling,
        };

        /// 使能事务取得本节点；失败表示本节点已被其他事务或变更持有。
        [[nodiscard]] bool lock() noexcept
        {
            State expected = State::Idle;
            return state_.compare_exchange_strong(expected,
                                                  State::Enabling,
                                                  std::memory_order_acquire,
                                                  std::memory_order_relaxed);
        }

        /// 非事务的生命周期变更取得本节点；失败表示存在竞争，调用者可以稍后重试。
        [[nodiscard]] bool tryHold() noexcept
        {
            State expected = State::Idle;
            return state_.compare_exchange_strong(expected,
                                                  State::Held,
                                                  std::memory_order_acquire,
                                                  std::memory_order_relaxed);
        }

        /// 本节点上的使能轮是否仍持有它；被取消后为 false，本轮据此放弃提交。
        [[nodiscard]] bool holdsRound() const noexcept
        {
            return state_.load(std::memory_order_acquire) == State::Enabling;
        }

        /// 本节点是否正在被失能接管；持有者据此放弃提交。
        [[nodiscard]] bool disabling() const noexcept
        {
            return state_.load(std::memory_order_relaxed) == State::Disabling;
        }

        /// 释放本节点：使能事务收尾，或持有者完成变更。
        void unlock() noexcept { state_.store(State::Idle, std::memory_order_release); }

        /**
         * 请求失能接管本节点在途的使能或状态转移。
         *
         * 返回 true 表示本节点原为空闲、已由本次调用临时持有（调用者负责 unlock()）；返回
         * false 表示本节点原有持有者，它已被置为 Disabling，并会在提交状态前观察到它、放弃
         * 提交并自行释放本节点。两种返回值下标记都是 Disabling，因此取消不会只发不收。
         */
        [[nodiscard]] bool cancel() noexcept
        {
            const State previous = state_.exchange(State::Disabling, std::memory_order_acq_rel);
            return previous == State::Idle;
        }

    private:
        std::atomic<State> state_{ State::Idle };
    } enable_state_;

    /// 本轮使能开始时本节点是否已经使能；只对本轮访问过的节点有效。
    bool enabled_before_last_{ false };
};

}; // namespace core::control
