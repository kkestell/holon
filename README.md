# Holon

Holon is a deterministic virtual machine that unifies program execution and persistence. Holon stores an application's executable code and durable state together as a single immutable value called an object.

The system operates as a deterministic state machine. Given the current object and an event, a transition computes the next object and a tuple of effects describing work for the host to perform:

```text
(object, event) -> (next_object, effects)
```

An object packages its vtable, a tuple of methods, alongside its state: `[vtable, state]`. Because a transition returns an entirely new object, it can update the state, the vtable, or both. Code upgrades and state migrations happen together in one atomic transition.

Before any effects are delivered to the outside world, the storage engine commits the next object and its effects to disk together. Reopening a store after a restart or crash restores the latest committed code, state, and any unacknowledged effects.

## One value model for code and data

Every datum in Holon is an immutable, acyclic value, which is one of two kinds:

- **Blob:** A finite byte array.
- **Tuple:** A finite, ordered array of values.

Scalars such as 64-bit unsigned integers (`u64`) and booleans are encoded as 8-byte blobs, and paths as blobs of `u64` indices. Data structures, vtables, objects, and expressions are built from nested tuples and blobs.

Because values are immutable, modifying a structure produces a new value, much like persistent data structures in functional programming. An implementation may share unchanged subtrees in memory or on disk as a directed acyclic graph (DAG), but sharing is invisible to programs. Equality (`eq`) is purely structural.

Code uses the same representation. An expression is a tuple whose first element is a `u64` opcode and whose remaining elements are its operands:

```text
[alu, add, [get, path[0]], [get, path[1]]]
```

In Holon's text notation, brackets (`[...]`) denote tuples, while strings (`"..."`), hex literals (`x"..."`), integers, booleans (`true`, `false`), opcode and ALU names (`alu`, `add`), and `path[...]` literals all denote blobs. Expanded, the expression is an abstract syntax tree (AST) of tuples with blobs at the leaves:

```text
tuple
├── blob        u64(alu)
├── blob        u64(add)
├── tuple
│   ├── blob    u64(get)
│   └── blob    path[0]
└── tuple
    ├── blob    u64(get)
    └── blob    path[1]
```

This expression reads elements `0` and `1` of the tuple it is evaluated against and adds them. Because code is ordinary data—a property known as homoiconicity—programs can construct, inspect, store, and evaluate expressions at runtime with the same opcodes they use for data.

## Point-free evaluation

Holon uses a combinator-style evaluator: evaluation is a pure mathematical function of two values, a subject and an expression.

```text
eval(subject, expression) -> Result<Value, Trap>
```

The subject is the only value the expression can read. Expressions address data by structural position within the subject. Holon defines 17 opcodes that transform or inspect the current subject:

- **Navigation and functional updates:** `get` traverses a static path of indices into the subject to read a value; `set` returns a new copy of the subject with the value at that path replaced. `get_at`, `set_at`, `slice`, and `concat` provide dynamic indexing, slicing, and concatenation.
- **Sequencing:** `pipe` evaluates a first expression against the current subject, then uses the result as the *new subject* for a second expression.
- **Control flow and dynamic code:** `if` evaluates a boolean condition and branches lazily. `eval` computes a subject and an expression at runtime and evaluates the expression against the subject.
- **Computation:** `alu` performs `u64` arithmetic, bitwise logic, and comparisons. `eq`, `kind`, `len`, `quote`, `tuple`, `call`, and `trap` handle comparison, introspection, construction, invocation, and error signaling.

Evaluation is strictly deterministic and referentially transparent. Given the same subject and expression, `eval` always returns the same value, traps with the same reason, or diverges (loops forever). An expression reads only its subject, so timestamps, random seeds, and network payloads arrive as part of the event.

To support deep recursion, the VM evaluates tail calls across `if`, `pipe`, `call`, and `eval` in constant stack space.

## Objects, method calls, and hot upgrades

By convention, an object is a two-element tuple:

```text
[vtable, state]
```

`vtable` is a tuple of method expressions, and `state` is any value. Invoking a method with `call` constructs a temporary pair `[object, args]` and passes it as the subject to the selected method. The method reads each part of the subject with `get` and the path shown:

```text
subject                 path[]
├── object              path[0]
│   ├── vtable          path[0, 0]
│   │   ├── method 0    path[0, 0, 0]
│   │   ├── method 1    path[0, 0, 1]
│   │   └── ...
│   └── state           path[0, 1]
└── args                path[1]
```

A `call` can target any method index and return any value. A transition always invokes the transition method, method `0`, on the root object (the object held in the store), with the event as `args`.

To succeed, the transition method must return a two-element tuple `[next_object, effects]`, where `next_object` is a valid object with a nonempty `vtable`, and `effects` is a tuple of values for the host to deliver.

For example, here is a counter object initialized with state `2`:

