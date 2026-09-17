#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>

namespace core::controller
{

/**
 * 生命周期变更在所有节点间互斥；对象构造、销毁和引用生命周期由调用者同步。
 * 候选引用必须组成 DAG，不检测环。业务派生类只继承 IController。
 */
class ControllerNode
{
public:
    virtual ~ControllerNode() { assert(state_ == State::Disabled); }
    ControllerNode(const ControllerNode&)            = delete;
    ControllerNode& operator=(const ControllerNode&) = delete;
    ControllerNode(ControllerNode&&)                 = delete;
    ControllerNode& operator=(ControllerNode&&)      = delete;

    /**
     * 节点在控制生命周期中的状态。
     *
     * 状态只由 ControllerNode/IController 的生命周期操作维护；业务派生类不应直接修改
     * 状态。除非正在执行一次持门的生命周期事务，否则 parent_ 非空仅对应 Controlled。
     */
    enum class State
    {
        /// 自身未使能，也没有活动控制权关系。
        Disabled,
        /// 已完成自身使能并处于硬件保护态，不接受直接控制命令。
        Protected,
        /// 无父控制器的直接控制态；只表示逻辑权限，不自动改变硬件输出。
        Standalone,
        /// 已被 parent_ 指向的控制器获取控制权，不接受自身直接控制命令。
        Controlled,
    };

    /**
     * 生命周期操作的结果。
     *
     * `Busy` 表示另一个生命周期事务正在执行；调用者可以在外部稍后重试。所有其他
     * 失败结果都只描述本次操作，不允许把失败当作状态或控制权已经改变。
     */
    enum class Result
    {
        /// 操作完成，或该操作在当前状态下是允许的幂等操作。
        Ok,
        /// 全局生命周期事务门已被其他调用者占用，本次操作未执行。
        Busy,
        /// 当前状态不允许执行请求的操作。
        InvalidState,
        /// 请求获取的节点已由另一个控制器持有。
        OwnershipConflict,
        /// 节点自身的 selfEnable() 失败，整次 enable 已回滚。
        EnableFailed,
    };

    /**
     * Disabled -> Protected，向下使能并获取控制权；已经使能时幂等，不遍历子树。
     * 非 Busy 失败会回滚本次新增状态和控制关系，但不能撤销已提交的硬件事件。
     */
    [[nodiscard]] virtual Result enable() noexcept = 0;

    /**
     * 仅 Protected/Standalone -> Disabled；Controlled 的外部调用被拒绝。
     *
     * @param reverse false 只释放直接子节点，true 要求根节点并关闭整棵活动树。
     */
    [[nodiscard]] virtual Result disable(bool reverse = false) noexcept = 0;

    /**
     * 无 owner 的 Protected -> Standalone；已 Standalone 时幂等。
     * 仅修改逻辑权限，不使能硬件、不提交业务命令。
     */
    [[nodiscard]] virtual Result standalone() noexcept final
    {
        if (lifecycle_gate_.test_and_set(std::memory_order_acquire))
            return Result::Busy;

        Result result = Result::InvalidState;
        if (parent_ == nullptr)
        {
            if (state_ == State::Protected)
            {
                state_ = State::Standalone;
                result = Result::Ok;
            }
            else if (state_ == State::Standalone)
            {
                result = Result::Ok;
            }
        }

        lifecycle_gate_.clear(std::memory_order_release);
        return result;
    }

    /**
     * 仅 Standalone -> Protected；先提交自身硬件保护，再改变逻辑状态。
     */
    [[nodiscard]] virtual Result protect() noexcept final
    {
        if (lifecycle_gate_.test_and_set(std::memory_order_acquire))
            return Result::Busy;

        Result result = Result::InvalidState;
        if (state_ == State::Standalone)
        {
            selfProtect();
            state_ = State::Protected;
            result = Result::Ok;
        }

        lifecycle_gate_.clear(std::memory_order_release);
        return result;
    }

    // TODO(thread-safety): Lifecycle mutations are serialized, but these getters are not.
    // Concurrent reads of state_/parent_ and lifecycle writes are a C++ data race;
    // separate reads may also observe intermediate or inconsistent state/owner pairs.
    // Use only with external synchronization or when lifecycle operations are quiescent.
    // The getter synchronization contract is intentionally deferred.
    // Derived update/setTarget operations are not serialized with the self* hooks either.

