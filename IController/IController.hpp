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
 * 每个节点持有一把三态节点锁 Idle / Active / Disabling：
 * - Idle       空闲，任何上下文都可以取得本节点；
 * - Active     使能轮或状态转移（standalone()/protect()）持有本节点；使能轮推进期间
 *              （lockTree() 到 unlockEnableTree()）整棵候选子树保持该标记；
 * - Disabling  失能正在接管本节点：若节点原为空闲，失能体取得取消声明并在自身动作完成后释放；若
 *              节点原由使能轮或状态转移持有，失能体只发出取消，持有者必须在提交前观察到该标记，
 *              回滚后再将标记恢复为 Idle。
 *
 * - enable() 先按“自身优先”的顺序锁住整个候选子树（lockTree），再自上而下推进；收尾时在
 *   仍持有这些节点锁的情况下检查本轮是否被取消并回滚（disablingTree），最后通过
 *   unlockEnableTree() 释放。只有整棵树都没被取消，本轮才算成功；因此取消检查和回滚都不会碰到
 *   其他上下文刚在同一棵树上完成的生命周期操作。
 * - disable() 优先级更高：它先取消本节点（以及控制链上各节点）在途的使能或状态转移，再
 *   执行失能。cancel() 返回 true 表示本节点原为空闲，本次失能负责在动作完成后释放标记；返回
 *   false 表示已有使能轮、状态转移或其他失能体持有/接管标记，由实际持有者在回滚或失能完成后
 *   释放。被取消的在途使能不会提交成功，其已提交的部分由该事务自身的回滚撤销：本轮新使能的
 *   节点被关闭，本轮借用的既有使能节点同样保持失能。失能产生的状态保持有效：被失能的节点保持
 *   Disabled。
 * - 失能只对“尚未取得节点锁”的使能让步：节点锁取得之前的失能按先后顺序先于本轮使能；
 *   取得之后、unlockEnableTree() 完成释放之前的失能都会被持有者观察并回滚；标记恢复为 Idle
 *   之后到达的失能才按排在本轮之后处理。
 *
 * 抢占点保证的是最终状态一致，不是 selfEnable() 内部没有重新打开硬件的窗口：取消检查
 * 位于每个节点的推进、自身使能之后与状态提交之前，取消发生后本节点不再提交状态，其自身
 * 硬件由该检查或外层回滚关闭。
 *
 * 状态写入的所有权规则：持有本节点锁的上下文可以直接写 state_；不持有本节点锁、却可能与
 * 失能或控制权释放竞争的写入（回滚释放控制权、releaseController、standalone()/protect()
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
     * 已使能时先取得自身节点锁；若未被失能接管则幂等返回 true，不遍历子树。返回 false 表示
     * 本轮失败（自身或后代使能失败、控制权冲突、候选节点处于 Standalone）或存在竞争，本轮的
     * 改动已被回滚。
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

    [[nodiscard]] State state() const noexcept { return state_.load(std::memory_order_relaxed); }

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
     * 回到 Protected 并保留其原有子树；已被失能取消的节点保持 Disabled。回滚可重复执行，
     * 也可被 disable 抢占：所有路径都收敛到失能结果，不重新建立控制边。
     */
    virtual void rollbackLastEnablement() noexcept = 0;

    /// 以本节点优先的顺序锁住候选子树；失败时释放已锁住的部分。
    virtual bool lockTree() noexcept = 0;

    /// 检查本轮锁定的候选子树上是否有节点已被失能取消；不改变任何节点锁。
    [[nodiscard]] virtual bool disablingTree() const noexcept = 0;

    /// 释放本节点及其候选子树的节点锁。
    virtual void unlockTree() noexcept = 0;
    /// 释放使能事务节点锁；发现取消时先回滚本轮，再释放取消标记。
    virtual void unlockEnableTree() noexcept = 0;

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
        // 已使能节点的幂等路径也必须取得自身锁，避免与 disable 的状态提交交错。
        if (isEnabled())
        {
            if (!enable_state_.lock())
                return false;
            if (isEnabled())
            {
                if (enable_state_.unlock())
                    return true;
                enable_state_.finishDisable();
                return false;
            }
            if (!enable_state_.unlock())
                enable_state_.finishDisable();
            return false;
        }

        // 尝试向下锁定树，注意，锁定树只保证不会有另一个使能请求同时进行
        if (!lockTree())
            return false;

        // 向下使能并获取控制权，这是一个非原子操作
        const bool enabled = enableAndAcquireController(nullptr);

        // 上面的使能过程一定没有被使能或者 protect/standalone 打断，但是期间可能产生了 disable
        // 此时 enable 成功但是中途产生了 disable，即使能被取消，需要 rollback
        const bool cancelled = enabled && disablingTree();

        // 如果使能并没有成功，需要回滚使能操作.
        // 该操作非原子，但是在 rollback 的过程中*不会*导致错误的使能
        if (cancelled)
            rollbackLastEnablement();

        // 解锁 tree；若释放时发现取消，unlockEnableTree 会先回滚本轮。
        unlockEnableTree();

        // 使能成功并且没有取消，即最终使能成功
        return enabled && !cancelled;
    }

    [[nodiscard]] bool disableTree() noexcept { return disable(true); }

    [[nodiscard]] bool disable(const bool reverse = false) noexcept final
    {
        // 向下失能只能由无控制者的节点发起
        if (reverse && (isControlled() || parent_ != nullptr))
            return false;

        // 将本节点状态置为 cancelled
        // 可以阻止新的 enable 进入本节点；同时标记本节点使能取消
        const bool held = enable_state_.cancel();

        // 先向上确保控制权断开
        ControllerNode* const up = parent_;
        parent_                  = nullptr;

        // 失能自身，并切换状态为失能
        selfDisable();
        state_.store(State::Disabled, std::memory_order_relaxed);

        if (reverse)
        {
            // 需要向下传递失能
            for (std::size_t i = 0; i < N; ++i)
                if (children_[i]->parent_ == this)
                    children_[i]->idleTree();
        }
        else
        {
            // 仅需向下释放控制权
            for (std::size_t i = 0; i < N; ++i)
                children_[i]->releaseController(this);
        }

        // 先向上传递失能，再释放锁，可以保证
        // 1. 任何使能链进来都会被本节点阻止
        // 2. 本节点打断使能链能成功被识别为 cancelled
        if (up != nullptr)
            (void)up->disable(false);

        // 只有本次 disable 从 Idle 取得了取消标记，才由 disable 自身恢复为 Idle；
        // 原本由使能轮或状态转移持有的节点保持 Disabling，由被打断的函数回滚后释放。
        if (held)
            enable_state_.finishDisable();

        return true;
    }

    [[nodiscard]] bool standalone() noexcept final
    {
        // 状态转移同样是节点范围内的生命周期变更：必须持有节点锁，才能与在途的使能轮、
        // 失能体互斥。取不到锁表示存在竞争，调用者可以稍后重试。
        if (!enable_state_.lock())
            return false;

        bool result = false;
        if (isStandalone())
        {
            result = true; // 已 Standalone：幂等
        }
        else if (isProtected())
        {
            // 提交前用 CAS 复检：失能可能在取得节点锁之后接管本节点，此时不再提交
            auto expected = State::Protected;
            result        = state_.compare_exchange_strong(expected,
                                                    State::Standalone,
                                                    std::memory_order_relaxed);
        }
        // 通过 bool unlock() 判断是否被 disable 打断
        if (enable_state_.unlock())
            return result;
        enable_state_.finishDisable();
        return false;
    }

    [[nodiscard]] bool protect() noexcept final
    {
        if (!enable_state_.lock())
            return false;

        bool result = false;
        if (isStandalone())
        {
            // 先提交自身保护行为，再改变状态；钩子期间可能被失能接管，因此写状态用 CAS 复检
            selfProtect();

            auto expected = State::Standalone;
            result        = state_.compare_exchange_strong(expected,
                                                    State::Protected,
                                                    std::memory_order_relaxed);
            if (!result && state() == State::Disabled)
                selfDisable(); // 失能先落地：撤销刚提交的保护行为
        }

        if (enable_state_.unlock())
            return result;
        enable_state_.finishDisable();
        return false;
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
        if (!enable_state_.isActive())
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
            if (controller == nullptr)
                return true;

            if (!acquireControllerWithoutCheckingState(controller))
                return false;

            // 收权过程中被失能：失能优先，放弃本次借用
            if (!enable_state_.isActive())
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
            if (!enable_state_.isActive() || !children_[i]->enableAndAcquireController(this))
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
        if (!enable_state_.isActive())
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
                if (!enable_state_.unlock())
                    enable_state_.finishDisable();
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
    /// 仅释放节点锁；用于 lockTree() 失败回滚，不执行事务状态回滚。
    void unlockTree() noexcept final
    {
        if (!enable_state_.unlock())
            enable_state_.finishDisable();
        for (std::size_t i = 0; i < N; ++i)
            children_[i]->unlockTree();
    }

    void unlockEnableTree() noexcept final
    {
        if (!enable_state_.unlock())
        {
            rollbackLastEnablement();
            enable_state_.finishDisable();
        }
        for (std::size_t i = 0; i < N; ++i)
            children_[i]->unlockEnableTree();
    }

    void idleTree() noexcept final
    {
        // 向下失能整颗树，首先取消可能存在的使能请求
        const bool held = enable_state_.cancel();

        // 向上断开控制权，并失能自身
        parent_ = nullptr;
        selfDisable();
        state_.store(State::Disabled, std::memory_order_relaxed);

        // 向下递归失能
        for (std::size_t i = 0; i < N; ++i)
            if (children_[i]->parent_ == this)
                children_[i]->idleTree();

        // 如果本次 idleTree 取得了取消声明，则在整棵树完成失能后释放它。
        if (held)
            enable_state_.finishDisable();
    }

    void releaseController(ControllerNode* controller) noexcept final
    {
        if (state() != State::Controlled || parent_ != controller)
            return;

        selfProtect();
        parent_ = nullptr;

        // 只释放这条控制边：并发失能可能已经接管本节点，此时 CAS 失败，保持失能结果
        auto expected = State::Controlled;
        if (state_.compare_exchange_strong(expected, State::Protected, std::memory_order_relaxed))
            return;

        if (state() == State::Disabled)
            selfDisable(); // 失能先落地：撤销刚提交的保护行为
    }

    std::array<ControllerNode*, N> children_{};

    /**
     * 节点锁：由使能事务与节点范围内的生命周期变更共同使用。
     *
     * Idle -> Active 表示使能轮或状态转移（standalone()/protect()）持有本节点；
     * Idle -> Disabling 表示失能请求已接管本节点；若原状态为 Idle，由失能体释放，若原状态为
     * Active，则由被打断的持有者回滚后释放。重复取消不会夺取已有失能体的释放责任。
     */
    class EnableState
    {
    public:
        enum class State
        {
            Idle,
            Active,
            Disabling,
        };

        /// 取得本节点；失败表示本节点已被其他事务或变更持有。
        [[nodiscard]] bool lock() noexcept
        {
            // lock 必须从 Idle 变更过来
            State expected = State::Idle;
            return state_.compare_exchange_strong(expected,
                                                  State::Active,
                                                  std::memory_order_acquire,
                                                  std::memory_order_relaxed);
        }

        /// 本节点上的使能轮是否仍持有它；被取消后为 false，本轮据此放弃提交。
        [[nodiscard]] bool isActive() const noexcept
        {
            return state_.load(std::memory_order_acquire) == State::Active;
        }

        /// 本节点是否正在被失能接管；持有者据此放弃提交。
        [[nodiscard]] bool disabling() const noexcept
        {
            return state_.load(std::memory_order_relaxed) == State::Disabling;
        }

        /// 释放正常持有；若发现 Disabling，调用者必须先完成回滚，再调用 finishDisable。
        void finishDisable() noexcept { state_.store(State::Idle, std::memory_order_release); }

        bool unlock() noexcept
        {
            // 只释放正常持有；Disabling 标记留给被打断的持有者在回滚后释放。
            State expected = State::Active;
            return state_.compare_exchange_strong(expected,
                                                  State::Idle,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire);
        }

        /**
         * 请求失能接管本节点在途的使能或状态转移。
         *
         * 返回 true 表示本节点原为空闲，本次失能取得并负责释放取消标记；返回 false 表示本节点
         * 原本由使能轮或状态转移持有，或已经由其他失能体接管。前一种情况下，被打断的持有者
         * 必须回滚后调用 finishDisable()；后一种情况下由已有失能体负责释放。
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