```text
[
  [
    # Method 0: add the event (path[1]) to the state (path[0, 1]),
    # then return [next_object, [next_state]].
    [pipe,
      [set, path[0, 1], [alu, add, [get, path[0, 1]], [get, path[1]]]],
      [tuple, [get, path[0]], [tuple, [get, path[0, 1]]]]]
  ],
  2
]
```

When this object receives an event of `3`, `set` produces an updated subject where `object`'s state at `path[0, 1]` is `5`, and `pipe` feeds that updated subject to `tuple` to return `[[vtable, 5], [5]]`.

Because `next_object` includes the `vtable`, an application upgrades itself by returning a new `vtable` alongside its migrated `state` in the transition result. If evaluation traps—for instance, due to an out-of-bounds index or type mismatch during migration—the entire transition is aborted, no effects are emitted, and the previous object remains intact.

## Atomic persistence and the transactional outbox

Holon persists the root object and its outbox together as a snapshot in a file called a store. The outbox holds effects waiting to be acknowledged.

When a transition succeeds, the storage engine atomically replaces the snapshot with the next object and the updated outbox, including the new effects. After a crash, the store contains either the previous snapshot or the new one. Effects become eligible for delivery only after their snapshot is durable on disk. This is the transactional outbox pattern: the next object and its effects commit together.

Each committed effect has a stable effect ID. Once the host delivers an effect to an external system and receives confirmation, it acknowledges the effect by committing a snapshot with that effect removed from the outbox.

After a restart, the host redelivers every effect still in the outbox. A crash can occur after delivery but before acknowledgment, so delivery is at-least-once. Recipients use the effect ID to deduplicate repeated deliveries.

The `holon` CLI drives this lifecycle with four commands—`init`, `send`, `inspect`, and `drain`:

```sh
$ holon init counter.store counter.holon
2
$ holon send counter.store 3
5
$ holon inspect counter.store
5
$ holon drain counter.store
effect: 9f86d081884c7d659a2feaa0c55ad015:2:0 5
```

## Examples

The `examples` directory holds small Holon programs. Build `holon` with `make` and run them from the repository root.

`counter.holon` adds each `u64` event to a running total. `upgrade.holon` is a vtable that, sent to the counter as an event, upgrades it to track `[total, count]`:

```sh
$ holon init counter.store examples/counter.holon
0
$ holon send counter.store 5
5
$ holon send counter.store "$(cat examples/upgrade.holon)"
[5, 0]
$ holon send counter.store 3
[8, 1]
$ holon drain counter.store
effect: eeaf1f78001d0e7f33e92d46c2914f4e:2:0 5
effect: eeaf1f78001d0e7f33e92d46c2914f4e:3:0 "upgraded"
effect: eeaf1f78001d0e7f33e92d46c2914f4e:4:0 [8, 1]
```

`gcd.holon` computes the greatest common divisor of each event `[a, b]` with Euclid's algorithm:

```sh
$ holon init gcd.store examples/gcd.holon
0
$ holon send gcd.store '[1071, 462]'
21
$ holon drain gcd.store
effect: e7d4f8a0f68d4b51156f35a5a91873a5:2:0 21
```

`fizzbuzz.holon` continues a FizzBuzz count by the number in each event and emits one effect per number:

```sh
$ holon init fizzbuzz.store examples/fizzbuzz.holon
0
$ holon send fizzbuzz.store 5
5
$ holon drain fizzbuzz.store
effect: fd52b3b08a451a18600baba4836b92fc:2:0 1
effect: fd52b3b08a451a18600baba4836b92fc:2:1 2
effect: fd52b3b08a451a18600baba4836b92fc:2:2 "Fizz"
effect: fd52b3b08a451a18600baba4836b92fc:2:3 4
effect: fd52b3b08a451a18600baba4836b92fc:2:4 "Buzz"
```

`turnstile.holon` is a coin-operated turnstile that accepts `"coin"` and `"push"` events and emits `"unlock"` or `"lock"` when its state changes:

```sh
$ holon init turnstile.store examples/turnstile.holon
"locked"
$ holon send turnstile.store '"coin"'
"unlocked"
$ holon send turnstile.store '"push"'
"locked"
$ holon drain turnstile.store
effect: 6b741645f409246af4db92902db19feb:2:0 "unlock"
effect: 6b741645f409246af4db92902db19feb:3:0 "lock"
```

`bank.holon` keeps an account balance and emits a receipt for each deposit and withdrawal. A withdrawal larger than the balance traps, so the store keeps its previous balance and outbox:

```sh
$ holon init bank.store examples/bank.holon
0
$ holon send bank.store '["deposit", 100]'
100
$ holon send bank.store '["withdraw", 30]'
70
$ holon send bank.store '["withdraw", 100]'
trap: "insufficient funds"
$ holon drain bank.store
effect: e50843f5f689ce9f4ed2f34ac8e8654c:2:0 ["deposit", 100, 100]
effect: e50843f5f689ce9f4ed2f34ac8e8654c:3:0 ["withdraw", 30, 70]
```