    /**
     * 返回当前逻辑状态。
     *
     * 注意：该查询不获取生命周期事务门。调用者必须保证生命周期操作静止，或自行提供
     * 外部同步；否则读取可能与状态写入产生数据竞争。
     */
    [[nodiscard]] State state() const { return state_; }

    /**
     * @return 当前节点是否不是 Disabled。
     *
     * Protected、Standalone、Controlled 均属于已使能状态。本查询与 state() 具有相同的
     * 外部同步要求。
     */
    [[nodiscard]] bool enabled() const { return state_ != State::Disabled; }

    /**
     * @return 当前节点是否处于 Standalone。
     *
     * 本查询与 state() 具有相同的外部同步要求。
     */
    [[nodiscard]] bool isStandalone() const { return state_ == State::Standalone; }

    /**
     * @return 当前节点是否处于 Protected。
     *
     * 本查询与 state() 具有相同的外部同步要求。
     */
    [[nodiscard]] bool isProtected() const { return state_ == State::Protected; }

    /**
     * @return 当前节点是否处于 Controlled。
     *
     * 本查询与 state() 具有相同的外部同步要求。
     */
    [[nodiscard]] bool isControlled() const { return state_ == State::Controlled; }

    /**
     * @return 当前持有本节点控制权的父控制器；没有控制器时返回 nullptr。
     *
     * 返回值为 non-owning 裸指针，只表示当前控制权关系，不延长父节点生命周期。本查询
     * 与 state() 具有相同的外部同步要求。
     */
    [[nodiscard]] ControllerNode* currentController() const { return parent_; }

protected:
    // 所有 self* 均在持门期间运行：必须短时、ISR 可用、非阻塞、不抛异常。
    // 禁止重入任何生命周期操作、传播父子关系或读取未同步的状态查询。
    // void 钩子必须完成不可失败的本地动作或非阻塞提交，不表示异步总线已完成。

    /**
     * 使节点进入硬件保护状态。
     *
     * 只执行本节点的硬件保护动作，不修改 state_，不传递控制关系，也不获取事务门。
     */
    virtual void selfProtect() = 0;

    /**
     * 执行本节点自身的使能动作。
     *
     * 只处理本节点硬件或内部资源，禁止调用 child 的生命周期函数或传播控制关系。返回
     * false 时，派生类必须自行清理本次部分使能，使节点保持调用前的 Disabled 状态。
     */
    virtual bool selfEnable() { return true; }

    /**
     * 执行本节点自身的失能动作。
     *
     * 只处理本节点硬件或内部资源，不传播父子失能关系。它是成功 selfEnable 的逆动作，
     * 由框架在持有生命周期事务门时调用。
     */
    virtual void selfDisable() {}

    /**
     * 所有传播函数均为 private；friendship 不传递给 IController 的业务派生类。
     * ControllerNode 只允许 IController<N> 通过 friendship 驱动这些关系变更。
     */
private:
    ControllerNode() = default;

    // 在事务外，parent_ 非空当且仅当 state_ 为 Controlled。
    State state_{ State::Disabled };

    // non-owning 当前控制器指针；其生命周期必须由调用者保证。
    ControllerNode* parent_{ nullptr };

    /**
     * 所有 ControllerNode 实例和所有 IController<N> 特化共享的生命周期事务门。
     *
     * 门只提供一次性的 try-acquire 语义：test_and_set 返回 true 时调用者必须立即返回
     * Busy，不能 clear 或等待。成功获取门的公共入口负责在唯一函数尾 clear；递归内部
     * 函数不得再次操作该门。
     */
    inline static std::atomic_flag lifecycle_gate_ = ATOMIC_FLAG_INIT;

    // enable 事务的 intrusive 回滚链节点；只在持有 lifecycle_gate_ 时访问。
    ControllerNode* enable_rollback_next_{ nullptr };

    // 本节点进入回滚链前的状态；commit/rollback 完成后恢复为 Disabled 哨兵值。
    State enable_previous_state_{ State::Disabled };

    /**
     * 将本节点加入本次 enable 的回滚链。
     *
     * 调用者必须保证本节点尚未被当前事务记录；该函数不分配内存、不获取门，也不执行
     * 硬件动作。
     */
    void rememberEnable(ControllerNode*& rollback_head) noexcept
    {
        enable_previous_state_ = state_;
        enable_rollback_next_  = rollback_head;
        rollback_head          = this;
    }

