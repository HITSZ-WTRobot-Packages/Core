# IController

`Core::IController` provides a header-only C++17 lifecycle interface for hierarchical control chains in the `core::control` namespace. It depends on `libs::Concurrency` for nonblocking lifecycle synchronization and has no hardware, RTOS, or driver dependency. The package does not schedule periodic updates or define application command policies.

Use the package through cpkg and include `IController.hpp`. The target must provide always-lock-free atomic controller pointers, as required by the header's compile-time assertion. Shared-repository STM32 family coverage is not a per-target compatibility guarantee.

## Composing a control chain

Derive controllers from `IController<N>` and supply an array of `N` candidate child pointers; `IController<>` is a leaf. For example, a chain can be composed as:

```text
RootController : IController<1>
    -> IntermediateController : IController<1>
        -> LeafController : IController<>
```

These links describe candidates, not object ownership. Construction does not acquire control: an active edge exists only after successful enablement and control acquisition. The caller owns all objects and must ensure that:

- candidate references are non-null, contain no self-reference or direct duplicate, and form an acyclic graph;
- candidate objects outlive every controller referring to them, and every node outlives all lifecycle operations and concurrent accessor reads;
- nodes are disabled before destruction; the base destructor asserts this requirement;
- an operation does not reach the same candidate through multiple paths. Such a topology is rejected as a conflict, even if it has no cycle; retrying the unchanged graph cannot resolve it.

Candidate references remain fixed. Active ownership can change without changing the candidate topology. Lifecycle traversal uses stack proportional to candidate depth, so the caller must budget stack for the chosen hierarchy; the interface does not allocate a dynamic lock list or require an RTOS mutex.

## Lifecycle and failure contract

| State | Direct control | Meaning |
| --- | --- | --- |
| `Disabled` | rejected | Not enabled; holds no control relation. |
| `Protected` | rejected | Enabled and applying its own protection behavior. |
| `Standalone` | accepted | Enabled without an owner. |
| `Controlled` | rejected | Control belongs to `currentController()`. |

These are logical permissions; application command APIs must respect them. Outside an in-progress lifecycle transaction, `currentController()` is non-null exactly in `Controlled`, and a disabled node owns no children.

Enablement is transactional. A newly enabled root finishes in `Protected`, and an acquired child finishes in `Controlled`. An already enabled node can return success without rebuilding ownership, but even this idempotent call can fail under contention because its candidate subtree must be available. A `Standalone` child is never taken over. A child already owned by another controller also prevents acquisition.

On enablement failure, all changes made by that round are rolled back: nodes newly enabled by the round are disabled again, while previously enabled subtrees that were borrowed retain their descendants and only lose the edge acquired by the failed round. A `false` result can mean enable-hook failure, ownership or topology conflict, a `Standalone` candidate, or lock contention; it is not solely a transient-busy indication. Retrying is appropriate only after the relevant cause has been resolved.

There are two distinct disable directions:

- `disable(false)` disables the receiver and its active ancestors. It releases their direct child ownership, leaving released children enabled as protected roots with their descendants unchanged.
- `disable(true)`, also exposed as `disableTree()`, requires an owner-free receiver; a controlled receiver returns `false`. It disables the active ownership subtree, parent before children. Inactive candidate references remain unchanged.

Both directions first require exclusive lifecycle access to the entire containing candidate tree, including inactive candidates and sibling branches. If this preflight encounters contention or a topology conflict, it returns `false` without invoking disable hooks or changing state or ownership. It neither waits for nor cancels a competing operation. A successful result means the requested propagation has completed, not that every candidate node has been disabled.

Direct-control permission is entered from an owner-free `Protected` state with `standalone()`; an already `Standalone` node can succeed idempotently. This transition changes only logical permission: it does not enable hardware or submit application commands. `protect()` returns a `Standalone` node to `Protected` after applying protection. Invalid source states and contention return `false`; `protect()` is not an idempotent success when already protected.

## Concurrency and derived-controller obligations

The documented concurrency model is single-core task/ISR preemption. Lifecycle mutations are nonblocking: conflicting operations return `false` rather than waiting. Enablement covers its candidate subtree, disablement covers the containing candidate tree, and permission transitions cover the receiver. Consequently an inactive candidate may still cause contention, and even operations on different active branches can conflict.

`currentController()` is an atomic point-in-time ownership snapshot. During a lifecycle transaction it may reflect either side of the transition; it is not a coherent snapshot with a separate `state()` call and does not extend the returned object's lifetime. Snapshot reads are not a substitute for synchronization of application commands, shared resources, or object destruction.

Derived lifecycle hooks execute synchronously in the caller's task or ISR context, with the node's lifecycle lock held but interrupts not masked. Essential obligations are:

- Keep execution and stack use bounded and short. Do not sleep, block, busy-wait for another context, throw exceptions, or rely on non-ISR-safe or potentially blocking allocation or I/O.
- Independently synchronize shared hardware, buses, DMA, global data, and periodic control/ISR accesses. The lifecycle locks do not protect these application resources automatically.
- Never reenter any node's lifecycle APIs, including through synchronous callbacks; do not invoke another node's hooks, change candidate topology, propagate control relationships yourself, or destroy related nodes from a hook.
- Support rollback, repeated disable/protection, and later re-enablement. A failed `selfEnable()` must undo its own local side effects: the framework does not call `selfDisable()` for that failed hook. A successful enable must remain safely reversible if an ancestor later fails.
- Complete or reliably submit safe disable/protection actions before returning. These hooks have no failure return channel; asynchronous work must not allow old enable/control requests to reactivate outputs after disable. Do not wait for another task or completion interrupt while holding the lifecycle lock.
- Do not use intermediate state or ownership snapshots to skip required safety actions or assume that the whole enablement transaction has committed.

The existing [Doxygen in `IController.hpp`](IController.hpp) is the reference for exact symbol contracts, hook preconditions, and lifecycle implementation details. No separate function-by-function API or lock-algorithm manual is maintained here.

[Repository overview](../README.md)
