# Core

## IController

`Core::IController` defines the lifecycle shared by every layer of a hierarchical control chain. The public C++ namespace is `core::control`. It has no hardware, RTOS, or driver dependency.

`IController<N>` holds `N` non-owning candidate child references; `IController<>` is a leaf. The references describe possible control links, not active ownership: an edge exists only while `enable()` enables the child and still owns its control.

| State | Direct control | Meaning |
| --- | --- | --- |
| `Disabled` | rejected | Not enabled; holds no control relation. |
| `Protected` | rejected | Enabled and applying its own protection behavior. |
| `Standalone` | accepted | Enabled without an owner. |
| `Controlled` | rejected | Its control is owned by `currentController()`. |

`currentController()` is non-null exactly in `Controlled`, and a disabled node owns no children. The candidate references must form an acyclic graph and stay alive for the whole lifetime of their users.

### Lifecycle

`enable()` enables the node and its candidate subtree:

- already enabled nodes return `true` immediately and are not traversed again;
- children are processed in array order; the node then runs `selfEnable()` and `selfProtect()` before committing `Controlled` (with an owner) or `Protected` (as a root);
- an already enabled child that is not `Standalone` is only borrowed: it keeps its existing descendants, and a failed round only releases the edge acquired by that round;
- `Standalone` children are rejected, never taken over;
- the round is transactional: `selfEnable()` failure, an ownership conflict, a `Standalone` child, or a cancellation rolls back every change of the round. Nodes the round enabled are closed again; nodes it borrowed only lose the edge it acquired;
- `false` means the round failed, was cancelled, or the tree was already locked by another lifecycle operation. Nothing of that round is left applied, and callers may retry.

`disable(false)` disables the node itself, releases its directly owned children (they stay enabled and become protected roots), and then disables the controlling chain above it: each ancestor loses the children that do not belong to the chain the same way.

`disable(true)` (`disableTree()`) requires an owner-free node. It cancels the in-flight enables of the nodes it tears down and then disables the whole active subtree, parent before children. Candidate references that are not active edges are neither cancelled nor disabled.

`standalone()` moves an owner-free `Protected` node to `Standalone` and is idempotent; `protect()` moves `Standalone` back to `Protected`, applies `selfProtect()`, and is not idempotent.

### Disable priority

Every node carries a three-state lock (`Idle` / `Enabling` / `Cancelled`) that is both the enable round's ownership mark and its cancellation mark:

- a round locks its complete candidate subtree up front, self first, then advances top-down. `unlockTree()` releases the marks and reports whether any node was cancelled. `enable()` returns `true` only if the traversal committed and no node was cancelled; otherwise the round is rolled back even though parts of it had already committed;
- `disable()` never waits for a round: it cancels the node's own in-flight mark (and, along the chain it tears down or in the subtree it tears down, the marks of those nodes) and then performs the disable. A cancelled round cannot commit anything new after the mark is observed;
- boundary rule: a disable that happens **before** a node's lock is taken is ordered before that node's participation in the round, so the round may still enable it; a disable **after** the lock is taken cancels the round. A disable that completes before `enable()` starts does not affect it;
- guarantee is about the final state: the node that was disabled stays `Disabled` — even if the round had borrowed it — nodes the round newly enabled are closed, and borrowed nodes keep their own enablement. Since `enable()` is not executed inside a critical section, a node whose `selfEnable()` is already running can briefly switch its hardware back on before the cancellation is observed and corrected.

### Concurrency and hooks

- Lifecycle mutations of one node are serialized by its lock; there is no global gate, mutex, spin-wait, or interrupt masking. Independent trees do not contend with each other except through shared candidate nodes.
- `state()` and `currentController()` are intentionally unsynchronized: read them while lifecycle operations are quiescent or with external synchronization. Business `update()`/`setTarget()` operations are not serialized with the hooks either.
- `selfEnable()`, `selfDisable()`, and `selfProtect()` run inside the propagation path: they must be short, non-blocking, ISR-usable, and nonthrowing, and must not re-enter the lifecycle API or propagate ownership.
- A failed `selfEnable()` must clean up its own partial work; the framework calls `selfDisable()` only for successful enables.
- `selfDisable()` must be safe to call repeatedly and on a node that never observed the enable, because a disable may interrupt an enable in flight.
- Every `IController<N>` specialization shares the same `ControllerNode`; a shared candidate reached through two paths in one round is rejected cleanly (`enable()` returns `false`, nothing is applied, no lock is left behind).

### Verification

The header is a header-only `C++17` interface with no host dependency, so it was verified with a throwaway host harness (not committed) plus an `arm-none-eabi-g++` parse:

- host harness: 133 assertions covering basic enable/release/borrow behavior, failure rollback, ownership conflicts, `Standalone` rejection, shared candidates, and disable injection from inside `selfEnable()`/`selfDisable()` for in-flight rounds (`g++ -std=c++17 -Wall -Wextra -Werror -O2`, also under `-fsanitize=address,undefined` and with `clang++`);
- threaded stress: 200 000 racing `disable()` calls against 100 000+ `enable()`/`disableTree()` rounds on a 2-level tree, 0 invariant violations and no leaked locks (verified by a full round and teardown after the loop);
- `arm-none-eabi-g++ -std=c++17 -mcpu=cortex-m4 -mthumb -fno-exceptions -fno-rtti -Wall -Wextra -Werror=return-type -fsyntax-only` on a translation unit that instantiates concrete controller trees.

A hierarchy can therefore be assembled from the same base:

```text
IController<> MotorVelController
        -> IController<1> Trajectory
        -> IController<1> BigClass
```

The arrows represent ownership edges only while the corresponding nodes are enabled and the control has been successfully acquired. Domain-specific controllers keep their own periodic update phases and command policies; those APIs are not part of `IController` itself.