    /**
     * 提交一次成功的 enable 事务，丢弃其回滚记录。
     *
     * 该函数只清理 intrusive 日志，不改变节点状态、控制权或硬件；调用者必须已经持有
     * lifecycle_gate_，并且 rollback_head 在返回时为 nullptr。
     */
    static void commitEnable(ControllerNode*& rollback_head) noexcept
    {
        while (rollback_head != nullptr)
        {
            ControllerNode* node         = rollback_head;
            rollback_head                = node->enable_rollback_next_;
            node->enable_rollback_next_  = nullptr;
            node->enable_previous_state_ = State::Disabled;
        }
    }

    /**
     * 逆序回滚一次失败的 enable 事务。
     *
     * 新开启节点执行 selfDisable 并回到 Disabled；原先 Protected 的借用节点只释放本次
     * 获取的 parent_，恢复为 Protected，并保留其原有子树。该函数不获取、不释放事务门，
     * 也不调用公共 disable()，避免影响事务之外的活动节点。
     */
    static void rollbackEnable(ControllerNode*& rollback_head) noexcept
    {
        while (rollback_head != nullptr)
        {
            ControllerNode* node = rollback_head;
            rollback_head        = node->enable_rollback_next_;

            if (node->enable_previous_state_ == State::Disabled)
            {
                // 新开启节点直接关闭，不额外提交一次保护，也不传播到事务外。
                if (node->state_ != State::Disabled)
                {
                    node->selfDisable();
                    node->state_ = State::Disabled;
                }
                node->parent_ = nullptr;
            }
            else if (node->parent_ != nullptr)
            {
                // 借用的已使能子树只释放本次获得的边，保留它原有的后代。
                node->releaseController(node->parent_);
            }

            node->enable_rollback_next_  = nullptr;
            node->enable_previous_state_ = State::Disabled;
        }
    }

    /**
     * 递归使能当前节点的全部候选子节点，并在最后完成自身使能。
     *
     * 前置条件：当前节点为 Disabled，调用者已取得 lifecycle_gate_。子节点按数组顺序
     * 处理；任一子节点失败立即返回，由最外层入口统一执行 rollbackEnable()。
     */
    Result enableSubtree(ControllerNode*& rollback_head) noexcept
    {
        for (std::size_t i = 0, count = childCount(); i < count; ++i)
        {
            const Result result = childAt(i)->enableAndAcquireController(this, rollback_head);
            if (result != Result::Ok)
                return result;
        }

        rememberEnable(rollback_head);
        if (!selfEnable())
            return Result::EnableFailed;

        selfProtect();
        state_ = State::Protected;
        return Result::Ok;
    }

    /**
     * 获取本节点的控制权，仅由 IController 的 enable 传播调用。
     *
     * 只有 Protected 节点可以被获取。Controlled 同一 owner 的重复获取幂等成功；不同
     * owner 返回 OwnershipConflict；Standalone 永远不会被隐式抢占或自动 protect。
     * 调用者必须已取得 lifecycle_gate_。
     */
    Result acquireController(ControllerNode* controller) noexcept
    {
        if (controller == nullptr)
            return Result::InvalidState;
        if (state_ == State::Controlled)
            return parent_ == controller ? Result::Ok : Result::OwnershipConflict;
        if (state_ != State::Protected || parent_ != nullptr)
            return Result::InvalidState;

        parent_ = controller;
        state_  = State::Controlled;
        return Result::Ok;
    }

    /**
     * 在同一 enable 事务中使能节点并获取其控制权。
     *
     * Disabled 节点递归使能；Protected 节点只记录并获取，不重建已有子树；Controlled
     * 节点只允许同一 owner 重复获取。rollback_head 必须引用当前最外层 enable 的日志头。
     */
    Result enableAndAcquireController(ControllerNode*  controller,
                                      ControllerNode*& rollback_head) noexcept
    {
        if (controller == nullptr)
            return Result::InvalidState;
        if (state_ == State::Controlled)
            return acquireController(controller);
        if (state_ == State::Standalone)
            return Result::InvalidState;

        if (state_ == State::Disabled)
        {
            const Result result = enableSubtree(rollback_head);
            if (result != Result::Ok)
                return result;
        }
        else
        {
            rememberEnable(rollback_head);
        }

        return acquireController(controller);
    }

    /**
     * 释放指定 controller 对本节点持有的控制权。
     *
     * 只有 owner 身份匹配且本节点为 Controlled 时才生效。释放不会关闭本节点或其子节点，
     * 而是先提交 selfProtect，再清除 parent_ 并回到 Protected。调用者必须已取得门。
     */
    void releaseController(ControllerNode* controller) noexcept
    {
        if (state_ != State::Controlled || parent_ != controller)
            return;

        selfProtect();
        parent_ = nullptr;
        state_  = State::Protected;
    }

