/**
 * @file    IController.hpp
 * @author  syhanjin
 * @date    2026-09-15
 */
#pragma once
#include "AtomicFlagLock.hpp"

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
 * parent_ 非空当且仅当本节点为 Controlled；除正在推进的生命周期事务之外，失能的节点不持有
 * 子节点（使能时父节点先取得子节点的控制权，最后才提交自身）。
 *
 * 并发模型（单核，任务与 ISR 之间可相互抢占）：
 * 每个节点持有一把非阻塞 AtomicFlagLock，所有生命周期操作都必须取得节点锁。
 * - enable() 先按“自身优先”的顺序锁住自身候选子树，不向上锁定祖先；已经使能的节点也必须
 *   先锁定子树，才能幂等返回 true。使能失败时回滚本轮建立的状态与控制关系。
 * - disable() 先锁住自身及沿 parent_ 向上的控制链，稳定根节点和控制关系，再补齐各层候选
 *   子树，取得所在整棵候选树的锁。未建立活动控制边的候选引用也参与锁定。
 * - 任一节点锁获取失败，立即释放本轮已取得的全部锁并返回 false；失能预锁失败不执行钩子、
 *   状态写入或控制关系修改，也不会取消或接管其他生命周期事务。
 * - reverse=false 在完整树已锁定后失能当前节点及其控制链，释放各层直接子节点的控制权；
 *   被释放的子节点及其后代保持使能。
 * - reverse=true 要求当前节点无控制者，在完整候选树已锁定后自顶向下关闭活动子树。
 *   未建立活动控制边的候选引用只参与锁定，不改变状态。
 *
 * 所有状态与控制关系写入都在对应节点锁内完成；提交或回滚结束后才释放本轮节点锁。
 * 预锁失败按递归栈逐层释放成功锁定的子树，不触碰失败节点或其他上下文持有的锁。失能在栈上
 * 保存根节点，传播结束后沿不变的 children_ 解锁，不在节点中保存锁链。返回 true 表示整树
 * 锁定且传播已完成，false 表示节点锁冲突或 reverse=true 的调用节点仍受控。同轮沿不同
 * 候选路径重复访问同一节点也视为冲突；这种拓扑冲突不能靠重试消除。
 *
 * 使能是可回滚事务：失败的使能撤销本轮建立的状态与控制关系。enable() 返回 false 表示本轮
 * 失败或存在竞争（例如候选子树正被其他操作锁定），调用者可以稍后重试。
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
     * 先取得自身及全部候选后代的节点锁；若已使能则幂等返回 true，不重建控制关系。
     * 返回 false 表示本轮失败（自身或后代使能失败、控制权冲突、候选节点处于 Standalone）
     * 或存在竞争，本轮的改动已被回滚。
     */
    [[nodiscard]] virtual bool enable() noexcept = 0;

    /**
     * 失能当前节点。
     *
     * 两种方向都先 try-lock 当前节点所在的整棵候选树：沿 parent_ 锁住祖先链，再补齐各层
     * 候选子树。reverse=false 失能当前节点及其控制链，并释放各层直接子节点的控制权，
     * 被释放的子节点及其后代保持使能。reverse=true 要求当前节点无控制者，只关闭活动子树。
     * 未建立活动边的候选引用参与锁定，但不改变状态。
     *
     * 任一节点已被占用时返回 false，释放本轮取得的全部锁，不执行任何失能钩子、状态写入或
     * 控制关系修改；调用者可以稍后重试。受控节点调用 reverse=true 也返回 false。
     * true 表示整树已锁定且请求的失能传播已经完成。
     */
    [[nodiscard]] virtual bool disable(bool reverse = false) noexcept = 0;

    /**
     * 无控制者的 Protected -> Standalone；已 Standalone 时幂等。
     * 只改变逻辑权限，不使能硬件、不提交业务命令。与在途的使能轮或其他节点变更竞争时返回
     * false，调用者可以稍后重试。
     */
    [[nodiscard]] virtual bool standalone() noexcept = 0;

    /**
     * 仅 Standalone -> Protected；先提交自身保护行为，再改变状态。提交前发生竞争时不改变状态，
     * 返回 false。
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
    // selfDisable() 必须可重复调用，并在节点本就未使能时保持安全；使能回滚和成功失能都
    // 可能调用它。

    virtual bool selfEnable() { return true; }
    virtual void selfDisable() {}
    virtual void selfProtect() {}

private:
    ControllerNode() = default;

    /**
     * 使能本节点并让 controller 取得其控制权，由 enable() 与父节点驱动。
     *
     * 已经使能、但不是 Standalone 也没有其他控制者的节点只被借用一次，不重建其子树；本轮内新
     * 使能的节点先推进候选子节点，再执行自身使能、保护与状态提交。
     */
    virtual bool enableAndAcquireController(ControllerNode* controller) noexcept = 0;

    /**
     * 回滚本轮使能对本节点状态与控制权的改动。
     *
     * 本轮新使能的节点关闭自身并向下回滚；本轮借用的既有使能节点只释放本轮获得的控制权，
     * 回到 Protected 并保留其原有子树。回滚可重复执行。
     */
    virtual void rollbackLastEnablement() noexcept = 0;

    /// 本轮开始前节点是否已使能；仅供本轮回滚判断。
    [[nodiscard]] virtual bool lastEnableState() const noexcept = 0;
    /// 锁住自身候选子树；失败时释放本轮已经取得的节点锁。
    virtual bool lockTree() noexcept = 0;
    /// 沿不变的候选引用释放完整子树的锁，不依赖可能已被失能清除的 parent_。
    virtual void unlockTree() noexcept = 0;

    /// 本节点已锁定：锁住祖先及各层剩余子树，返回根节点；失败只保留本节点的锁。
    virtual bool lockDisableUpstream(ControllerNode*  lockedChild,
                                     ControllerNode*& root) noexcept = 0;
    /// 预锁回退：释放本节点、祖先及各层子树，但跳过尚由调用者持有的向下路径。
    virtual void unlockDisableUpstream(ControllerNode* lockedChild) noexcept = 0;

    /// 在所在整棵候选树已锁定时执行向上失能。
    virtual void disableUpstreamLocked() noexcept = 0;

    /// 在所在整棵候选树已锁定时执行活动子树失能。
    virtual void disableTreeLocked() noexcept = 0;

    /// 释放 controller 通过 parent_ 对本节点持有的控制边。
    virtual void releaseControllerWithoutCheckState(ControllerNode* controller) noexcept = 0;

