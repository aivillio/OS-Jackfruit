# Engine Explanations Log

This file tracks step-by-step explanations for `engine.c` as we work.

## 1) Data Types in `engine.c`

### `command_kind_t`
Represents which control/CLI action is requested.
- `CMD_SUPERVISOR`: start supervisor mode
- `CMD_START`: start a container
- `CMD_RUN`: run a container command
- `CMD_PS`: list containers
- `CMD_LOGS`: fetch logs
- `CMD_STOP`: stop a container

### `container_state_t`
Represents lifecycle status of a container.
- `CONTAINER_STARTING`
- `CONTAINER_RUNNING`
- `CONTAINER_STOPPED`
- `CONTAINER_KILLED`
- `CONTAINER_EXITED`

### `container_record_t`
Linked-list metadata node for one container.
- `id`: container ID
- `host_pid`: host PID
- `started_at`: start timestamp
- `state`: current lifecycle state
- `soft_limit_bytes`, `hard_limit_bytes`: monitor limits
- `exit_code`, `exit_signal`: process termination details
- `log_path`: per-container log file path
- `next`: pointer to next list node

### `log_item_t`
One queued log chunk.
- `container_id`: source container
- `length`: valid bytes in `data`
- `data`: raw log bytes

### `bounded_buffer_t`
Thread-safe ring buffer used between log producers and consumer thread.
- `items`: fixed queue storage
- `head`: next write index
- `tail`: next read index
- `count`: number of queued items
- `shutting_down`: stop flag for graceful teardown
- `mutex`: protects shared queue state
- `not_empty`: consumers wait here when empty
- `not_full`: producers wait here when full

### `control_request_t`
Message from CLI client to supervisor.
- `kind`: command type
- `container_id`: target container
- `rootfs`: container root filesystem
- `command`: command to run
- `soft_limit_bytes`, `hard_limit_bytes`: memory limits
- `nice_value`: scheduling nice value

### `control_response_t`
Message from supervisor back to client.
- `status`: success/failure code
- `message`: descriptive response text

### `child_config_t`
Parameters passed into clone child setup logic.
- `id`, `rootfs`, `command`
- `nice_value`
- `log_write_fd`: where child output is written

### `supervisor_ctx_t`
Top-level supervisor runtime context.
- `server_fd`: control endpoint fd
- `monitor_fd`: `/dev/container_monitor` fd
- `should_stop`: event-loop stop flag
- `logger_thread`: log consumer thread
- `log_buffer`: bounded log queue
- `metadata_lock`: protects container metadata
- `containers`: linked-list head for `container_record_t`

## 2) What "shutdown" Means in the Buffer

In this context, shutdown means the bounded buffer is entering teardown and should stop accepting/waiting for new work.

`bounded_buffer_begin_shutdown()` sets `shutting_down = 1`, broadcasts both condition variables, and allows blocked producers/consumers to wake and exit cleanly.

## 3) Condition Logic in `bounded_buffer_push` and `bounded_buffer_pop`

### `bounded_buffer_push(buffer, item)`
- Lock mutex
- If `shutting_down`, return `-1`
- While full (`count >= LOG_BUFFER_CAPACITY`), wait on `not_full`
- Re-check shutdown while waiting
- Enqueue at `head`, advance circularly, increment `count`
- Signal `not_empty`
- Unlock and return `0`

### `bounded_buffer_pop(buffer, item)`
- Lock mutex
- While empty and not shutting down (`count == 0 && !shutting_down`), wait on `not_empty`
- If empty and shutting down, return `-1` (drained + done)
- Dequeue from `tail`, advance circularly, decrement `count`
- Signal `not_full`
- Unlock and return `0`

### Why `while` for waits
Condition variable waits use `while` (not `if`) to handle spurious wakeups and to re-check state after wakeups.

## 4) What the Linked List Contains

The linked list stores per-container metadata records.

- The head pointer is `containers` inside `supervisor_ctx_t`.
- Each node is a `container_record_t`.

