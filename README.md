# Core

## IController

`Core::IController` defines the lifecycle shared by every layer of a hierarchical control chain. The public C++ namespace is `core::control`. It uses `libs::Concurrency` for nonblocking node locks and has no hardware, RTOS, or driver dependency.

`IController<N>` holds `N` non-owning candidate child references; `IController<>` is a leaf. The references describe possible control links, not active ownership: an edge exists only while `enable()` enables the child and still owns its control.

| State | Direct control | Meaning |
| --- | --- | --- |
| `Disabled` | rejected | Not enabled; holds no control relation. |
| `Protected` | rejected | Enabled and applying its own protection behavior. |
| `Standalone` | accepted | Enabled without an owner. |
| `Controlled` | rejected | Its control is owned by `currentController()`. |

Outside an in-progress lifecycle transaction, `currentController()` is non-null exactly in `Controlled`, and a disabled node owns no children. The candidate references must form an acyclic graph and stay alive for the whole lifetime of their users.

`currentController()` performs a lock-free atomic read and returns a point-in-time ownership snapshot. A
concurrent lifecycle transaction may make that snapshot represent either side of the transition; it does
not form a coherent pair with a separate `state()` call and does not extend the controller's lifetime.
Every node must therefore outlive all lifecycle operations and concurrent accessor reads.

### Lifecycle

`enable()` enables the node and its candidate subtree:

- every call first locks its complete candidate subtree, never its ancestors; already enabled nodes then
  return `true` without rebuilding ownership; contention anywhere in that subtree returns `false`;
- children are processed in array order; the node then runs `selfEnable()` and `selfProtect()` before
  committing `Controlled` (with an owner) or `Protected` (as a root);
- an already enabled child that is not `Standalone` is only borrowed: it keeps its existing descendants,
  and a failed round only releases the edge acquired by that round;
- `Standalone` children are rejected, never taken over;
- the round is transactional: `selfEnable()` failure, an ownership conflict, a `Standalone` child, or
  lock contention rolls back every change of the round. Nodes the round enabled are closed again; nodes
  it borrowed only lose the edge it acquired;
- `false` means the round failed or the tree was already locked by another lifecycle operation. Nothing
  of that round is left applied, and callers may retry.

Both disable directions first try-lock the entire containing candidate tree. The receiver and its active
`parent_` chain are locked first to stabilize the root, then the remaining candidate subtrees at every
level are locked. Inactive candidate references participate in locking too, but are not disabled.

`disable(false)` disables the receiver and each ancestor in the active chain, releasing direct child
ownership while leaving every released child enabled as a protected root with its descendants unchanged.

`disable(true)` (`disableTree()`) requires an owner-free node; a controlled receiver returns `false`.
After the complete candidate tree is locked, it disables only the active ownership subtree, parent before
children. Candidate references that are not active edges remain unchanged.

For either direction, an occupied node makes the preflight return `false`; every lock acquired by that
attempt is released and no hook, state write, parent change, or control-edge release occurs. The operation
does not wait for or cancel the competing enable, `standalone()`, or `protect()`; callers can retry after
the competing operation completes.

`standalone()` moves an owner-free `Protected` node to `Standalone` and is idempotent; `protect()` moves
`Standalone` back to `Protected` and applies `selfProtect()`. Both are node-scoped lifecycle mutations:
they take the node's lock and are refused with `false` while another lifecycle operation holds that node.

### Disable locking

Every node carries an `AtomicFlagLock`, held by an enable round, a permission transition, or a disable
preflight. All lifecycle operations use the same nonblocking try-lock rule: a failed acquisition returns
`false` and never waits or takes over the existing owner. There is no per-node acquisition-list field:
each recursive frame tracks its successfully locked child prefix. `disable()` keeps the locked root in
a local pointer, then unlocks through immutable candidate references after teardown, even though
`parent_` has changed. No dynamic allocation or RTOS mutex is needed; recursion uses stack proportional
to candidate depth, not a stack array of all acquired nodes.

- enable locks only its complete candidate subtree self first, including idempotent calls, and releases
  the whole transaction range after commit or rollback;
- disable preflights the whole tree for either direction before running any teardown. The tree remains
  locked throughout teardown, so state and ownership writes are performed by the holder;
- a failed preflight recursively releases only successfully locked child prefixes and, for disable,
  already locked ancestor ranges, without unlocking the failed child or any other operation's locks;
  the entire graph is unchanged, and retry is possible after the competing operation completes;
- the root is found through active `parent_` edges, not candidate references; locking then covers every
  candidate descendant from that root, including inactive references and sibling descendants;
- only the mutation direction differs: `reverse=false` follows active edges upward and releases direct
  child ownership; `reverse=true` follows active edges downward from an owner-free receiver.
- a candidate reached twice through different paths in one attempt is rejected in either direction,
  with all acquired locks released. This is a candidate-topology conflict, not transient contention;
  retrying the same graph cannot resolve it.

### Verification

The header is a header-only C++17 interface with no host dependency. Verification uses throwaway host
harnesses plus compile sweeps because no permanent lifecycle test target is committed in this package.

- lifecycle harness: leaf enable/disable, upstream release preserving enabled child subtrees, full active
  subtree disable, controlled-node rejection, inactive-candidate preservation, self-enable failure rollback,
  and repeated enable/disable cycles;
- range-lock harness: occupied receiver, ancestor, direct child, and descendant cases all return `false`
  with no hook/state/edge side effect; a preflight that acquired earlier nodes releases those locks before
  returning; retry succeeds after the competing operation releases its node;
- deterministic threaded scenarios: a paused lifecycle hook makes competing operations return `false`
  only when their lock ranges overlap; retries succeed after the competing operation releases its locks;
- recursive-rollback scenarios: a busy late candidate descendant leaves earlier sibling subtrees usable,
  repeated failed attempts never release the busy node, and successful teardown can be followed by
  re-enabling the same tree after its active parent links were cleared;
- compile sweep over translation units instantiating concrete leaf and multi-child trees with
  `g++ -std=c++17 -Wall -Wextra -Werror -Wshadow -Wconversion`, `clang++ -std=c++17 -Wall -Wextra -Werror`,
  and, when available, `arm-none-eabi-g++ -std=c++17 -mcpu=cortex-m4 -mthumb -fno-exceptions -fno-rtti
  -Wall -Wextra -Werror -fsyntax-only`.

The harnesses emulate ISR/task preemption with coordinated threads, without reentering lifecycle APIs
from hooks. Real failure rates on a target RTOS are not measured.

A hierarchy can therefore be assembled from the same base:

```text
IController<> MotorVelController
        -> IController<1> Trajectory
        -> IController<1> BigClass
```

The arrows represent ownership edges only while the corresponding nodes are enabled and the control has been successfully acquired. Domain-specific controllers keep their own periodic update phases and command policies; those APIs are not part of `IController` itself.