private:
    template <std::size_t> friend class IController;

    std::atomic<State> state_{ State::Disabled };

    ControllerNode* parent_{ nullptr };
    AtomicFlagLock  lock_flag_;
};

template <std::size_t N = 0> class IController : public ControllerNode
{
    [[nodiscard]] bool lastEnableState() const noexcept final { return last_enable_state_; }

    std::array<ControllerNode*, N> children_{};

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
     * 整个候选图无环。同轮重复访问同一候选节点会使预锁失败，不会重复取得或释放其锁。
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
        if (!lockTree())
            return false;

        // 幂等成功也必须发生在子树已完整锁定之后。
        const bool enabled = isEnabled() || enableAndAcquireController(nullptr);
        if (!enabled)
            rollbackLastEnablement();

        unlockTree();
        return enabled;
    }

    [[nodiscard]] bool disableTree() noexcept { return disable(true); }

    [[nodiscard]] bool disable(const bool reverse = false) noexcept final
    {
        if (!lock_flag_.lock())
            return false;

        // 持有自身锁后检查控制关系；预锁失败前不执行任何生命周期改动。
        if (reverse && parent_ != nullptr)
        {
            lock_flag_.unlock();
            return false;
        }

        ControllerNode* root = nullptr;
        if (!lockDisableUpstream(nullptr, root))
        {
            lock_flag_.unlock();
            return false;
        }

        if (reverse)
            disableTreeLocked();
        else
            disableUpstreamLocked();

        // 失能会清除 parent_；必须使用预锁阶段保存在栈上的根节点解锁。
        // children 并不会发生变化，这里不会由于 parent_ 断开导致无法 unlock
        root->unlockTree();
        return true;
    }

    [[nodiscard]] bool standalone() noexcept final
    {
        if (!lock_flag_.lock())
            return false;

        bool result = false;
        if (isStandalone())
            result = true;
        else if (isProtected())
        {
            state_.store(State::Standalone, std::memory_order_relaxed);
            result = true;
        }

        lock_flag_.unlock();
        return result;
    }

    [[nodiscard]] bool protect() noexcept final
    {
        if (!lock_flag_.lock())
            return false;

        bool result = false;
        if (isStandalone())
        {
            selfProtect();
            state_.store(State::Protected, std::memory_order_relaxed);
            result = true;
        }

        lock_flag_.unlock();
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
        if (isEnabled())
        {
            // 只有无主的 Protected 节点可以被借用，Standalone 不接受被间接接管。
            if (!isProtected())
                return false;

            // 已经使能的节点：本轮只借用，不重建其子树。
            if (controller == nullptr)
                return true;

            if (!acquireControllerWithoutCheckingState(controller))
                return false;

            state_.store(State::Controlled, std::memory_order_relaxed);
            return true;
        }

        // 先向下传递使能关系。
        for (std::size_t i = 0; i < N; ++i)
        {
            if (!children_[i]->enableAndAcquireController(this))
            {
                // 回滚本轮已经使能成功的子节点。
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
        if (!last_enable_state_)
        {
            // 本轮开始时未使能：关闭本轮新使能，并只向下处理本轮可能访问过的节点。
            parent_ = nullptr;
            if (state() != State::Disabled)
            {
                selfDisable();
                state_.store(State::Disabled, std::memory_order_relaxed);
            }

            for (std::size_t j = N; j-- > 0;)
                if (children_[j]->parent_ == this || !children_[j]->lastEnableState() ||
                    children_[j]->state() == State::Disabled)
                    children_[j]->rollbackLastEnablement();
            return;
        }

        // 未取得本轮控制边，或这是一个已使能的根节点：不改变既有使能状态。
        if (state() != State::Controlled || parent_ == nullptr)
            return;

        selfProtect();
        parent_ = nullptr;

        state_.store(State::Protected, std::memory_order_relaxed);
    }

    bool lockTree() noexcept final
    {
        if (!lock_flag_.lock())
            return false;

        last_enable_state_ = isEnabled();
        for (std::size_t i = 0; i < N; ++i)
        {
            if (!children_[i]->lockTree())
            {
                // 失败的子树已自行回退，只释放此前完整锁定的子树。
                for (std::size_t j = i; j-- > 0;)
                    children_[j]->unlockTree();
                lock_flag_.unlock();
                return false;
            }
        }
        return true;
    }

    void unlockTree() noexcept final
    {
        for (std::size_t i = N; i-- > 0;)
            children_[i]->unlockTree();
        lock_flag_.unlock();
    }

    bool lockDisableUpstream(ControllerNode* lockedChild, ControllerNode*& root) noexcept final
    {
        // 先稳定整条控制链，避免向上寻找根节点时 parent_ 被其他事务改写。
        if (parent_ != nullptr)
        {
            if (!parent_->lock_flag_.lock())
                return false;
            if (!parent_->lockDisableUpstream(this, root))
            {
                parent_->lock_flag_.unlock();
                return false;
            }
        }
        else
        {
            root = this;
        }

        for (std::size_t i = 0; i < N; ++i)
        {
            if (children_[i] == lockedChild)
                continue;
            if (!children_[i]->lockTree())
            {
                // 当前层只回退成功前缀，保留向下路径供调用者逐层解锁。
                for (std::size_t j = i; j-- > 0;)
                    if (children_[j] != lockedChild)
                        children_[j]->unlockTree();
                if (parent_ != nullptr)
                    parent_->unlockDisableUpstream(this);
                return false;
            }
        }
        return true;
    }

    void unlockDisableUpstream(ControllerNode* lockedChild) noexcept final
    {
        // 仅用于预锁失败回退，此时 parent_ 尚未改变，向上各层均已完整锁定。
        for (std::size_t i = N; i-- > 0;)
            if (children_[i] != lockedChild)
                children_[i]->unlockTree();
        if (parent_ != nullptr)
            parent_->unlockDisableUpstream(this);
        lock_flag_.unlock();
    }

    void disableTreeLocked() noexcept final
    {
        parent_ = nullptr;
        selfDisable();
        state_.store(State::Disabled, std::memory_order_relaxed);
        for (auto& child : children_)
            if (child->parent_ == this)
                child->disableTreeLocked();
    }

    void disableUpstreamLocked() noexcept final
    {
        const auto up = parent_;
        parent_       = nullptr;
        selfDisable();
        state_.store(State::Disabled, std::memory_order_relaxed);
        for (auto& child : children_)
            child->releaseControllerWithoutCheckState(this);

        if (up != nullptr)
            up->disableUpstreamLocked();
    }

    void releaseControllerWithoutCheckState(ControllerNode* controller) noexcept final
    {
        if (parent_ != controller)
            return;

        selfProtect();
        parent_ = nullptr;
        state_.store(State::Protected, std::memory_order_relaxed);
    }

    /// 本轮开始前节点是否已使能；lockTree() 每轮都会刷新，避免使用过期回滚状态。
    bool last_enable_state_{ false };
};

}; // namespace core::control