Each `container_record_t` node contains:
- `id`: container ID string
- `host_pid`: PID on the host
- `started_at`: container start timestamp
- `state`: current lifecycle state
- `soft_limit_bytes`, `hard_limit_bytes`: memory limits sent to monitor
- `exit_code`, `exit_signal`: termination details
- `log_path`: path to that container's log file
- `next`: pointer to the next node

So conceptually, the list is the supervisor's in-memory table of all containers it is tracking.

## 5) Logging Consumer Thread (`logging_thread`)

### What it does
- Runs in a dedicated thread started by the supervisor.
- Repeatedly calls `bounded_buffer_pop(&ctx->log_buffer, &item)`.
- For each popped `log_item_t`, finds the matching container by ID in `ctx->containers` under `metadata_lock`.
- Uses the container's `log_path` when available, otherwise falls back to `logs/<container_id>.log`.
- Opens the log file in append mode and writes the chunk bytes.
- Keeps running until `bounded_buffer_pop` returns `-1`, which means shutdown has begun and the queue is fully drained.

### Why this thread exists
- Separates log I/O from the rest of the runtime logic.
- Prevents producer-side code from blocking on slow disk writes.
- Preserves ordering through the bounded queue while still enabling concurrency.
- Gives clean shutdown semantics: drain queued logs first, then exit.

### Shutdown behavior
- Producers stop accepting new entries once `shutting_down` is set.
- The consumer continues draining existing queued entries.
- After queue becomes empty during shutdown, pop returns `-1` and the thread exits cleanly.

## 6) Clone Child Entrypoint (`child_fn`)

### What it now does
- Validates input config (`rootfs` and `command` must be present).
- Applies process priority (`setpriority`) using `nice_value` when non-zero.
- Redirects `stdout` and `stderr` to `log_write_fd` so child output goes to supervisor logging flow.
- Redirects `stdin` to `/dev/null`.
- Marks mounts private (`MS_REC | MS_PRIVATE`) so mount changes do not propagate outside container context.
- Sets hostname to container ID (or fallback `container`).
- Enters the container filesystem using `chdir(rootfs)` + `chroot(".")` + `chdir("/")`.
- Ensures `/proc` exists and mounts procfs at `/proc`.
- Executes configured command via `/bin/sh -c <command>`.

### Why each required outcome is satisfied
- Isolated mount/UTS behavior: private mounts + hostname set inside container context.
- Rootfs isolation: `chroot` changes filesystem root seen by the child.
- Working `/proc`: procfs mounted after entering rootfs.
- Output routing: `dup2` redirects standard output/error to logging fd.
- Command execution: final `execl` replaces child process with requested command.

Note: PID namespace isolation is controlled by the `clone(...)` flags used by the parent when launching this child entrypoint.

## 7) Long-Running Supervisor (`run_supervisor`)

### What was implemented
- Initializes runtime context already present (`metadata_lock`, bounded buffer).
- Creates `logs/` directory if missing.
- Attempts to open `/dev/container_monitor` for ioctl registration.
- Creates and binds UNIX control socket at `/tmp/mini_runtime.sock`.
- Installs signal handlers:
  - `SIGINT` / `SIGTERM` request shutdown.
  - `SIGCHLD` requests child reaping.
- Starts `logger_thread`.
- Enters an event loop that:
  - polls control socket for client requests,
  - handles `start` / `run` / `ps` / `logs` / `stop`,
  - drains child log pipes into bounded buffer,
  - reaps exited children and updates container states.
- On shutdown:
  - sends `SIGTERM` to running containers,
  - drains/reaps remaining child state,
  - shuts down and joins logger thread,
  - closes sockets/device fds,
  - frees container metadata and child stacks.

### Metadata/state behavior
- `start` / `run` now clone a child with PID/UTS/mount namespaces and insert a `container_record_t`.
- `stop` sends SIGTERM and marks state as stopped.
- Reap path updates records to exited/killed + exit code/signal.

### Current limitation
- `send_control_request()` (client side) is still TODO, so external CLI commands will still print "Control-plane client path not implemented" until that TODO is implemented.

