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

namespace core::control
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
        Disabled,
        Protected,
        Standalone,
        Controlled
    };

    virtual bool enable() = 0;

    virtual bool disable(bool reverse) noexcept = 0;

    virtual bool standalone() noexcept = 0;

    virtual bool protect() noexcept = 0;

    [[nodiscard]] State state() const noexcept { return state_; }

    bool isEnabled() const noexcept { return state_ != State::Disabled; }
    bool isStandalone() const noexcept { return state_ == State::Standalone; }
    bool isProtected() const noexcept { return state_ == State::Protected; }
    bool isControlled() const noexcept { return state_ == State::Disabled; }

    ControllerNode* currentController() const noexcept { return parent_; }

protected:
    virtual bool selfEnable() { return true; }
    virtual void selfDisable() {}
    virtual void selfProtect() {}

private:
    ControllerNode();

    virtual bool enableAndAcquireController(ControllerNode* controller) noexcept = 0;

    virtual void rollbackLastEnablement() noexcept = 0;

    virtual bool lockTree() = 0;

    virtual void unlockTree() = 0;

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

    bool enable() noexcept final
    {
        if (isEnabled())
            return true;
        if (!lockTree())
            return false;
        const bool success = enableAndAcquireController(nullptr);
        unlockTree();
        return success;
    }

    bool disableTree() noexcept { return disable(true); }

    bool disable(const bool reverse = false) noexcept final
    {
        if (!isEnabled())
            return true;

        if (reverse)
        {
            // 只有根节点可以向下传递失能关系
            if (isControlled())
                return false;
            lockTree();
        }
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
    bool acquireControllerWithoutCheckingState(ControllerNode* parent) noexcept
    {
        if (parent_ == nullptr)
        {
            parent_ = parent;
            return true;
        }
        if (parent_ == parent)
            return true;
        return false;
    }

    bool enableAndAcquireController(ControllerNode* parent) noexcept final
    {
        // 如果我已经使能了，可以尝试 acquireController 并返回
        if (isEnabled())
        {
            enabled_before_last_ = true;
            return acquireControllerWithoutCheckingState(parent);
        }
        // 先向下传递使能关系
        for (int i = 0; i < N; ++i)
        {
            auto child = children_[i];
            if (!child->enableAndAcquireController(this))
            {
                // 对每个节点回滚使能关系
                for (int j = i - 1; j >= 0; --j)
                    children_[j]->rollbackLastEnablement();
                return false;
            }
        }
        if (!selfEnable())
        {
            for (int j = N - 1; j >= 0; --j)
                children_[j]->rollbackLastEnablement();
            return false;
        }
        selfProtect();
        if (parent != nullptr)
        {
            parent_ = parent;
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
        // 此时节点一定是锁定状态
        // 如果之前未使能，需要继续向下级回滚，并失能自身
        if (!enabled_before_last_)
        {
            for (auto& child : children_)
                child->rollbackLastEnablement();

            parent_ = nullptr;
            selfDisable();
            state_ = State::Disabled;
        }
        // 如果之前已使能，则只需要断开控制权，此时会进入保护态
        else
        {
            selfProtect();
            parent_ = nullptr;
            state_  = State::Protected;
        }
    }

    bool lockTree() noexcept final
    {
        for (int i = 0; i < N; ++i)
        {
            if (!children_[i]->lockTree())
            {
                for (int j = i - 1; j >= 0; --j)
                    children_[j]->unlockTree();
                return false;
            }
        }
        if (!enable_lock_.lock())
        {
            for (int j = N - 1; j >= 0; --j)
                children_[j]->unlockTree();
            return false;
        }
        return true;
    }

    void unlockTree() noexcept final
    {
        enable_lock_.unlock();
        for (auto& child : children_)
            child->unlockTree();
    }

    std::array<ControllerNode*, N> children_{};

    // 节点锁，该锁由父节点管理
    AtomicFlagLock enable_lock_{};

    bool enabled_before_last_{ false };
};

}; // namespace core::control