    /**
     * 私有向上传播失能。
     *
     * 允许处理 Controlled 节点，仅供框架内部调用；它不检查公共入口权限，也不获取门。
     * 当前节点先失能，再释放直接子节点的控制权，然后沿旧 parent_ 继续向上。被释放的
     * 子节点保持使能并进入 Protected，其已有后代关系不受影响。
     */
    void disableUpstream() noexcept
    {
        ControllerNode* node = this;
        while (node != nullptr)
        {
            ControllerNode* ancestor = node->parent_;
            node->selfDisable();
            node->state_  = State::Disabled;
            node->parent_ = nullptr;

            for (std::size_t i = 0, count = node->childCount(); i < count; ++i)
            {
                ControllerNode* child = node->childAt(i);
                if (child->parent_ == node)
                    child->releaseController(node);
            }
            node = ancestor;
        }
    }

    /**
     * 私有向下传播失能。
     *
     * 只沿 child->parent_ == this 的活动边递归，不遍历未建立控制权的候选引用。当前节点
     * 先执行 selfDisable 并进入 Disabled，再处理活动子节点；该函数不向上传播、不获取门。
     */
    void disableDownstream() noexcept
    {
        selfDisable();
        state_  = State::Disabled;
        parent_ = nullptr;

        for (std::size_t i = 0, count = childCount(); i < count; ++i)
        {
            ControllerNode* child = childAt(i);
            if (child->parent_ == this)
                child->disableDownstream();
        }
    }

    /**
     * 返回当前节点候选子节点数量。
     *
     * 该接口只供 IController 的内部传播使用，不代表所有候选引用都已成为活动控制边。
     */
    [[nodiscard]] virtual std::size_t childCount() const noexcept = 0;

    /**
     * 返回指定下标的候选子节点。
     *
     * 调用者必须传入小于 childCount() 的下标。返回值为 non-owning 指针，候选引用不由
     * ControllerNode 所有或释放。
     */
    [[nodiscard]] virtual ControllerNode* childAt(std::size_t i) const noexcept = 0;

    template <std::size_t> friend class IController;
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
     * 传播并 acquireController() 成功后建立。调用者必须保证引用非空、无自身引用、无
     * 直接重复项，并保证整个候选图无环。
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

    /**
     * 使能当前节点及其候选子树。
     *
     * 该入口不可覆写。成功取得全局门后，完整传播、self* 钩子和回滚日志收尾都在同一
     * 事务中完成；竞争时不读取状态、不执行硬件动作，直接返回 Busy。
     */
    [[nodiscard]] Result enable() noexcept final
    {
        if (lifecycle_gate_.test_and_set(std::memory_order_acquire))
            return Result::Busy;

        Result result = Result::Ok;
        if (state_ == State::Disabled)
        {
            ControllerNode* rollback_head = nullptr;
            result                        = enableSubtree(rollback_head);
            if (result == Result::Ok)
                commitEnable(rollback_head);
            else
                rollbackEnable(rollback_head);
        }

        lifecycle_gate_.clear(std::memory_order_release);
        return result;
    }

    /**
     * 失能当前节点。
     *
     * 该入口不可覆写。reverse=false 只失能当前节点并释放直接子节点的控制权；reverse=true
     * 要求当前节点是根，并沿活动边关闭整个活动子树。竞争时立即返回 Busy。
     */
    [[nodiscard]] Result disable(bool reverse = false) noexcept final
    {
        if (lifecycle_gate_.test_and_set(std::memory_order_acquire))
            return Result::Busy;

        Result result = Result::InvalidState;
        if ((state_ == State::Protected || state_ == State::Standalone) &&
            (!reverse || parent_ == nullptr))
        {
            if (reverse)
                disableDownstream();
            else
                disableUpstream();
            result = Result::Ok;
        }

        lifecycle_gate_.clear(std::memory_order_release);
        return result;
    }

private:
    /// IController 的候选子节点数组；non-owning，释放活动边不会修改数组。
    [[nodiscard]] std::size_t     childCount() const noexcept final { return N; }
    [[nodiscard]] ControllerNode* childAt(std::size_t i) const noexcept final
    {
        return children_[i];
    }

    std::array<ControllerNode*, N> children_{};
};

} // namespace core::controller