## 8) Supervisor Simplification Pass

`run_supervisor` was simplified to keep behavior but reduce complexity.

### What changed
- Converted repeated error branches into a single `cleanup:` path.
- Added small init-state flags:
  - `metadata_inited`
  - `buffer_inited`
  - `logger_started`
  - `socket_bound`
- Kept runtime behavior unchanged (same setup, loop, and shutdown steps).

### Why this is simpler
- One cleanup path is easier to reason about than many duplicated branches.
- Fewer chances to forget releasing a resource on an early return.
- The happy path is now easier to read top-to-bottom.

## 9) Client Control Request Path (`send_control_request`)

The client-side TODO is now implemented.

### What it does
- Validates input request pointer.
- Opens a UNIX domain socket client.
- Connects to `/tmp/mini_runtime.sock`.
- Sends full `control_request_t` to supervisor.
- Reads full `control_response_t` back.
- Prints supervisor response message:
  - success (`status == 0`) to stdout
  - failure to stderr
- Returns `0` on success and `1` on failure.

### Result
CLI paths (`start`, `run`, `ps`, `logs`, `stop`) now have a functional client transport to the running supervisor.

## 10) PS Metadata Response

The `CMD_PS` path in supervisor now returns real container metadata instead of only aggregate counts.

### Response format
Each container contributes a compact entry:

`<id>(pid=<pid>,state=<state>,exit=<code>,sig=<signal>,start=<epoch>)`

Entries are joined with `; `.

### Behavior details
- If there are no containers: response is `no containers`.
- If the response exceeds `CONTROL_MESSAGE_LEN`: it is truncated safely and ends with ` ...`.
- This keeps output simple for demos/debugging while still exposing key metadata.

## 11) Current Project Status

### Implemented in `engine.c`
- Supervisor socket control path exists.
- Container start/run metadata is tracked in memory.
- Bounded-buffer logging is wired up.
- Child setup does `chroot`, `/proc` mount, hostname setup, and stdio redirection.
- `ps` now returns container metadata and a termination reason.
- `logs` now reads log content instead of only returning the file path.
- `run` has a polling-based wait path and forwards stop intent on SIGINT/SIGTERM.

### Still to finish for guide compliance
- Kernel monitor TODOs in `monitor.c`.
- End-to-end runtime validation for stop vs hard-limit termination classification.
- Scheduler experiments and README writeup.
- Possible cleanup of remaining compile warnings.

### Why this note exists
- It gives us a running snapshot while we work through the rest of the project.
- It helps separate implemented behavior from guide-level compliance that still needs verification.

## 12) Monitor TODO 1

`monitor.c` starts with a linked-list node and global list lock.

### What TODO 1 requires
- A node type that tracks:
  - PID
  - container ID
  - soft limit
  - hard limit
  - whether the soft-limit warning was already emitted
  - `struct list_head` linkage
- A shared global list of monitored entries.
- A lock protecting insert, remove, and iteration.

### What we chose
- Node type: `struct monitor_entry`
- Global list: `monitored_entries`
- Lock: `spinlock_t monitored_lock`

### Why a spinlock makes sense here
- The timer callback can run in atomic context, where sleeping is not allowed.
- A spinlock is safe for short critical sections that protect list insert/remove/iteration.
- The code path is simple enough that a sleepable mutex is not necessary.

### Status
- TODO 1 is now in place in `boilerplate/monitor.c`.
- Next monitor steps are periodic enforcement, ioctl register/unregister behavior, and cleanup.

## 13) Monitor TODO 2

TODO 2 is the shared list and lock used by both ioctl and timer paths.

### What is in place
- Global list: `monitored_entries`
- Lock: `monitored_lock`

### Why this satisfies the requirement
- `monitored_entries` is shared by registration, unregister, and periodic enforcement.
- `monitored_lock` protects insert, remove, and iteration.
- A spinlock fits because the timer path cannot sleep and the critical sections are short.

### Status
- TODO 2 is now represented directly in `boilerplate/monitor.c`.
- The next monitor TODO is periodic enforcement in the timer callback.