## Opcodes

Opcodes are numbered from `0` in the order below. The `value` of `quote`, the `path` of `get` and `set`, and the `operation` of `alu` are literals. All other operands are expressions, evaluated against the current subject unless the description says otherwise.

| # | Expression | Behavior |
| --- | --- | --- |
| 0 | `[quote, value]` | Returns `value`. |
| 1 | `[tuple, a, b, ...]` | Returns a tuple of the operands' results. Takes any number of operands. |
| 2 | `[kind, value]` | Returns `0` for a blob and `1` for a tuple. |
| 3 | `[len, value]` | Returns the number of elements in a tuple or bytes in a blob. |
| 4 | `[get, path]` | Returns the value at `path` in the subject. |
| 5 | `[set, path, replacement]` | Returns the subject with the value at `path` replaced by `replacement`. |
| 6 | `[get_at, value, index]` | Returns the element at `index` of a tuple, or the byte at `index` of a blob as a 1-byte blob. |
| 7 | `[set_at, tuple, index, replacement]` | Returns `tuple` with the element at `index` replaced by `replacement`. |
| 8 | `[slice, value, start, count]` | Returns `count` elements or bytes of `value`, beginning at `start`. |
| 9 | `[concat, first, second]` | Joins two tuples or two blobs. |
| 10 | `[if, condition, then, else]` | Evaluates `then` if `condition` is `1` and `else` if it is `0`. |
| 11 | `[pipe, first, second]` | Evaluates `second` with the result of `first` as its subject. |
| 12 | `[call, object, index, args]` | Evaluates method `index` of `object`'s vtable with the subject `[object, args]`. |
| 13 | `[eval, subject, expression]` | Evaluates the result of `expression` against the result of `subject`. |
| 14 | `[eq, first, second]` | Returns `1` if the values are structurally equal and `0` otherwise. |
| 15 | `[alu, operation, a, b]` | Applies `operation` to two `u64`s. |
| 16 | `[trap, reason]` | Traps with `reason`. |

ALU operations are numbered from `0` in this order: `add`, `sub`, `mul`, `div`, `mod`, `and`, `or`, `xor`, `shl`, `shr`, `lt`, `le`. Arithmetic wraps modulo 2^64, shifts by 64 or more return `0`, `div` and `mod` trap on a zero divisor, and `lt` and `le` return booleans.

## Glossary

- **Acknowledge:** To durably remove a delivered effect from the outbox.
- **ALU operation:** An arithmetic, bitwise, or comparison operation selected by an `alu` expression.
- **`args`:** The arguments passed to a method, paired with its object as `[object, args]`.
- **AST:** Abstract syntax tree. Every expression is an AST of tuples with blobs at the leaves.
- **Blob:** A finite byte array; one of the two kinds of value.
- **Boolean:** A `u64` that is `0` (false) or `1` (true).
- **Commit:** To durably replace the store's snapshot.
- **Effect:** A value returned by a transition describing a side effect for the host to perform.
- **Effect ID:** A stable identifier that lets recipients recognize repeated deliveries of an effect.
- **Event:** A value carrying outside input to the root object's transition method.
- **Expression:** A tuple whose first element is an opcode and whose remaining elements are its operands.
- **Host:** The software that runs Holon: it submits events, delivers effects to external systems, and acknowledges them.
- **Kind:** Whether a value is a blob or a tuple.
- **Method:** An expression in a vtable, evaluated with the subject `[object, args]`.
- **Object:** A two-element tuple `[vtable, state]`.
- **Opcode:** The `u64` at the head of an expression that selects its operation; see [Opcodes](#opcodes).
- **Operand:** An element of an expression after the opcode.
- **Outbox:** The snapshot's tuple of committed, unacknowledged effects.
- **Path:** A sequence of tuple indices locating a value within the subject, written `path[...]`.
- **Root object:** The object held in the store, whose transition method processes events.
- **Sequence:** The snapshot's transition counter.
- **Snapshot:** The stored value containing the root object, outbox, and persistence metadata.
- **State:** The second element of an object; any value.
- **Storage engine:** The part of Holon that commits snapshots to the store and loads them from it.
- **Store:** The file that holds the current snapshot.
- **Store ID (`store_id`):** A random identifier assigned when a store is created.
- **Subject:** The value an expression is evaluated against, and the only value it can read.
- **Transition:** One step of the state machine, processing an event to produce the next root object and effects.
- **Transition method:** Method `0` of the root object's vtable, responsible for handling events.
- **Trap:** An evaluation error that aborts the current evaluation.
- **Tuple:** A finite, ordered array of values; one of the two kinds of value.
- **`u64`:** An unsigned 64-bit integer, stored as an 8-byte little-endian blob.
- **Upgrade:** A transition that replaces the application's code by returning a new vtable.
- **Value:** An immutable, acyclic blob or tuple. All code and data in Holon are values.
- **Vtable:** The first element of an object: a tuple of methods, indexed by `call`.