## 14) Monitor TODO 3

Implemented the periodic monitor loop in `timer_callback`.

### What it does each interval
- Takes `monitored_lock`.
- Iterates `monitored_entries` with `list_for_each_entry_safe`.
- Reads RSS for each PID.
- Removes entries whose process no longer exists.
- Emits soft-limit warning once per entry (`soft_limit_warned`).
- Enforces hard limit with SIGKILL helper and removes that entry.
- Releases lock and re-arms the timer.

### Why this satisfies TODO 3
- Safe deletion during iteration: uses `_safe` list traversal.
- Tracks exited tasks and cleans them up.
- Guarantees one-time soft warning behavior.
- Enforces hard limit policy and removes killed entry from monitor list.

## 15) Monitor Build Validation Notes

When validating `monitor.c`, use kernel build flow (`make module`), not plain user-space `gcc`.

### Issues encountered and fixed
- `gcc -fsyntax-only monitor.c` failed with missing kernel headers (`linux/cdev.h`) which is expected for kernel module code.
- `make module` initially failed due to:
  - API mismatch: `del_timer_sync` on this kernel headers version.
  - stale root-owned dependency file `.monitor.o.d` from an earlier build.

### Fixes applied
- Updated unload path to use `timer_delete_sync(&monitor_timer)`.
- Removed stale build artifacts (`.monitor.o.d`, `monitor.o`) and rebuilt.

### Current status
- `make module` succeeds in this environment.

## 16) Monitor TODO 4

Implemented register-path insertion in `monitor_ioctl` for `MONITOR_REGISTER`.

### What it now does
- Validates request fields (`pid > 0`, non-zero limits, `soft <= hard`).
- Allocates and initializes a `monitor_entry` from user request data.
- Protects list operations under `monitored_lock`.
- Rejects duplicate PID registration with `-EEXIST`.
- Inserts valid entries into `monitored_entries`.

## 17) Monitor TODO 5

Implemented unregister-path removal in `monitor_ioctl` for `MONITOR_UNREGISTER`.

### Matching behavior
- Supports removal by PID, container ID, or both.
- Uses lock-protected safe traversal and deletion.
- Returns `0` on successful removal, `-ENOENT` when no match exists.

## 18) Monitor TODO 6

Implemented module-exit cleanup for all remaining tracked entries.

### What happens on unload
- Stops timer synchronously via compatibility helper.
- Iterates the monitored list with safe deletion under lock.
- Frees every remaining `monitor_entry`.
- Leaves no tracked-list memory behind on module unload.

## 19) Monitor Init Locking

Added explicit lock initialization in `monitor_init`:
- `spin_lock_init(&monitored_lock);`

This ensures lock state is valid before ioctl/timer paths begin using the shared list.

## 20) Engine Logging Producer-Thread Refactor

Updated `engine.c` to align with the guide requirement that container output is read by producer thread(s) and inserted into the bounded shared buffer.

### What changed
- Added producer-thread fields to `container_record_t`:
  - `producer_thread`
  - `producer_started`
- Added `log_producer_ctx_t` carrying:
  - `supervisor_ctx_t *ctx`
  - `container_record_t *record`
- Added new helpers:
  - `container_log_producer_thread(...)`
  - `start_log_producer(...)`
  - `join_log_producer(...)`
  - `stop_log_producer(...)`
  - `stop_all_log_producers(...)`

### New producer behavior
- For each started container, supervisor now launches one producer thread bound to that container's log pipe read end.
- The producer reads chunks from the pipe and pushes `log_item_t` into `bounded_buffer_t`.
- The existing `logging_thread` remains the single consumer that writes queued chunks to log files.

### Lifecycle and cleanup
- Producer startup is now part of start/run container handling.
- On stop/shutdown paths, producers are stopped and joined before final metadata teardown.
- This replaces event-loop-based periodic log pipe draining with explicit producer threads.

### Validation status
- `make -s engine` succeeds after this refactor.
- There are still warning-level `strncpy`/format truncation diagnostics, but no compile errors.
