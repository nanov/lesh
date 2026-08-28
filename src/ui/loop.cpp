#include "ui/loop.h"

#include "fiber/slot.h"
#include "fiber/stack.h"
#include "leshper/keymap.h"
#include "substrate/assert.h"
#include "substrate/fork_guard.h"
#include "substrate/log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <variant>

namespace lesh::ui {
namespace {

// The pid this process started as, captured before any handler can run.
//
// THE ONE THING THE HANDLER CHECKS FIRST. fish's `reraise_if_forked_child`
// compares `getpid()` to this rather than consulting an atfork flag, "Don't use
// is_forked_child: it relies on atfork handlers which may have not yet run" -
// and a handler that runs in a forked child and pokes the PARENT's self-pipe is
// a wakeup delivered to the wrong process with a signal silently swallowed.
const pid_t g_main_pid = ::getpid();

// The installed hub. A handler takes no context pointer, so exactly one hub at
// a time may own the dispositions; every other hub is a test's and is driven
// through `deliver()`.
//
// A plain pointer and not an atomic: it is written before `sigaction` publishes
// the handler and cleared after `sigaction` unpublishes it, so a handler that
// runs at all runs after the write and before the clear.
signal_hub* g_installed_hub = nullptr;

void fish_style_handler(int signo) {
	const int saved = errno;

	// FIRST, before anything else. A forked child that has not yet exec'd must
	// behave as though it had inherited the default disposition, which means
	// resetting and re-raising rather than running the parent's editor logic.
	if (::getpid() != g_main_pid) {
		struct sigaction dfl{};
		dfl.sa_handler = SIG_DFL;
		sigemptyset(&dfl.sa_mask);
		::sigaction(signo, &dfl, nullptr);
		::raise(signo);
		errno = saved;
		return;
	}

	if (g_installed_hub != nullptr)
		g_installed_hub->deliver(signo);

	errno = saved;
}

// What the kernel currently has, reduced to the four cases the ownership rules
// distinguish (#142; the rules themselves are on `reassert` in the header).
enum class disposition_kind : std::uint8_t { ours, defaulted, ignored, foreign };

// SA_SIGINFO IS ANSWERED FIRST, and not out of tidiness: `sa_handler` and
// `sa_sigaction` are a union on every platform this builds for, so reading
// `sa_handler` out of a three-argument handler is reading the wrong member. Such
// a handler is `foreign` by every rule that matters - it is not ours, it is not a
// disposition, and `deliver` declines to chain to it for want of a `siginfo_t`.
disposition_kind kind_of(const struct sigaction& current) noexcept {
	if ((current.sa_flags & SA_SIGINFO) != 0)
		return disposition_kind::foreign;
	if (current.sa_handler == &fish_style_handler)
		return disposition_kind::ours;
	if (current.sa_handler == SIG_DFL)
		return disposition_kind::defaulted;
	if (current.sa_handler == SIG_IGN)
		return disposition_kind::ignored;
	return disposition_kind::foreign;
}

// The pointer `deliver` may call for this disposition, or null for "there is
// nothing to call".
//
// THE FLAGS ARE CONSUMED HERE rather than carried into the slot. A handler
// installed with SA_SIGINFO is not chainable - `deliver` has only a signal
// number, and inventing a `siginfo_t` to pass would be worse than declining -
// and `sa_handler` is the wrong member of the union to read for one anyway. It
// is still TAKEN like any other foreign handler; it is simply chained to by
// nobody. Nothing lesh installs uses SA_SIGINFO, and the sanitizers' handlers
// are for signals this hub never touches.
void (*chain_target_of(const struct sigaction& current) noexcept)(int) {
	if ((current.sa_flags & SA_SIGINFO) != 0)
		return nullptr;
	if (current.sa_handler == SIG_DFL || current.sa_handler == SIG_IGN)
		return nullptr;
	return current.sa_handler;
}

bool make_wakeup_pipe(int& read_fd, int& write_fd) noexcept {
	int fds[2] = {-1, -1};
	if (::pipe(fds) != 0)
		return false;
	for (int fd : fds) {
		::fcntl(fd, F_SETFD, FD_CLOEXEC);
		::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	}
	read_fd = fds[0];
	write_fd = fds[1];
	return true;
}

// The position of one alternative in `leshper::effect`, as a constant.
//
// So that `carry_out`'s switch labels are spelled as TYPES while the switch
// itself is on `index()`: a reordered variant moves every label with it, where a
// hand-written `case 4:` would quietly start meaning something else.
template <typename Which, typename... Rest>
constexpr std::size_t position_in(const std::variant<Rest...>*) noexcept {
	constexpr bool matches[] = {std::is_same_v<Which, Rest>...};
	for (std::size_t at = 0; at < sizeof...(Rest); ++at) {
		if (matches[at])
			return at;
	}
	return sizeof...(Rest);
}

template <typename Which>
inline constexpr std::size_t effect_index =
	position_in<Which>(static_cast<const leshper::effect*>(nullptr));

// POLLIN AND POLLHUP TOGETHER, always. fish `fds.cpp`: "If a pipe is widowed
// with no data, Linux sets POLLHUP but not POLLIN, so test for both." POLLERR
// joins them because a topic whose fd has errored still has to be answered -
// with a drain that finds nothing and a queue that stops being armed.
constexpr short kReadable = POLLIN | POLLHUP | POLLERR;

// How long a turn sleeps when even the signal topic will not poll (#211 §4.1).
// Reached only after `poll` has failed twice on a set narrowed to this process's
// own self-pipe, which is a shell with nothing left to wait on; what the sleep
// buys is a `waitpid` sweep that ends the command rather than a spun core.
constexpr int kBlindWaitMs = 20;

constexpr short revents_of(const struct pollfd& one) noexcept {
	return static_cast<short>(one.revents & kReadable);
}

// A ZERO TIMEOUT IS A QUESTION, NOT A WAIT - AND `poll` ANSWERS IT SLOWLY (#206).
//
// Measured on the dev machine, release, arm64 macOS, one non-readable pipe:
//
//   poll(fds, n, 0)     nothing ready      8.1 - 8.7 us
//   poll(fds, n, 0)     a byte waiting     0.34 us
//   poll(nullptr, 0, 0) nothing to ask     7.5 us
//   select(..., {0,0})  either way         0.18 - 0.28 us
//   read() -> EAGAIN                       0.17 us
//
// XNU's `poll` takes a "nothing is ready, so wait - for zero nanoseconds" path
// through the scheduler that `select` fast-paths and it does not; `poll` with
// NO DESCRIPTORS AT ALL costs the same 7.5 us, which is what makes it the wait
// and not the scan. Thirty times, for identical semantics.
//
// It is the whole price of a cooperative yield. A reactor that yields hands the
// thread back to `turn`, which asks the terminal what arrived before it comes
// back - so 8 us of kernel sat on top of a 12 ns switch and a 21 ns tick, and
// the autosuggester's 5000-entry walk paid it 2500 times. #202's "a yield is one
// extra loop turn whose poll(0) finds nothing - about 1 us" was the right shape
// and the wrong platform constant.
//
// So the zero-timeout case asks `select` and every other case keeps `poll`,
// where a wait's cost is the wait and poll's larger descriptor space is worth
// having. `select` cannot name a descriptor at or above `FD_SETSIZE` - writing
// one into an `fd_set` is a write past the end of the object - so a loop handed
// one falls back rather than corrupting its own stack.
//
// The two readiness bits `select` does not have are not bits this loop reads:
// a hung-up or errored descriptor selects as READABLE, and `revents_of` folds
// POLLHUP and POLLERR into "go and drain it" anyway, which is exactly what the
// drains then do - `drain_tty` calls a zero-length read a hangup.
int ready_now(struct pollfd* fds, int count, int timeout_ms) noexcept {
	if (timeout_ms != 0)
		return ::poll(fds, static_cast<nfds_t>(count), timeout_ms);

	fd_set readable;
	FD_ZERO(&readable);
	int highest = -1;
	for (int i = 0; i < count; ++i) {
		const int fd = fds[static_cast<std::size_t>(i)].fd;
		if (fd < 0 || fd >= FD_SETSIZE)
			return ::poll(fds, static_cast<nfds_t>(count), 0);
		FD_SET(fd, &readable);
		if (fd > highest)
			highest = fd;
	}

	struct timeval right_now{};
	const int ready = ::select(highest + 1, &readable, nullptr, nullptr, &right_now);
	for (int i = 0; i < count; ++i) {
		struct pollfd& one = fds[static_cast<std::size_t>(i)];
		one.revents = ready > 0 && FD_ISSET(one.fd, &readable) ? POLLIN : 0;
	}
	return ready;
}

} // namespace

const char* name_of(topic which) noexcept {
	switch (which) {
		case topic::tty: return "tty";
		case topic::signal: return "signal";
		case topic::timer: return "timer";
		case topic::watch: return "watch";
		case topic::count_: break;
	}
	return "?";
}

const char* name_of(phase which) noexcept {
	switch (which) {
		case phase::editing: return "editing";
		case phase::executing: return "executing";
		case phase::boundary: return "boundary";
	}
	return "?";
}

// ---------------------------------------------------------------------------
// A reactor's lane (#202)
// ---------------------------------------------------------------------------

// ONE FIBER, ONE SLOT, AND THE STORAGE ITS COMPUTES SERVE OUT OF.
//
// WHAT THE SLOT CARRIES IS THE EVENT MASK, NOT THE SNAPSHOT, and that is the one
// judgment call in this file. `slot<T>::send` takes `T` by value, so a
// `slot<request_snapshot>` would have the host BUILD a snapshot per keystroke and
// move it in - and a moved-from `std::string` has given its capacity away, so the
// next keystroke allocates. Zero is the number the allocation gate holds this
// path to (`AWarmShellReactorRoundCostsNoHeap`), so the snapshot stays in
// storage the lane owns and the slot carries the notification: "these kinds
// changed". Two snapshots rather than one, swapped at `recv`, because the sender
// must be able to write the NEXT one while a compute is still reading the last -
// which is exactly the `std::swap(job, owner->pending)` the helper pool used to
// do, one thread further in.
//
// The `slot` is still doing the whole of its job: it is the conflating channel
// whose send counter supersedes every outstanding token, it is what parks and
// wakes the fiber, and its two debug counters answer "how many notifications did
// this reactor get, and how many did nobody pick up".
struct reactor_lane {
	reactor_lane(event_loop& loop, fiber::scheduler& on, std::string_view named)
		: owner(&loop), inbox(on), name(named) {}

	event_loop* owner = nullptr;
	// The channel: capacity one, conflating, latest wins. Overwrite IS
	// cancellation (#90's rule as #198 generalized it).
	fiber::slot<std::uint32_t> inbox;
	// OWNED, because the fiber's name must outlive the scheduler and the registry
	// key this was copied from is only guaranteed to outlive the dispatch table.
	std::string name;
	lesh_reactor_fn fn = nullptr;
	void* userdata = nullptr;
	fiber::fiber* self = nullptr;

	// WHAT THE HOST WRITES and WHAT THE FIBER READS. Swapped at every `recv`, so
	// both keep their capacity for the life of the session.
	request_snapshot arriving;
	request_snapshot computing;
	leshper::reactor_batch batch;

	// THE FLAG THE ABI POLLS, and how it reflects the slot.
	//
	// #202 kept `run_reactor_here`'s `const std::atomic<bool>&` rather than
	// adapting `lesh_request_superseded` to a callback, so `abi.h` and #90's poll
	// are untouched. The flag is RAISED by the send site and CLEARED by the fiber
	// immediately after `recv`, which makes it exactly the token's own answer:
	// `slot::send` bumps the counter that supersedes every outstanding token, and
	// `slot::recv` mints a fresh one. `reactor_yield` asserts the two agree at
	// every poll point, so the duplication is held to account by a check rather
	// than by this comment.
	std::atomic<bool> superseded{false};

	std::size_t computes = 0;
	std::size_t abandoned = 0;
	std::size_t yields = 0;
};

// ---------------------------------------------------------------------------
// signal_hub
// ---------------------------------------------------------------------------

signal_hub::signal_hub() {
	const bool made = make_wakeup_pipe(_read_fd, _write_fd);
	LESH_ASSERT(made);
	(void)made;
	// One `pthread_atfork` child hook, for debug assertions only - never for the
	// handler's child check above (see substrate/fork_guard.h).
	install_fork_child_detection();
}

signal_hub::~signal_hub() {
	uninstall();
	if (_read_fd >= 0)
		::close(_read_fd);
	if (_write_fd >= 0)
		::close(_write_fd);
}

void signal_hub::deliver(int signo) noexcept {
	// EVERYTHING HERE IS ASYNC-SIGNAL-SAFE, and nothing else may be added that
	// is not. Two `volatile sig_atomic_t` writes and one `write(2)`.
	if (signo == SIGWINCH) {
		// A COUNTER, NOT A QUEUE. The size is read from the kernel by the loop;
		// queueing resizes would be queueing sizes that were already stale when
		// they were queued (#128's trap 12, fish `termsize.cpp`).
		_resizes = _resizes + 1;
	} else if (signo > 0 && signo < kMaxTrackedSignal) {
		_pending[signo] = 1;
	}
	poke();

	// THE CHAIN, and it is last on purpose (#134). Our own work is done and the
	// wakeup is already in the pipe, so whatever the shell installed before us -
	// `runtime/signals.cpp`'s `record_signal`, which is the only thing that ever
	// sets `g_pending` - runs with nothing of ours left to lose. Calling a
	// function pointer is async-signal-safe; the function on the other end is the
	// shell's business and is written to the same rule.
	//
	// `_chain` AND NOT `_saved` (#142): the target is whatever the kernel had at
	// the LAST take, so a `trap` typed five commands into the session is the
	// thing called. `_saved` is the entry-time disposition and is `uninstall`'s
	// alone.
	//
	// ONE RELAXED LOAD OF A LOCK-FREE POINTER, which is what makes this safe from
	// a handler: no lock, no tear, and no ordering needed because the pointer is
	// the whole of the information. SIG_DFL and SIG_IGN were resolved to null at
	// the store (`chain_target_of`), and they are re-checked here anyway -
	// calling address 1 would be a jump into nothing.
	if (signo <= 0 || signo >= kMaxTrackedSignal)
		return;
	void (*const previous)(int) = _chain[signo].load(std::memory_order_relaxed);
	if (previous == nullptr || previous == SIG_DFL || previous == SIG_IGN)
		return;
	previous(signo);
}

bool signal_hub::catches(int signo) const noexcept {
	// ASKED OF THE KERNEL. See the header: a `trap` inside the command that is
	// running now has already replaced the disposition and the `reassert` on the
	// way out of that command has not happened yet, so a member would be stale
	// exactly when the answer matters.
	struct sigaction now{};
	if (signo < 0 || signo >= kMaxTrackedSignal || ::sigaction(signo, nullptr, &now) != 0)
		return false;
	return (now.sa_flags & SA_SIGINFO) == 0 && now.sa_handler == &fish_style_handler;
}

void signal_hub::poke() noexcept {
	const char byte = 0;
	ssize_t n;
	do {
		n = ::write(_write_fd, &byte, 1);
	} while (n < 0 && errno == EINTR);
	(void)n;
}

unsigned signal_hub::resize_count() const noexcept {
	return static_cast<unsigned>(_resizes);
}

std::size_t signal_hub::drain(std::vector<int>& out) noexcept {
	// The byte first, then the flags: the opposite order loses a signal that
	// lands between the two reads, because its byte would be consumed by the
	// read that preceded its flag being set. fish's `iothread_service_main`
	// states the same ordering rule for the same reason ("we must consume events
	// before handling requests, as posting uses the opposite order").
	char scratch[64];
	ssize_t n;
	do {
		n = ::read(_read_fd, scratch, sizeof(scratch));
	} while (n > 0 || (n < 0 && errno == EINTR));

	std::size_t found = 0;
	for (int signo = 1; signo < kMaxTrackedSignal; ++signo) {
		if (_pending[signo] == 0)
			continue;
		_pending[signo] = 0;
		out.push_back(signo);
		++found;
	}
	return found;
}

bool signal_hub::install() noexcept {
	if (_installed)
		return true;
	LESH_ASSERT(g_installed_hub == nullptr);
	g_installed_hub = this;
	_installed = true;
	return take_dispositions();
}

bool signal_hub::reassert() noexcept {
	// Nothing was ever taken, so there is nothing to take back - and a hub a test
	// only ever `deliver`s to must not start writing the binary's dispositions.
	if (!_installed)
		return false;
	return take_dispositions();
}

bool signal_hub::take_dispositions() noexcept {
	bool complete = true;

	// THE ENTRY-TIME DISPOSITION, AT MOST ONCE. `uninstall`'s only source: what
	// the process had before this hub touched the signal at all. The caller
	// already asked the kernel, so this takes the answer rather than asking
	// again - a second query could see a handler installed in between and record
	// the wrong original.
	const auto save_once = [this](int signo, const struct sigaction& current) {
		if (_saved_valid[signo])
			return;
		_saved[signo] = current;
		_saved_valid[signo] = true;
	};

	// THE CHAIN TARGET, AT EVERY TAKE. One relaxed store of a lock-free pointer,
	// and NOT a `sigprocmask` fence: a mask is per-thread and says nothing about
	// delivery to another thread, so it would have fenced nothing. What makes the
	// race benign is that the slot is a single un-tearable pointer - an in-flight
	// signal chains to the old target or the new one, and both are real handlers.
	const auto retarget = [this](int signo, void (*to)(int)) {
		_chain[signo].store(to, std::memory_order_relaxed);
	};

	// The caught set: ours to take unless the kernel says otherwise. Rules 1-4a
	// on `reassert` in the header, in the order they are written there.
	const auto catch_it = [&](int signo, int flags) {
		struct sigaction current{};
		if (::sigaction(signo, nullptr, &current) != 0) {
			complete = false;
			return;
		}
		switch (kind_of(current)) {
			case disposition_kind::ours:
				return;  // rule 1: held already, and self-chaining excluded here
			case disposition_kind::ignored:
				return;  // rule 3: the newest ignore stands, whoever set it
			case disposition_kind::defaulted:
			case disposition_kind::foreign:
				// Rule 2 and rule 4a, and they are one line because
				// `chain_target_of` already answers both: a default resolves to
				// null (nothing to call), a real handler to itself (take it AND
				// call through).
				save_once(signo, current);
				retarget(signo, chain_target_of(current));
				break;
		}
		struct sigaction action{};
		action.sa_handler = &fish_style_handler;
		sigemptyset(&action.sa_mask);
		action.sa_flags = flags;
		if (::sigaction(signo, &action, nullptr) != 0)
			complete = false;
	};

	// The ignored set: SIG_DFL is the ONLY thing here that is ours to overwrite.
	// An inherited ignore is rule 3 and a user's `trap` is rule 4b, and both come
	// out the same way - we stand aside. The hub never consumed these signals, so
	// there is nothing to chain and `_chain` stays empty for them.
	const auto ignore_it = [&](int signo) {
		struct sigaction current{};
		if (::sigaction(signo, nullptr, &current) != 0) {
			complete = false;
			return;
		}
		if (kind_of(current) != disposition_kind::defaulted)
			return;
		save_once(signo, current);
		struct sigaction action{};
		action.sa_handler = SIG_IGN;
		sigemptyset(&action.sa_mask);
		if (::sigaction(signo, &action, nullptr) != 0)
			complete = false;
	};

	// SIGINT WITHOUT SA_RESTART. The whole point: a `poll` blocked in the middle
	// of a turn must be interrupted so Ctrl-C becomes an event now rather than
	// whenever the next key happens to arrive.
	catch_it(SIGINT, 0);
	// SIGCHLD WITH SA_RESTART: "we want SIGCHLD to not interrupt restartable
	// syscalls" (fish `signal.cpp`). The reap is the shell's own, on this same
	// thread, and there is nothing here that a child exiting should tear.
	catch_it(SIGCHLD, SA_RESTART);
	catch_it(SIGWINCH, SA_RESTART);

	// SIGHUP IS NOT HERE, and its absence is the decision (#142). The editor's
	// hangup is the tty's POLLHUP, which `drain_tty` already turns into
	// `signal_event{SIGHUP}` and `_exiting`; a real SIGHUP is the shell's own,
	// which makes `trap - HUP` fatal as POSIX says and `nohup` respected by
	// construction rather than by the conditional that used to sit here. The
	// termios residual is written down on `install()` in the header.

	// "We are a shell, we know what is best for the user" (fish) - but only over
	// the default. SIGTTOU and SIGTTIN also go through tty.h's
	// `ignore_background_write_signals`, because they must be ignored before a
	// mode change even in a process that never installed a hub.
	ignore_it(SIGPIPE);
	ignore_it(SIGQUIT);
	ignore_it(SIGTSTP);
	ignore_it(SIGTTOU);
	ignore_it(SIGTTIN);

	return complete;
}

void signal_hub::uninstall() noexcept {
	if (!_installed)
		return;
	for (int signo = 1; signo < kMaxTrackedSignal; ++signo) {
		// THE ENTRY-TIME DISPOSITION, not the newest one. A signal never taken -
		// an inherited SIG_IGN, a user's `trap` on the ignored set - has no
		// `_saved_valid` and is correctly left exactly as it is.
		if (_saved_valid[signo]) {
			::sigaction(signo, &_saved[signo], nullptr);
			_saved_valid[signo] = false;
		}
		// The chain goes with it: our handler is unpublished by the line above,
		// so nothing can be reading this any more.
		_chain[signo].store(nullptr, std::memory_order_relaxed);
	}
	if (g_installed_hub == this)
		g_installed_hub = nullptr;
	_installed = false;
}

// ---------------------------------------------------------------------------
// event_loop
// ---------------------------------------------------------------------------

event_loop::event_loop(loop_fds fds, loop_options options)
	: _fds(fds),
	  _options(std::move(options)),
	  _terminal(fds.input),
	  _decoder(_options.escape_timeout),
	  _blitter(_pool, _options.capabilities),
	  // THE SCHEDULER (#202). `watchdog_action::log` for a shell - a frozen prompt
	  // is bad and a dead prompt is worse - and the default stack size, which is
	  // 512 KB and 1 MB under ASan: two fibers is one or two megabytes of reserved
	  // address space, committed on touch.
	  _sched(fiber::scheduler_options{0, std::chrono::milliseconds{50},
	                                  _options.reactor_watchdog}) {
	// The signal topic always exists, even in a loop that never installs a
	// handler: `stop()` rings its pipe to wake a poll that has nothing else to
	// say, and a test delivers to it through `deliver()`. Only `install()` is
	// process-global.
	_own_signals.emplace();
	_signals = &*_own_signals;

	// Capacity taken once, so a warm turn allocates nothing. The numbers are the
	// shapes of the thing, not tuning: one read of a terminal, a handful of
	// events per turn, one message per attached topic.
	_read_buffer.reserve(4096);
	_events.reserve(16);
	_carried_events.reserve(16);
	_signal_numbers.reserve(8);
	_out.reserve(4096);
	_accepted.reserve(256);
	// RESERVED, because `await_child` is reached through a `noexcept` verb and
	// must not allocate. Eight is far past what any shape today can produce -
	// there is one execution fiber and it waits for one child at a time.
	_child_waits.reserve(8);

	install_fork_child_detection();
}

event_loop::~event_loop() {
	// NOTHING TO JOIN AND NOTHING TO GIVE BACK (#201). This used to `stop()` a
	// thread of its own and then hand the actor's pooled messages back before the
	// actor that owned their storage died (ADR-0007); there is no thread and no
	// message pool. `run()` has returned by the time a caller destroys a loop,
	// because `run()` is a call on the caller's own thread.
	//
	// ONE THING TO DO, AND IT IS NOT OPTIONAL (#202). v1 has no cancellation by
	// destruction: `~scheduler` unmaps a parked fiber's stack WITHOUT unwinding
	// it, so anything that stack owned is lost - and the one thing a reactor's
	// stack always owns mid-compute is the snapshot's buffer, which
	// `run_reactor_here` moved into the token and has not yet moved back. A loop
	// destroyed with a fiber suspended inside a `$PATH` walk would therefore leak
	// it, and `FiberLsanPositiveControl` is the proof that LeakSanitizer sees
	// exactly this shape. So every emitter is superseded and run out to its next
	// cancellation poll first, which is where it abandons the walk and parks back
	// on `recv` owning nothing.
	//
	// AND THE EXECUTION FIBER CANNOT BE DRAINED THAT WAY (#211 §4.1), because a
	// command has no cancellation poll to abandon at. What holds for it is the
	// stronger statement: `run_the_line` does not return until the status is back,
	// so by the time anybody can destroy this loop the fiber is parked on its inbox
	// owning nothing. Asserted rather than trusted - the abandoned case leaked the
	// executor's frames, its redirection fds and its child, and left no mark.
	LESH_ASSERT(!has_execution_fiber_mid_command()
	            && "a loop destroyed with a command still on the execution fiber");
	drain_emitters();
}

void event_loop::attach_registry(leshper::registry& reg) noexcept { _registry = &reg; }

void event_loop::attach_shell(shell_side& shell, const leshper::host* host,
                             shell_writing_flag* writing) noexcept {
	_shell = &shell;
	_shell_host = host;
	_writing = writing;
}

void event_loop::attach_signals(signal_hub& hub) noexcept {
	_signals = &hub;
	_resizes_seen = hub.resize_count();
}

void event_loop::attach_watch(int fd, void (*on_readable)(void* userdata),
                              void* userdata) noexcept {
	// BOTH OR NEITHER. A descriptor with no hook would be polled forever and
	// never consumed, which is the spin this topic's whole contract is about.
	if (fd < 0 || on_readable == nullptr) {
		detach_watch();
		return;
	}
	_watch_fd = fd;
	_watch_hook = on_readable;
	_watch_userdata = userdata;
}

void event_loop::detach_watch() noexcept {
	_watch_fd = -1;
	_watch_hook = nullptr;
	_watch_userdata = nullptr;
}

// ---------------------------------------------------------------------------
// The read entry
// ---------------------------------------------------------------------------

void event_loop::refresh_size_from_terminal() {
	const terminal_size size = _terminal.size();
	if (size.columns == _state.columns && size.rows == _state.rows)
		return;
	_state.columns = size.columns;
	_state.rows = size.rows;
	// A different size means the previous surface is a different shape, and
	// #112's `update` refuses two surfaces of different sizes anyway.
	_have_previous = false;
	_needs_render = true;
}

void event_loop::enter_read() {
	if (_options.manage_terminal) {
		// SIGTTOU first, always, and before anything that could stop us.
		ignore_background_write_signals();
		// UNCONDITIONALLY (fish #9181): "a command we ran when job control was
		// disabled nevertheless stole the tty from us ... So just unconditionally
		// reclaim the tty."
		const tty_transfer took = _terminal.reclaim();
		if (took == tty_transfer::not_ours || took == tty_transfer::failed)
			LESH_LOG(log::level::warn, log::category::loop,
			         "read entry could not reclaim the terminal (%d)", static_cast<int>(took));
		_terminal.enter_raw();
	}

	// #98 decision 6: the winsize is re-queried at EVERY read start, which is
	// what makes a resize missed during a command impossible rather than
	// handled. The SIGWINCH counter is realigned here for the same reason.
	// THE COUNTER ONLY, AND NO `sigaction` (#142). Re-asserting the dispositions
	// used to happen here, which made the loop a writer of process-wide state the
	// `trap` builtin writes too - two writers when the loop was a thread of its
	// own. It moved to the shell side of the ui layer (`ui/session.cpp`), which
	// leaves one; the loop only ever READS the hub.
	if (_signals != nullptr)
		_resizes_seen = _signals->resize_count();
	refresh_size_from_terminal();
	_needs_render = true;
}

void event_loop::leave_read() {
	// #152: THE PARENT'S PROMPT STARTS AT COLUMN 0. fish and zsh both move to a
	// fresh line on their way out; zsh's inverse `%` marker is what a shell that
	// does not looks like from the outside, and it is what the owner saw.
	//
	// A CHECK, NOT A GUESS. The last layout knows which cell the terminal's
	// cursor is parked on - `screen.cursor()` is the placement the blitter
	// honoured - so the newline is written exactly when the cursor is not at the
	// left edge. When there is no live layout there is nothing to move and
	// nothing is written: an accepted `exit` took that path, where
	// `accept_current_line` wrote the newline before the command ran and dropped
	// `_have_previous` on the way, and painted nothing after it.
	//
	// Before `leave_raw`, because until it returns the terminal is still the
	// loop's - the byte goes out the way every other byte the terminal receives
	// does (#98).
	if (_have_previous && _previous.screen.cursor().column != 0 && _fds.output >= 0)
		write_all(_fds.output, "\r\n");
	if (_options.manage_terminal)
		_terminal.leave_raw();
	// The bytes still held belong to whoever has the terminal next (#111's
	// `reset` exists for exactly this).
	_decoder.reset();
}

// ---------------------------------------------------------------------------
// Timeout
// ---------------------------------------------------------------------------

int event_loop::poll_timeout_ms() const noexcept {
	std::optional<clock::time_point> soonest = _decoder.deadline();
	for (const timer_due& armed : _timers) {
		if (!soonest.has_value() || armed.due < *soonest)
			soonest = armed.due;
	}
	// A RUNNABLE REACTOR IS A ZERO TIMEOUT (#202, and the owner's tick: "if no new
	// keys arrived, tick them"). A fiber that has yielded mid-walk is work this
	// loop owes, so the poll must not sleep on top of it - it asks the terminal
	// what is there and comes straight back to the slice. With nothing runnable
	// the loop blocks exactly as it always did.
	if (_sched.runnable(sliced_lanes()))
		return 0;
	// WHILE A COMMAND RUNS THERE IS NO CLOCK (#208). The timer topic is out of the
	// turn - a timer would dispatch an action into an editor with no terminal -
	// and the decoder's ESC deadline belongs to bytes nothing is reading. What is
	// left to wait for is the signal topic (the SIGCHLD that ends the wait) and
	// the watch, and both of those are descriptors, so the poll blocks on them.
	if (_phase == phase::executing)
		return -1;
	if (!soonest.has_value())
		return -1;  // nothing waits on time; block until a topic speaks

	const clock::time_point now = clock::now();
	if (*soonest <= now)
		return 0;
	const auto left =
		std::chrono::duration_cast<std::chrono::milliseconds>(*soonest - now).count();
	// Rounded UP: a timeout that expires a fraction of a millisecond early makes
	// `expire()` decline (it re-checks `now`) and the loop spin.
	return static_cast<int>(left) + 1;
}

// ---------------------------------------------------------------------------
// The turn
// ---------------------------------------------------------------------------

turn_result event_loop::turn() {
	// THE REGISTRY'S QUEUE BEFORE THE DEADLINE (#168). A timer armed while the
	// loop was not turning - the prompt's tick, rearmed by the shell side on its
	// way out of a command - has to be in the table before the poll timeout is
	// computed from it, or the wake it asked for waits on the next input instead.
	drain_registry_effects();
	return turn(poll_timeout_ms());
}

turn_result event_loop::turn(int timeout_ms) {
	turn_result result;
	_events.clear();
	_needs_render = false;
	drain_registry_effects();

	// THE SIGNALS A COMMAND'S TURNS HELD BACK (#208), FIRST THING. Replayed here
	// rather than at `resume_after_execution` for a plain reason: a turn clears
	// `_events` before it does anything else, so an event pushed between turns
	// would be wiped. This is the same delivery the self-pipe byte used to make
	// on the first turn after a command - see `_deferred_signals`.
	if (_phase != phase::executing && !_deferred_signals.empty()) {
		for (const int signo : _deferred_signals) {
			LESH_LOG(log::level::debug, log::category::loop,
			         "topic=signal (deferred past a command) signo=%d", signo);
			_events.push_back(leshper::signal_event{signo});
		}
		_deferred_signals.clear();
	}

	// THERE IS NO DEFERRED QUEUE ANY MORE (#201). `_deferred` held what the
	// blocked `wait_on_shell` poll drained while a command ran - it was the only
	// thing that polled during an execution, so what it read had to be kept
	// somewhere until the editor existed again. Nothing polls during an execution
	// now: the signal sits in the self-pipe, this turn's poll finds it readable,
	// and `drain_signal_topic` turns it into an event like any other.

	// THE LEADING SLICES (#202). Every emitter that was runnable when this turn
	// began gets one, BEFORE the terminal is polled - which is what continues a
	// walk that yielded at its last cancellation poll. `tick` snapshots the ready
	// set up front (#198), so a fiber woken by another fiber's slice waits for the
	// next turn and this call is bounded work.
	//
	// The events and the render a leading slice produces are this turn's: the walk
	// and the render below are after it, so a batch that lands here is applied and
	// painted without waiting for another poll.
	(void)tick_fibers();

	int at = 0;
	int tty_at = -1, signal_at = -1, watch_at = -1;
	const auto poll_on = [&](int fd) {
		_poll[static_cast<std::size_t>(at)].fd = fd;
		_poll[static_cast<std::size_t>(at)].events = POLLIN;
		_poll[static_cast<std::size_t>(at)].revents = 0;
		return at++;
	};
	// THE TTY TOPIC IS OUT WHILE A COMMAND RUNS (#208). The terminal is the
	// child's - the loop gave up the foreground group at `quiesce` - so reading it
	// would earn a SIGTTIN, and the bytes the user types belong to the command
	// and not to a line editor that is not on screen. The signal topic and the
	// watch stay in: the first is how the foreground wait ends, and a history
	// written by another shell is no less true during a command.
	if (_fds.input >= 0 && _phase != phase::executing)
		tty_at = poll_on(_fds.input);
	if (_signals != nullptr)
		signal_at = poll_on(_signals->wakeup_fd());
	if (_watch_fd >= 0)
		watch_at = poll_on(_watch_fd);
	// AND THEN THE FD INTERESTS (#209), WHICH ARE NOT TOPICS. A topic is a fixed
	// descriptor with a drain behind it; an interest is one fiber's question about
	// one descriptor, for the length of one wait, with nothing behind it at all -
	// the waiter's own `::read` is what consumes the bytes. Nearly always none, so
	// the fast path is the poll set this loop has always built.
	//
	// THIS IS ALSO HOW THE TTY GETS BACK IN WHILE A COMMAND RUNS. The topic above
	// is out during `executing` (#208) and `await_readable(0)` from the `read`
	// builtin puts the same descriptor here instead - polled, never drained.
	for (std::size_t i = 0; i < _fd_wait_count; ++i)
		(void)poll_on(_fd_waits[i]->fd);
	// AND A POLL THAT HAS ALREADY FAILED KEEPS ONLY THE SIGNAL TOPIC (#211 §4.1).
	// One of the descriptors in the set has gone and the kernel does not say which;
	// the self-pipe is this process's own, and the SIGCHLD byte on it is the one
	// thing the foreground wait still needs. Narrowing rather than giving up is
	// what lets the turns go on until the command has finished.
	if (_poll_failed) {
		at = 0;
		tty_at = watch_at = -1;
		signal_at = _signals != nullptr ? poll_on(_signals->wakeup_fd()) : -1;
	}

	// AND THE CALLER'S TIMEOUT IS CLAMPED THE SAME WAY `poll_timeout_ms` IS
	// (#202). `turn()` computes its timeout and finds the zero there; `turn(ms)`
	// is what the tests and the paste path call, and a reactor mid-walk must not
	// be held behind somebody's 50 ms either.
	if (timeout_ms != 0 && _sched.runnable(sliced_lanes()))
		timeout_ms = 0;

	const int ready = _fail_polls > 0 ? (--_fail_polls, errno = EBADF, -1)
	                                  : ready_now(_poll.data(), at, timeout_ms);
	if (ready < 0) {
		if (errno != EINTR) {
			// #128's trap 1: a non-EINTR poll error is the terminal having gone,
			// not something to retry. fish treats exactly this as "the tty has
			// been closed".
			LESH_LOG(log::level::error, log::category::loop, "poll failed: %s",
			         std::strerror(errno));
			// BUT IT DOES NOT LEAVE WHILE A COMMAND IS RUNNING (#211 §4.1).
			// Returning here used to hand control back to `accept_current_line`
			// with the execution fiber suspended inside `execute`, and the
			// destructor then unmapped that stack without unwinding it: the
			// executor's frames leaked, its redirection fds stayed open, its child
			// was orphaned and the executing flag was never lowered. The child
			// still exits and SIGCHLD still rings the self-pipe, so the loop keeps
			// turning on the signal topic alone (see the narrowing above) until the
			// status is back, and `run_the_line` leaves then.
			if (has_execution_fiber_mid_command()) {
				// STILL FAILING WITH THE SET ALREADY DOWN TO THE SELF-PIPE. There
				// is nothing left to wait on, so the turn sleeps for a slice
				// instead of spinning a core and the waiter table below does the
				// waiting by asking `waitpid` directly.
				if (_poll_failed)
					(void)::poll(nullptr, 0, kBlindWaitMs);
				_poll_failed = true;
			} else {
				_exiting = true;
				result.exiting = true;
				return result;
			}
		}
		// EINTR means a signal landed. The self-pipe has the byte; fall through
		// and drain the topics with everything marked not-ready, which the
		// signal drain does not need.
		for (int i = 0; i < at; ++i)
			_poll[static_cast<std::size_t>(i)].revents = 0;
	}
	result.timed_out = ready == 0;

	const leshper::input_instant now = clock::now();

	// TTY FIRST. fish's `readb` gives stdin explicit priority over its other two
	// descriptors - "This gives priority to the foreground" - and the ordering
	// is also what keeps a signal from tearing a multibyte sequence: the bytes
	// in hand are fed to the decoder before any injected event is appended, so a
	// half-read codepoint stays inside the decoder rather than having a resize
	// spliced through it.
	if (tty_at >= 0 && revents_of(_poll[static_cast<std::size_t>(tty_at)]) != 0) {
		drain_tty(now, result);
	}
	// The ESC disambiguation, whether or not anything arrived: `expire` re-reads
	// `now` and declines if the deadline has not passed, so an early wake cannot
	// resolve a sequence that is still legitimately in flight (#111). Nothing to
	// disambiguate while a command runs: `quiesce` reset the decoder and no byte
	// has reached it since.
	if (_phase != phase::executing)
		_decoder.expire(now, _events);

	if (signal_at >= 0
	    && (ready < 0 || revents_of(_poll[static_cast<std::size_t>(signal_at)]) != 0))
		drain_signal_topic(result);
	// THE FOREGROUND WAIT'S OTHER HALF (#208). The SIGCHLD byte is what made the
	// poll come back; this is where the awaited children are reaped and their
	// fibers woken. Guarded on the table rather than on the phase, so a wait taken
	// from anywhere else would be served the same way, and costing one
	// `waitpid(WNOHANG)` per outstanding wait per wake.
	if (!_child_waits.empty())
		reap_awaited_children();
	// AND THE INPUT WAIT'S OTHER HALF (#209), on exactly the same terms: guarded
	// on the table rather than on the phase, so a wait taken from anywhere would
	// be served the same way, and costing one zero-timeout `select` per
	// outstanding wait per wake.
	if (_fd_wait_count != 0)
		wake_readable_fds();
	// AND NO TIMERS WHILE A COMMAND RUNS. A timer expiring here would dispatch an
	// action into an editor that has no terminal; the arming survives, and the
	// first turn after `resume_after_execution` fires whatever is due.
	if (_phase != phase::executing)
		fire_timers(now, result);
	// LAST, AND IT PRODUCES NO EVENT. Everything above turns a descriptor into
	// something the editor sees; the watch turns one into a fact about a file the
	// editor has never heard of. Last because it is the least urgent thing in a
	// turn - a history that is one turn out of date is a history that is one turn
	// out of date - and because running it after the drains means the view it
	// swaps in is the one the events below will read.
	if (watch_at >= 0 && revents_of(_poll[static_cast<std::size_t>(watch_at)]) != 0)
		drain_watch_topic(result);

	// SWAPPED OUT BEFORE THE WALK, the way `drain_registry_effects` does it
	// (#162). `handle` PUSHES onto `_events` - the shell reactor's `worker_result`
	// does it from inside `notify_reactors`, and a shell message drained inside a
	// blocked `wait_on_shell` was the case that found it - and a range-for over
	// the vector being appended to reads freed memory the moment the push
	// reallocates: a heap-use-after-free ASan catches. The push lands in the
	// emptied `_events` now and the outer pass picks it up, so nothing is dropped
	// and nothing dangles.
	// AND THE TRAILING SLICES, BETWEEN THE TWO WALKS (#202). The owner asked for
	// "reactor slices before and after the UI part"; the record adds that whatever
	// a trailing slice emits may land on the next (immediate) turn. It lands on
	// THIS one, and the reason is not ambition: `turn` clears `_events` and
	// `_needs_render` at the top, so a `worker_result` pushed after the last walk
	// and a repaint asked for after the last render would BOTH be dropped rather
	// than deferred. Walking once more after the slices is three lines and keeps
	// #201's property - the highlight lands in the turn that produced the
	// keystroke, one paint where there used to be two.
	for (bool sliced = false;;) {
		while (!_events.empty()) {
			_carried_events.clear();
			_carried_events.swap(_events);
			for (const leshper::event& one : _carried_events) {
				handle(one, result);
				if (_exiting)
					break;
			}
			result.events += _carried_events.size();
			if (_exiting)
				break;
		}
		if (sliced || _exiting)
			break;
		sliced = true;
		(void)tick_fibers();
	}

	// NO RENDER WHILE A COMMAND RUNS (#208). The screen is the command's, the
	// terminal is out of raw mode, and the frame that goes back up is the full
	// repaint `resume_after_execution` asks for.
	if (_needs_render && !_exiting && _phase != phase::executing) {
		render();
		result.rendered = true;
	}
	result.exiting = _exiting;
	return result;
}

void event_loop::drain_tty(leshper::input_instant now, turn_result& result) {
	++result.topics_drained;
	_read_buffer.clear();

	bool hangup = false;
	for (;;) {
		char chunk[4096];
		const ssize_t n = ::read(_fds.input, chunk, sizeof(chunk));
		if (n > 0) {
			_read_buffer.append(chunk, static_cast<std::size_t>(n));
			if (_read_buffer.size() >= _options.readahead_limit)
				break;
			// #128's trap 4, fish's `read_normal_chars`: batch while a
			// ZERO-TIMEOUT poll says the fd is still readable, so a paste is one
			// edit and one repaint while a typed character paints now. The poll
			// is what makes it "while there is more", not "up to N bytes".
			// AND THIS ONE IS ON THE KEYSTROKE PATH (#206): a single typed
			// character is one read that gets it and one readiness check that
			// finds nothing more, so the 8 us `poll` costs when nothing is ready
			// was being paid once per keystroke as well as once per yield.
			struct pollfd again{};
			again.fd = _fds.input;
			again.events = POLLIN;
			if (ready_now(&again, 1, 0) == 1 && (again.revents & kReadable) != 0)
				continue;
			break;
		}
		if (n == 0) {
			// EOF ON THE TERMINAL IS A HANGUP, NOT A KEY (fish `input.cpp`:
			// `reader_sighup`). Ctrl-D at an empty prompt is a BINDING, decided
			// by the keymap on a real U+0004; this is the descriptor closing.
			hangup = true;
			break;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			break;
		LESH_LOG(log::level::error, log::category::loop, "tty read failed: %s",
		         std::strerror(errno));
		hangup = true;
		break;
	}

	if (!_read_buffer.empty())
		_decoder.feed(_read_buffer, now, _events);

	if (hangup) {
		_events.push_back(leshper::signal_event{SIGHUP});
		_exiting = true;
	}
}

void event_loop::drain_signal_topic(turn_result& result) {
	if (_signals == nullptr)
		return;

	_signal_numbers.clear();
	// THE PIPE FIRST, then the counter, then the ioctl. Draining before reading
	// the counter is what keeps a SIGWINCH that lands during this function from
	// being lost: its byte is written after our read, so the next poll wakes,
	// and its counter bump is after our read, so the next turn re-queries.
	const std::size_t signals = _signals->drain(_signal_numbers);
	const unsigned resizes = _signals->resize_count();

	if (signals != 0 || resizes != _resizes_seen)
		++result.topics_drained;

	// DRAINED, AND THEN DROPPED, WHILE A COMMAND RUNS (#208). The byte was the
	// point: it woke the poll so the awaited children can be reaped. What must NOT
	// happen is an event - a SIGINT turned into `cancel_line` here would call
	// `execute` a second time from inside the first, and a resize dispatched into
	// an editor with no terminal would paint over the command's output. Both are
	// answered where they always were: the shell's own handler chain set
	// `g_pending` for the signal (#134), and `resume_after_execution` re-reads the
	// resize counter and the winsize on the way back.
	if (_phase == phase::executing) {
		for (const int signo : _signal_numbers) {
			// A LEVEL AND NOT A COUNT, exactly as the hub's own `_pending` is, which
			// is what bounds this list by the number of signals that exist.
			if (std::find(_deferred_signals.begin(), _deferred_signals.end(), signo)
			    == _deferred_signals.end())
				_deferred_signals.push_back(signo);
		}
		LESH_LOG(log::level::debug, log::category::loop,
		         "topic=signal during execution: %zu signal(s) deferred, %u resize(s)",
		         signals, resizes);
		return;
	}

	if (resizes != _resizes_seen) {
		// #128's trap 12: the counter was read BEFORE this ioctl.
		_resizes_seen = resizes;
		const terminal_size size = _terminal.size();
		_events.push_back(leshper::resize_event{size.columns, size.rows});
	}

	for (int signo : _signal_numbers) {
		LESH_LOG(log::level::debug, log::category::loop, "topic=signal signo=%d", signo);
		_events.push_back(leshper::signal_event{signo});
	}
}

void event_loop::fire_timers(leshper::input_instant now, turn_result& result) {
	// A timer can only be in the table because a registry produced the `arm_timer`
	// that put it there, so the second half of this is an invariant rather than a
	// case - written down because the log line below resolves a handle through it.
	if (_timers.empty() || _registry == nullptr)
		return;

	for (timer_due& armed : _timers) {
		if (armed.due > now)
			continue;
		// Rearmed from NOW rather than from the missed due instant: a loop that
		// was blocked in an execution for a minute must not then fire a
		// one-second timer sixty times.
		armed.due = now + std::chrono::milliseconds(armed.interval_ms);
		++result.topics_drained;
		++_timer_dispatches;
		// The name is resolved ONLY here, and `LESH_LOG` evaluates its arguments
		// only when the category is on: an expiry that nobody is logging costs a
		// due-instant comparison and an event push.
		LESH_LOG(log::level::debug, log::category::loop, "topic=timer id=%llu action=%.*s",
		         static_cast<unsigned long long>(armed.id),
		         static_cast<int>(leshper::timer_action_name(*_registry, armed.action).size()),
		         leshper::timer_action_name(*_registry, armed.action).data());
		// AN EVENT, NOT A DISPATCH (#168). What the host knows is that a timer
		// came due; which action that is and how to run it is the editor's, and
		// `step` runs it through the same registry a keystroke reaches. The loop
		// used to invoke it here, through a `loop_harness` of its own, which is
		// how a timer expiry and a key came to run through two different objects.
		_events.push_back(leshper::timer_fired{armed.id, armed.action});
	}
}

void event_loop::take_batch(leshper::reactor_batch& answer) {
	// THE DROP RULE lives in `apply_batch` (N-4, ADR-0008), which is the one
	// applier both this and the harness path go through (#144). A batch computed
	// against a generation the editor has moved past is not rejected so much as
	// it has nowhere to go: nothing else applies batches.
	if (!apply_batch(_state, answer)) {
		++_dropped;
		LESH_LOG(log::level::debug, log::category::reactor,
		         "dropped %s: gen=%llu editor=%llu", answer.reactor.c_str(),
		         static_cast<unsigned long long>(answer.computed_against.value()),
		         static_cast<unsigned long long>(_state.gen.value()));
		return;
	}

	// AND IT HAS LANDED (#141, #144). Both halves of the batch went into the
	// editor's own state: the spans and virtual text into `marks`, which is what
	// `lay_out` reads, and the proposals into `proposals`, which is what
	// `lesh_proposal_read` walks. So the trail from a reactor's emit runs to a
	// coloured cell AND to what an accepting action puts in the buffer, and both
	// end in the same object.
	//
	// #141 left the second half out, on the reading that proposals belong to the
	// dispatching harness rather than to the decoration store. The first half of
	// that is right - a proposal is what an action would ACCEPT, not something
	// anchored to a buffer position - and the second half was the defect #144
	// fixed: the harness the loop dispatches through was not the harness that
	// held them, so the shell painted a suggestion no action could read.
	++_applied;
	_needs_render = true;
}

void event_loop::drain_watch_topic(turn_result& result) {
	if (_watch_hook == nullptr)
		return;
	++result.topics_drained;
	++_watch_drains;
	LESH_LOG(log::level::debug, log::category::loop, "topic=watch fd=%d", _watch_fd);
	// THE HOOK CONSUMES THE DESCRIPTOR, not this function - see `attach_watch`.
	// The loop never reads the fd, because it does not know what a read off it
	// means: an inotify record and a kevent are two different shapes and neither
	// is the loop's business.
	_watch_hook(_watch_userdata);
}

// ---------------------------------------------------------------------------
// One event
// ---------------------------------------------------------------------------

void event_loop::handle(const leshper::event& incoming, turn_result& result) {
	// EVERYTHING BEFORE AND AFTER, so `selection_changed` can be fired by
	// COMPARISON. #116 deliberately left the state emitting no events - it is a
	// data structure - so noticing that a region moved is the loop's job, and
	// doing it by comparison rather than by having every action remember to
	// announce itself is what makes it impossible to forget.
	leshper::effects interrupted;

	const leshper::generation before_gen = _state.gen;
	const leshper::position before_cursor = _state.cursor;
	const leshper::position before_anchor = _state.selection_anchor();
	const bool before_active = _state.selection_active();

	// KEYBOARD INTERRUPT, BEFORE THE EDITOR SEES IT (#98 decision 2). Ctrl-C in
	// raw mode never arrives as a byte - ISIG stays on precisely so the kernel
	// turns it into SIGINT - so it can never reach a keymap, and `step` says as
	// much: the entrance for a signal exists and the binding does not. The
	// binding is HERE, by action name, so it is still the rebindable
	// `cancel_line` of F-13 and not a hardcoded behaviour.
	if (const auto* signal = std::get_if<leshper::signal_event>(&incoming);
	    signal != nullptr && signal->signal_number == SIGINT && !_options.interrupt_action.empty()) {
		leshper::editing_context& context = context_of(_state);
		leshper::action_result what =
			context.loop().invoke(_state, _options.interrupt_action, leshper::invocation{});
		if (what.status == LESH_ERR_NOTFOUND)
			LESH_LOG(log::level::warn, log::category::dispatch,
			         "no action registered for the interrupt: %s",
			         _options.interrupt_action.c_str());
		else
			// Its effects are this turn's too: the interrupt action is the
			// rebindable `cancel_line`, and what it asks for arrives the way every
			// other request does now rather than through a latch (#168).
			interrupted = std::move(what.produced);
	}

	// `step` logs the event itself, at #109's `event` category and into the
	// replay file - the one event serialization, and the reason this loop does
	// not write a second one for the events that reach the editor.
	const leshper::effects produced = step(_state, incoming);

	std::uint32_t kinds = 0;
	if (!(_state.gen == before_gen))
		kinds |= LESH_EVENT_BUFFER_CHANGED;
	if (!(_state.cursor == before_cursor))
		kinds |= LESH_EVENT_CURSOR_MOVED;
	if (!(_state.selection_anchor() == before_anchor)
	    || _state.selection_active() != before_active)
		kinds |= LESH_EVENT_SELECTION_CHANGED;

	if (kinds != 0)
		notify_reactors(kinds);

	// EVERYTHING THE TURN ASKED FOR, IN ORDER (#168). What an action requested -
	// accept, cancel, exit - is an effect in this list now, so there is no second
	// pass afterwards that reaches back into the editor for a latched outcome.
	// Two verbs on one key still means the second one wins, because the effects
	// are carried out in the order they were produced.
	carry_out(interrupted);
	carry_out(produced);
	(void)result;
}

void event_loop::carry_out(const leshper::effects& produced) {
	// A SWITCH ON THE VARIANT'S INDEX, not a chain of `holds_alternative`. The
	// chain was nine sequential type comparisons for what is one jump table, on a
	// path that runs per effect per keystroke. `effect_index` keeps the labels
	// spelled as TYPES, so reordering the variant moves the labels with it rather
	// than silently re-pointing them.
	for (const leshper::effect& one : produced) {
		switch (one.index()) {
			case effect_index<leshper::render_request>:
				_needs_render = true;
				break;

			case effect_index<leshper::worker_request>:
				// The fan-out already happened in `handle`, from the comparison -
				// which is strictly more information than this effect carries, since
				// it knows WHICH of the three kinds changed. Nothing to do.
				break;

			case effect_index<leshper::spawn_request>: {
				// #92's lane 2. The implementation is the shell side's (#134 wires
				// it); what the loop owes is not to silently drop it.
				const auto& spawn = std::get<leshper::spawn_request>(one);
				LESH_LOG(log::level::warn, log::category::spawn,
				         "spawn_request with no spawner attached: %s",
				         spawn.argv.empty() ? "" : spawn.argv.front().c_str());
				break;
			}

			case effect_index<leshper::line_accepted>:
				accept_current_line();
				break;

			case effect_index<leshper::line_cancelled>:
				// #98 decision 2: discard the buffer, paint the indicator, fresh
				// prompt. `$?` = 130 and the user's INT trap are the SHELL's, and
				// `finish_cancelled_line` is how they are asked for - the loop still
				// does not own exit statuses, it owns the handoff.
				if (_options.manage_terminal || _fds.output >= 0)
					write_all(_fds.output, "^C\r\n");
				apply_edit(_state, leshper::position{}, _state.buffer.end_position(), "");
				_state.undo.break_coalescing();
				// The same rule accept follows, on both halves of what was applied.
				_state.marks.clear();
				_state.proposals.clear();
				_have_previous = false;
				_needs_render = true;
				finish_cancelled_line();
				break;

			case effect_index<leshper::end_of_file>:
				_exiting = true;
				_exit_status = std::get<leshper::end_of_file>(one).status;
				break;

			case effect_index<leshper::recursive_edit_request>:
				// F-18's recovery shape. Nothing in v1 nests reads, and inventing
				// half of it here would be building the thing its ticket must.
				LESH_LOG(log::level::warn, log::category::dispatch,
				         "recursive_edit requested; not implemented in v1");
				break;

			case effect_index<leshper::arm_timer>: {
				// Re-arming an id that is already here is not a thing the registry
				// can produce - ids are minted once and never reused - so this is an
				// append, and the due instant is put on at the moment the host hears
				// about it.
				const auto& arm = std::get<leshper::arm_timer>(one);
				_timers.push_back(
					timer_due{arm.id, arm.interval_ms, arm.action,
				              clock::now() + std::chrono::milliseconds(arm.interval_ms)});
				break;
			}

			case effect_index<leshper::disarm_timer>: {
				const std::uint64_t id = std::get<leshper::disarm_timer>(one).id;
				_timers.erase(std::remove_if(_timers.begin(), _timers.end(),
				                             [&](const timer_due& armed) {
					                             return armed.id == id;
				                             }),
				              _timers.end());
				break;
			}

			default:
				break;
		}
	}
}

void event_loop::drain_registry_effects() {
	if (_registry == nullptr || _registry->pending.empty())
		return;
	// SWAPPED OUT FIRST, then carried: an effect that arms a timer while this
	// runs would otherwise be walked by the loop that is draining the queue it
	// was appended to. The member keeps its capacity, which is what N-2 asks of
	// everything a turn touches.
	_carried.clear();
	_carried.swap(_registry->pending);
	carry_out(_carried);
}

void event_loop::refresh_dispatch_table() {
	_dispatch_table.clear();
	if (_registry == nullptr) {
		_dispatch_valid = false;
		return;
	}
	for (const auto& [name, entry] : _registry->reactors) {
		// ADR-0009: the highlighter reads the alias, function and builtin tables,
		// and that state has exactly one owner - which since #201 is this thread and
		// since #202 is a fiber on it. So what the comparison still decides is
		// WHOSE HOST is stamped on the token, and it is made HERE, once per table
		// change, rather than once per reactor per keystroke.
		//
		// AND THE FIBER IS SPAWNED HERE (#202), which is what the ticket means by
		// "spawned when the dispatch table is (re)built": `lane_for` creates the
		// lane and its fiber the first time a name is seen and hands back the
		// existing one on every rebuild after that. The fn and userdata are
		// refreshed from the registry on every rebuild, so a re-registered reactor
		// runs its new function on the fiber it already had.
		reactor_lane& lane = lane_for(name);
		lane.fn = entry.fn;
		lane.userdata = entry.userdata;
		_dispatch_table.push_back(reactor_dispatch{
			std::string_view{name}, entry.fn, entry.userdata, entry.event_mask,
			name == _options.shell_thread_reactor, &lane});
	}
	_dispatch_built_from = _registry;
	_dispatch_generation = _registry->reactors_generation;
	_dispatch_shell_reactor = _options.shell_thread_reactor;
	_dispatch_valid = true;
}

void event_loop::notify_reactors(std::uint32_t kinds) {
	if (_registry == nullptr)
		return;
	// THE STALENESS CHECK IS THE STEADY STATE. Two scalar comparisons and one
	// short string comparison, against a map walk plus a string comparison per
	// entry - and the rebuild below runs only when a binding registers a reactor
	// or the caller renames the shell-thread one.
	if (!_dispatch_valid || _dispatch_built_from != _registry
	    || _dispatch_generation != _registry->reactors_generation
	    || _dispatch_shell_reactor != _options.shell_thread_reactor)
		refresh_dispatch_table();

	for (const reactor_dispatch& one : _dispatch_table) {
		const std::uint32_t served = one.event_mask & kinds;
		if (served == 0)
			continue;
		LESH_ASSERT(one.lane != nullptr);
		reactor_lane& lane = *one.lane;
		if (lane.fn == nullptr)
			continue;

		// INTO THE LANE'S OWN STORAGE, so the fan-out allocates nothing warm:
		// `take_snapshot` assigns into the buffer the last keystroke grew.
		take_snapshot(lane.arriving, _state, served);
		// THE STAMP (#151). Not the snapshot-taker's and not the reactor's: the loop
		// serves one shell, and this is that shell's door, on every token it mints
		// for the reactor that reads shell state. Every other reactor is state-free
		// and gets the honest null `take_snapshot` just wrote.
		if (one.on_shell_thread)
			lane.arriving.host = _shell_host;

		// AND THE SEND IS THE CANCELLATION. `slot::send` bumps the counter that
		// supersedes every outstanding token; the flag beside it is that counter as
		// the ABI's poll can read it (see `reactor_lane::superseded`). A reactor
		// mid-walk sees it at its next poll, abandons, and loops back to `recv`;
		// a reactor parked on `recv` is woken by the send itself.
		lane.superseded.store(true, std::memory_order_relaxed);
		lane.inbox.send(served);
	}
}

// ---------------------------------------------------------------------------
// The reactor fibers (#202, step 1d of #145)
// ---------------------------------------------------------------------------

std::uint8_t event_loop::sliced_lanes() noexcept {
	return static_cast<std::uint8_t>(group_mask(fiber_group::emitters)
	                                 | group_mask(fiber_group::execution));
}

bool event_loop::tick_fibers() {
	// ONE MASK FOR BOTH LANES, and no phase test, because the scheduler's own park
	// bit already answers it: while a command runs the emitters' group is parked
	// and skipped, and while a line is being edited the execution fiber is parked
	// on its inbox. Two sets that are never runnable at once are one tick.
	return _sched.tick(sliced_lanes());
}

reactor_lane& event_loop::lane_for(std::string_view name) {
	for (const std::unique_ptr<reactor_lane>& each : _lanes) {
		if (each->name == name)
			return *each;
	}
	_lanes.push_back(std::make_unique<reactor_lane>(*this, _sched, name));
	reactor_lane& made = *_lanes.back();
	// THE NAME IS THE LANE'S OWN COPY, because `spawn` requires a name that
	// outlives the scheduler and the registry's key only outlives the table.
	//
	// SPAWNED INTO `emitters`, and spawning into a PARKED group is legal (#200):
	// the fiber is ready, is not runnable, and takes its first slice at the resume.
	// So a reactor registered from inside a command - a binding sourced by an rc
	// file, say - is not a special case.
	made.self = &_sched.spawn(&event_loop::reactor_body, &made, made.name.c_str(),
	                          group_index(fiber_group::emitters));
	LESH_LOG(log::level::debug, log::category::reactor,
	         "reactor fiber spawned: %s", made.name.c_str());
	return made;
}

void event_loop::reactor_body(fiber::scheduler& on, void* userdata) {
	reactor_lane& lane = *static_cast<reactor_lane*>(userdata);
	(void)on;
	// FOR EVER. A reactor fiber is never called and never returns: it parks on its
	// slot, computes what it is sent, applies the batch, and parks again (#145's
	// pinned rule - "no fiber call stack"). The host is the only resumer, and the
	// only way out is the process ending or `~scheduler` unmapping the stack, which
	// `drain_emitters` makes sure happens with the fiber parked on `recv`.
	for (;;) {
		const std::uint32_t kinds = lane.inbox.recv();
		// CLEARED RIGHT AFTER `recv`, which is what makes the flag equal to the
		// token's own answer: `recv` minted a fresh token, so nothing outstanding is
		// superseded until the next send.
		lane.superseded.store(false, std::memory_order_relaxed);
		// AND THE HOST'S WRITE BUFFER BECOMES THE COMPUTE'S. Both keep their
		// capacity, so the next notification assigns into the string this compute is
		// about to give back rather than allocating one.
		std::swap(lane.arriving, lane.computing);
		LESH_ASSERT(lane.computing.event_kind == kinds
		            && "the slot's notification and the snapshot it arrived with disagree");
		(void)kinds;
		++lane.computes;
		run_reactor_here(lane.name, lane.fn, lane.userdata, lane.computing,
		                 lane.superseded, lane.batch,
		                 reactor_cooperation{&event_loop::reactor_yield, &lane});
		lane.owner->apply_reactor_batch(lane);
	}
}

void event_loop::reactor_yield(void* userdata) {
	reactor_lane& lane = *static_cast<reactor_lane*>(userdata);
	++lane.yields;
	// THE YIELD IS THE WHOLE OF IT. The host gets the thread back, polls the
	// terminal, dispatches whatever arrived - and a keystroke that changed the
	// buffer sends into this very slot on its way through, which is what the poll
	// this call is inside of is about to read.
	lane.owner->_sched.yield();
	// AND THE FLAG STILL COVERS THE SLOT (see `reactor_lane::superseded`).
	//
	// AN IMPLICATION AND NOT AN EQUALITY, which is the honest invariant: a SEND
	// must never fail to raise the flag - that direction is the whole of #90's
	// cancellation and the one a broken send site would break - while the flag is
	// deliberately a superset, because `quiesce()` and `drain_emitters()` raise it
	// with nothing sent. That is #115's lever kept: parking supersedes what is in
	// flight, through the poll the ABI already has, rather than waiting it out.
	//
	// Asserted HERE because this is the one place both are observable at a moment
	// the host has just had the thread, so a send that bumped the counter without
	// raising the flag fails on the next poll rather than in a stale highlight
	// nobody can reproduce.
	LESH_ASSERT((!lane.inbox.superseded() || lane.superseded.load(std::memory_order_relaxed))
	            && "a send left this reactor's cancellation flag down");
}

void event_loop::apply_reactor_batch(reactor_lane& lane) {
	// THE RECEIVER'S HALF OF THE DROP RULE, and it is two rules deep on purpose.
	//
	// The first is the token's: a batch computed under a superseded token is for a
	// line the user has left, and it is not applied at all - which is what makes
	// "an emission computed for the dead line is never applied" true at accept,
	// where `quiesce` raised every flag before parking the group.
	//
	// The second is `apply_batch`'s generation rule (N-4, ADR-0008), inside
	// `take_batch`, and it stays exactly where it was: it is the one applier both
	// this path and `loop_harness`' go through.
	if (lane.superseded.load(std::memory_order_relaxed)
	    || lane.batch.status == LESH_ERR_SUPERSEDED) {
		++lane.abandoned;
		LESH_LOG(log::level::debug, log::category::reactor,
		         "abandoned %s: gen=%llu", lane.name.c_str(),
		         static_cast<unsigned long long>(lane.batch.computed_against.value()));
		return;
	}
	LESH_LOG(log::level::debug, log::category::reactor,
	         "reactor %s gen=%llu status=%d spans=%zu", lane.name.c_str(),
	         static_cast<unsigned long long>(lane.batch.computed_against.value()),
	         static_cast<int>(lane.batch.status), lane.batch.spans.size());
	take_batch(lane.batch);
	// AND THE EDITOR HEARS ABOUT THE ARRIVAL: `step` carries the same drop rule,
	// emits the redraw, and the replay file records it (#109's `event`). Pushed
	// from inside a slice, which `turn` runs BETWEEN its two event walks - so this
	// push cannot reallocate a vector somebody is walking, and the walk that
	// follows the trailing slices is what picks it up.
	_events.push_back(leshper::worker_result{lane.batch.computed_against});
}

const reactor_lane* event_loop::lane_named(std::string_view reactor) const noexcept {
	for (const std::unique_ptr<reactor_lane>& each : _lanes) {
		if (each->name == reactor)
			return each.get();
	}
	return nullptr;
}

void event_loop::drain_emitters() {
	// EVERY EMITTER OUT OF ITS COMPUTE AND BACK ONTO `recv`. See the destructor for
	// why this is not optional. The group is resumed first because a parked group's
	// fibers are not runnable and `tick` would skip them; every flag is raised so
	// that a walk in progress abandons at its next poll rather than finishing.
	if (_sched.group_parked(group_index(fiber_group::emitters)))
		_sched.resume_group(group_index(fiber_group::emitters));
	for (const std::unique_ptr<reactor_lane>& each : _lanes)
		each->superseded.store(true, std::memory_order_relaxed);
	// BOUNDED, and the bound is a diagnostic rather than a policy: a reactor that
	// ignores its cancellation poll cannot be made to stop, and spinning here
	// for ever at shutdown would be worse than saying so and leaving.
	constexpr int ceiling = 4096;
	int slices = 0;
	while (_sched.runnable(group_mask(fiber_group::emitters))) {
		if (++slices > ceiling) {
			LESH_LOG(log::level::warn, log::category::reactor,
			         "a reactor fiber would not give up after %d slices", ceiling);
			break;
		}
		(void)_sched.tick(group_mask(fiber_group::emitters));
	}
}

std::size_t event_loop::reactor_slices(std::string_view reactor) const noexcept {
	const reactor_lane* const lane = lane_named(reactor);
	return lane == nullptr || lane->self == nullptr ? 0 : lane->self->slices();
}

std::size_t event_loop::reactor_computes(std::string_view reactor) const noexcept {
	const reactor_lane* const lane = lane_named(reactor);
	return lane == nullptr ? 0 : lane->computes;
}

std::size_t event_loop::reactor_abandoned(std::string_view reactor) const noexcept {
	const reactor_lane* const lane = lane_named(reactor);
	return lane == nullptr ? 0 : lane->abandoned;
}

std::size_t event_loop::reactor_yields(std::string_view reactor) const noexcept {
	const reactor_lane* const lane = lane_named(reactor);
	return lane == nullptr ? 0 : lane->yields;
}

std::uint64_t event_loop::reactor_sends(std::string_view reactor) const noexcept {
	const reactor_lane* const lane = lane_named(reactor);
	return lane == nullptr ? 0 : lane->inbox.sends();
}

std::size_t event_loop::reactor_superseded_sends(std::string_view reactor) const noexcept {
	const reactor_lane* const lane = lane_named(reactor);
	return lane == nullptr ? 0 : lane->inbox.superseded_sends();
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void event_loop::render() {
	if (_state.columns == 0 || _state.rows == 0) {
		// "There is no honest picture of a screen whose size is unknown"
		// (layout.h). Not an error: the size arrives with the first read entry.
		return;
	}

	leshper::layout_input in = input_for(_state, _options.prompt, _options.continuation);
	in.width = _options.width;
	in.prompt_pen = _options.prompt_pen;
	in.text_pen = _options.text_pen;

	// The theme, caught up with whatever the registry has interned since the
	// last repaint. Here rather than at `attach_registry` because a binding may
	// intern a name at any time, and incremental so that it is a size comparison
	// on every repaint after the first - which is what keeps the repaint pin in
	// allocation_tests.cpp a constant with highlighting in it.
	if (_registry != nullptr) {
		_theme.sync(_registry->styles);
		in.theme = &_theme;
	}

	leshper::layout desired = lay_out(_pool, in);

	_out.clear();
	if (_have_previous && can_diff(_previous, desired)) {
		_blitter.update_into(_previous.screen, desired.screen, _out);
	} else if (_have_previous) {
		// A REPAINT WITH A FRAME STILL ON SCREEN (#185, F-38): a resize, which is
		// the only thing that reaches here with a previous frame - `can_diff`
		// answers no exactly when the sizes differ, and every path that leaves
		// the screen showing something that is not ours drops `_have_previous`
		// instead. So there is a frame to REPLACE, and painting from where the
		// cursor happens to be is what put a second copy of the prompt below the
		// first, once per resize.
		const leshper::cursor_placement top = frame_top_above_cursor();
		LESH_LOG(log::level::debug, log::category::render,
		         "repaint: the frame's top is %u row(s) up (%s)",
		         static_cast<unsigned>(top.row),
		         _options.assume_reflow ? "reflowed" : "unreflowed");
		_blitter.paint_from(top, desired.screen, _out);
	} else {
		// The first paint of a read: the cursor IS at the surface's origin,
		// because the shell has just written the newline that put it there.
		_blitter.paint_into(desired.screen, _out);
	}

	if (!_out.empty() && _fds.output >= 0) {
		// #98: the redraw goes out through the loop, like every other byte the
		// terminal receives. The blitter produced them and touched no fd.
		if (!write_all(_fds.output, _out)) {
			LESH_LOG(log::level::error, log::category::render, "terminal write failed: %s",
			         std::strerror(errno));
			_exiting = true;
		}
	}

	_previous = std::move(desired);
	_have_previous = true;
	// The input beside the picture, so the next repaint can ask where the
	// terminal has since moved this frame's top row to (#185). `assign` into
	// strings that already have the capacity, which is what keeps the per-render
	// cost the constant `ALoopRepaintCostsAConstantThatDoesNotGrow` pins.
	_previous_input.prompt.assign(in.prompt);
	_previous_input.continuation.assign(in.continuation);
	_previous_input.buffer.assign(in.buffer);
	_previous_input.cursor = in.cursor;
	_previous_input.width = in.width;
	_previous_input.prompt_pen = in.prompt_pen;
	_previous_input.text_pen = in.text_pen;
	LESH_LOG_TRACE(log::category::render, "rendered %zu bytes", _out.size());
}

leshper::cursor_placement event_loop::frame_top_above_cursor() {
	// A NON-REFLOWING TERMINAL LEFT THE ROWS WHERE THEY WERE, so the frame's top
	// is as many rows up as it was before the resize - which is what the picture
	// we painted already says.
	if (!_options.assume_reflow)
		return _previous.screen.cursor();

	// A REFLOWING ONE REWRAPPED THEM, and the honest answer is the same input
	// laid out at the size the terminal is now: the row the cursor lands on IS
	// how far below that frame's top row the terminal's cursor now sits. Hard
	// newlines survive this for free - `lay_out` starts a new row at one either
	// way - which is why the answer is a re-layout rather than arithmetic on the
	// old row count.
	//
	// AND THE FRAME IT REWRAPPED IS THE ONE WE PAINTED (#189): the blitter writes
	// a soft-wrapped row through the right edge, so the terminal holds those rows
	// as one logical line and rewraps them exactly as this re-layout does. While
	// it did not - while every row was painted as a hard line - this count was
	// right about a frame nobody had drawn.
	leshper::layout_input was;
	was.prompt = _previous_input.prompt;
	was.continuation = _previous_input.continuation;
	was.buffer = _previous_input.buffer;
	was.cursor = _previous_input.cursor;
	was.width = _previous_input.width;
	was.prompt_pen = _previous_input.prompt_pen;
	was.text_pen = _previous_input.text_pen;
	was.columns = _state.columns;
	was.rows = _state.rows;
	return lay_out(_pool, was).screen.cursor();
}

// ---------------------------------------------------------------------------
// Quiesce
// ---------------------------------------------------------------------------

void event_loop::quiesce() {
	if (!_quiesced) {
		// THE EMITTERS DIE AT ACCEPT - "cancel, park", in the owner's words, and
		// not kill. Every flag is raised first, so a walk in progress abandons at
		// its next poll and `apply_reactor_batch` declines whatever it produced;
		// then the group's bit goes down, which makes every one of them unrunnable
		// in one store (#200). The fibers stay alive and own nothing of the dead
		// line, and the next line's first send is waiting for them at the resume.
		for (const std::unique_ptr<reactor_lane>& each : _lanes)
			each->superseded.store(true, std::memory_order_relaxed);
		// ONE OF THE TWO PHASE WRITES, and the derivation runs one way only: the
		// group bit is written FROM the phase and never independently.
		_phase = phase::executing;
		_sched.park_group(group_index(fiber_group::emitters));
		if (_options.manage_terminal)
			_terminal.leave_raw();
		_decoder.reset();
	}
	// IDEMPOTENT (#203): a second call finds the bit up and does nothing. Nothing
	// nests - see the header - so what used to be a depth counter is this bit.
	_quiesced = true;
}

void event_loop::resume_after_execution() {
	LESH_ASSERT(_quiesced && "a resume with nothing parked is a caller that lost track");
	_quiesced = false;
	// THE SECOND PHASE WRITE. `execute` has returned, so the history append has
	// already happened inside `session::execute` and the prompt is refreshed on
	// the way out of the command; `boundary` is that instant, and it is the phase
	// an `observers` group would still be runnable in.
	_phase = phase::boundary;
	if (_options.manage_terminal) {
		// The order is the read-entry order, because that is what this is: the
		// terminal comes back, then the modes, then the size.
		ignore_background_write_signals();
		_terminal.reclaim();
		_terminal.enter_raw();
	}
	// The counter only; see `enter_read`. The command that just ran may well have
	// been a `trap`, but the side that ran it re-asserts before it hands the loop
	// back - one thread writes dispositions (#142).
	if (_signals != nullptr)
		_resizes_seen = _signals->resize_count();
	refresh_size_from_terminal();
	// The screen is whatever the command left behind, so there is nothing to
	// diff against: the next render is a full repaint.
	_have_previous = false;
	_needs_render = true;
	// AND THE EDITOR IS BACK. The emitters are runnable again from here, and
	// each of them is parked on `recv` owning nothing of the line that just
	// ran - the supersede at `quiesce` is what made that true.
	_phase = phase::editing;
	_sched.resume_group(group_index(fiber_group::emitters));
}

void event_loop::assert_quiesced() const noexcept {
	// TWO CLAUSES SINCE #203, and both of them are checks rather than echoes. The
	// group's bit and the park flag are stores `quiesce()` made a few lines up -
	// asserting them proved that an assignment assigns. What is worth asserting
	// is what the code around them CLAIMS:
	//
	// NO EMITTER IS MID-SLICE. Structurally true - the host is the only resumer,
	// every yield returns there, and nothing a reactor can reach forks - and
	// #91 chose crash-on-violation over `pthread_atfork` precisely so that the day
	// somebody adds a fork site inside a slice, the sanitized gate says so.
	const fiber::fiber* const running = _sched.current();
	LESH_ASSERT(running == nullptr
	            || running->group() != group_index(fiber_group::emitters));
	(void)running;   // `LESH_ASSERT` is nothing in Release
	// The other half, and the one only the loop can check: a fork taken with the
	// terminal still in raw mode gives the child a terminal it cannot use.
	LESH_ASSERT(!_options.manage_terminal || !_terminal.raw());
}

// ---------------------------------------------------------------------------
// Accept, and the port
// ---------------------------------------------------------------------------

std::optional<std::int32_t> event_loop::accept_current_line() {
	_accepted.assign(_state.buffer.text());

	// The cursor is left wherever the layout put it; the command's output has to
	// start on a fresh row below the edit line (F-39 scrolls output above the
	// prompt, which only works if the prompt is the last thing on screen).
	if (_fds.output >= 0)
		write_all(_fds.output, "\r\n");
	_have_previous = false;

	// PARK, THEN RESTORE, THEN CALL. In that order, and the order is the whole of
	// quiesce: the fork happens inside `execute`, on this thread, so the emitters
	// have to be parked and the terminal handed back BEFORE the call is made.
	// #201 shortened the distance between the park and the fork from a channel to
	// a stack frame; it did not change what has to be true at it.
	quiesce();
	assert_quiesced();

	std::optional<std::int32_t> status;
	if (_shell != nullptr) {
		status = run_the_line(_accepted);
		_exit_status = *status;
	}

	// THE LINE THAT JUST RAN MAY HAVE BEEN AN `exit` (#152). Only the shell can
	// know that, and it says so by `request_stop()` from inside the call above -
	// so the flag is settled by the time it is read here, and reading it here is
	// the whole point: one line further down the loop lays out a fresh prompt for
	// a line nobody will ever type, paints it, and the process exits out from
	// under it, leaving the user's parent shell to start its own prompt after
	// `$ `. The unpark still happens - the park depth is a balance and
	// `leave_read` is owed a terminal to hand back - but the repaint does not.
	// OR THE LOOP ITSELF IS DONE (#211 §4.1): a poll that failed while the command
	// ran leaves `_exiting` up, and a fresh prompt painted onto a terminal that has
	// gone is the same mistake as one painted for a line nobody will type.
	const bool ending = _stopping || _exiting;

	resume_after_execution();

	if (ending) {
		_exiting = true;
		return status;
	}

	// The decorations go with the line they were computed for. Not because they
	// are stale - the drop rule would sort that out - but because they are
	// anchored to BYTE OFFSETS in a buffer that is about to be emptied, and a
	// suggestion left standing over an empty prompt is a suggestion for a line
	// that has already run.
	//
	// The offers go with them (#144). A proposal carries no offsets, so the first
	// reason does not reach it - but the second does, and more plainly: a
	// proposal is what this line would BECOME, and this line has just run.
	_state.marks.clear();
	_state.proposals.clear();

	// A fresh line: the accepted text is gone, and it is gone as ONE edit so
	// that undo does not walk back into a command that has already run.
	if (!_state.buffer.text().empty()) {
		apply_edit(_state, leshper::position{}, _state.buffer.end_position(), "");
		_state.undo.break_coalescing();
	}
	_needs_render = true;
	render();
	return status;
}

void event_loop::finish_cancelled_line() {
	// #98 decision 3, the zsh way: Ctrl-C at the prompt cancels the line AND
	// fires the user's INT trap, with `$?` = 130. Both are the shell's, and the
	// only door to the shell is `execute` - so a cancel is delivered as an EMPTY
	// line, which is exactly what it is: nothing to run, at a command boundary,
	// which is where #33 says a trap body belongs.
	//
	// THE QUIESCE IS NOT CEREMONY. A trap body is arbitrary shell code and may
	// fork, so the emitters have to be parked and the terminal handed back before
	// the call is made, on the identical path an accepted line takes.
	if (_shell == nullptr)
		return;
	quiesce();
	assert_quiesced();
	// THE STATUS IS KEPT, as it was when this went through the `execute` slot and
	// `wait_on_shell` recorded every `execute_done` it matched: a cancel reports
	// 130 (#98 decision 3), and the loop's copy of `$?` is what a test reads to
	// see that it did. Down the same door an accepted line takes, so an INT trap
	// body that forks forks from wherever an ordinary command would.
	_exit_status = run_the_line(std::string_view{});
	resume_after_execution();
}

// ---------------------------------------------------------------------------
// Running a line: the execution fiber, or the host's own stack (#208)
// ---------------------------------------------------------------------------

namespace {

// 8 MB OF RESERVE FOR THE EXECUTION FIBER, 16 under ASan - the same doubling
// `stack.h` applies to a reactor's, and for the same reason: instrumented frames
// are bigger and a guard-page fault is not a recoverable condition.
//
// RESERVE, NOT MEMORY. minicoro's mapping is committed page by page as the stack
// is touched, so a shell that never nests pays for the pages a `run_input`
// actually walks and nothing more. What the number buys is the case a 512 KB
// reactor stack could not carry: `tree_walking_executor` recurses through the
// node tree, and a deeply nested script - a function calling a function inside a
// `while` inside a `case` - is frames all the way down. Eight megabytes is what
// a thread gets on Linux by default, so it is also the depth the same script
// survives in a non-interactive shell.
[[nodiscard]] std::size_t execution_stack_bytes() noexcept {
	return fiber::built_under_asan() ? 16u * 1024u * 1024u : 8u * 1024u * 1024u;
}

} // namespace

void event_loop::execution_body(fiber::scheduler& on, void* userdata) {
	event_loop& me = *static_cast<event_loop*>(userdata);
	(void)on;
	// FOR EVER, like a reactor's (ADR-0011's "there is no fiber call stack"): it
	// parks on its inbox, runs the line it is sent, sends the status back and
	// parks again. It is never called and never returns, so the host stays the
	// sole resumer and every yield inside `execute` - which is what a foreground
	// wait's park is - comes back to `turn`.
	for (;;) {
		const std::string_view line = me._exec_inbox.recv();
		std::int32_t status = 0;
		if (me._shell != nullptr) {
			// ADR-0009's one writer, ANNOUNCED (#151): this is where a `PATH=`
			// assignment, an `alias`, a function definition or an `unset` actually
			// happens. The flag is up for the whole command now - including the
			// turns the host takes while this fiber is parked in a wait - and that
			// is exactly right: the shell IS writing for the whole of it, and the
			// emitters that would read through the adapter are a parked group.
			const shell_writing_flag::scope writing{me._writing};
			status = me._shell->execute(line);
		}
		me._exec_done.send(status);
	}
}

std::int32_t event_loop::run_the_line(std::string_view line) {
	if (_options.execution == execution_mode::inline_) {
		// THE DIRECT CALL, KEPT FIRST-CLASS. `execute` runs on this stack,
		// `scheduler::current()` is null throughout, and every `wait_child` under
		// it is a blocking `::waitpid` - the shell exactly as it ran before this
		// ticket, and the honest fallback if a fiber stack ever turns out to be
		// the wrong place to fork from.
		const shell_writing_flag::scope writing{_writing};
		return _shell->execute(line);
	}

	// SPAWNED ON THE FIRST LINE AND NEVER AGAIN. A loop that never accepts one -
	// which is most of the editor's own tests - reserves no stack at all.
	if (_execution == nullptr) {
		_execution = &_sched.spawn(&event_loop::execution_body, this, "execution",
		                           group_index(fiber_group::execution),
		                           execution_stack_bytes());
		LESH_LOG(log::level::debug, log::category::exec,
		         "execution fiber spawned: stack=%zu", execution_stack_bytes());
	}

	// A VIEW OF `_accepted`, WHICH IS A MEMBER. The slot's own debug assert covers
	// the hazard this would otherwise be (a message pointing into the SENDER's
	// fiber stack); the sender here is the host, whose stack outlives everything.
	_exec_inbox.send(line);

	// AND THE HOST KEEPS TURNING. This is the whole of what the ticket buys: the
	// signal topic and the watch are polled, the execution fiber gets its slices,
	// and the wait at the bottom of the interpreter is a park rather than a
	// blocked thread. The tty and the timers are out of the turn - see `turn`.
	//
	// RE-ENTRANT `turn`, AND IT IS SAFE BY EXCLUSION. The outer turn is walking
	// `_carried_events` when it reaches `accept_current_line`; a turn taken from
	// here pushes nothing onto `_events` (the signal drain drops its events while
	// `executing` and no other topic is polled), so the swap that would move the
	// outer walk's storage never happens.
	// AND THE CONDITION IS THE ANSWER AND NOTHING ELSE (#211 §4.1). It used to be
	// `&& !_exiting`, which made a fatal poll error during a command a way of
	// leaving with the fiber suspended inside `execute` - see `turn`. Nothing else
	// sets `_exiting` while `executing`: the tty topic is out of the poll set, no
	// event is dispatched, and nothing renders. So this terminates when the command
	// does, which is what it means for a command to be running.
	while (_exec_done.empty())
		turn();

	// AND NOW WE LEAVE, if the terminal went away while the command ran. The turns
	// above happened only so that the fiber could finish and give its stack back;
	// there is nothing left to edit on.
	if (_poll_failed)
		_exiting = true;

	// `try_recv` and not `recv`, because the host has no stack to park - and the
	// answer is always there now, because the loop above waits for it.
	return _exec_done.try_recv().value_or(0);
}

pid_t event_loop::await_child(pid_t pid, int flags, int* status) noexcept {
	// NO SIGCHLD, NO WAKE - so do the thing that always worked (#208).
	//
	// The park below is paid for by exactly one wake, and that wake is the hub's
	// self-pipe byte. `signal_hub::reassert`'s rule 3 leaves an inherited SIG_IGN
	// and a user's `trap '' CHLD` alone, both of which are legitimate and both of
	// which mean nothing will ever ring the pipe again - and with SIGCHLD ignored
	// the kernel reaps children itself, so even a poll that woke for another
	// reason would find nothing. Asking the kernel per foreground command is a
	// syscall nowhere near a keystroke, and the alternative is a shell that hangs
	// on `trap '' CHLD; sleep 1`.
	if (_signals == nullptr || !_signals->catches(SIGCHLD))
		return ::waitpid(pid, status, flags);

	child_wait waiting;
	waiting.pid = pid;
	waiting.flags = flags;
	waiting.status = status;
	return static_cast<pid_t>(_sched.block_or_park(
		waiting.slot,
		// No fiber to park: an action's `port_call`, the EXIT trap after `run()`
		// has returned, or `execution_mode::inline_`.
		[&] { return ::waitpid(pid, status, flags); },
		[&] {
			_child_waits.push_back(&waiting);
			// AND ASK ONCE, RIGHT NOW. The child may already be a zombie - `sleep
			// 0.1 & wait` is the ordinary shape - and the SIGCHLD that said so may
			// have been drained turns ago. `reap_awaited_children` completes the
			// slot in that case, and `block_or_park` then never parks.
			reap_awaited_children();
		}));
}

void event_loop::reap_awaited_children() noexcept {
	// ONLY AWAITED PIDS, NEVER `-1`. A background child stays a zombie until
	// `wait` asks for it, exactly as it did before this ticket, so nothing about
	// job control moves here - the "the shell notices mid-command" upgrade is a
	// later ticket built on this seam and not a side effect of it.
	//
	// WNOHANG ON TOP OF THE CALL SITE'S OWN FLAGS, which is what keeps `WUNTRACED`
	// meaning what it means: a foreground command stopped by Ctrl-Z reports here
	// exactly as it reported to the blocking wait, and the executor's stop path
	// runs unchanged.
	for (std::size_t i = 0; i < _child_waits.size();) {
		child_wait& waiting = *_child_waits[i];
		int wait_status = 0;
		const pid_t got = ::waitpid(waiting.pid, &wait_status, waiting.flags | WNOHANG);
		if (got == 0)
			{ ++i; continue; }              // still running: leave it enlisted
		if (got < 0 && errno == EINTR)
			continue;                       // ask again for the same waiter
		// `> 0` is an exit or a stop; `< 0` is ECHILD, which is `waitpid`'s own
		// answer and the one the blocking call would have returned. Either way the
		// wait is over, so the entry goes before the waiter is woken - a woken
		// fiber may enlist again on its very next statement.
		if (waiting.status != nullptr)
			*waiting.status = wait_status;
		_child_waits.erase(_child_waits.begin() + static_cast<std::ptrdiff_t>(i));
		waiting.slot.complete(got);
	}
}

void event_loop::await_readable(int fd) noexcept {
	// A DESCRIPTOR THAT IS NOT ONE is not something to park on. The caller's read
	// will answer with EBADF, which is what it would have answered anyway.
	if (fd < 0)
		return;

	fd_wait waiting;
	waiting.fd = fd;
	(void)_sched.block_or_park(
		waiting.slot,
		// NO FIBER TO PARK, so there is nothing to say: `execution_mode::inline_`,
		// an action's `port_call`, a `read` in the EXIT trap after `run()` has
		// returned. The caller's `::read` blocks on this stack, exactly as it did
		// before this ticket - which is the same answer the no-op cooperation
		// gives every non-interactive shell.
		[] { return 0; },
		[&] {
			// THE ONE PLACE THE FIXED CAPACITY IS CHECKED, and an overflow degrades
			// rather than corrupts: no entry means no park, which means the blocking
			// read above. Unreachable with today's shapes - one waiter, one fd.
			if (_fd_wait_count >= kMaxFdWaits) {
				LESH_ASSERT(false && "more descriptors awaited than the table can hold");
				waiting.slot.complete(0);
				return;
			}
			_fd_waits[_fd_wait_count++] = &waiting;
			// AND ASK ONCE, RIGHT NOW - `await_child`'s move, for `await_child`'s
			// reason. A regular file is always readable and a pipe usually has the
			// rest of the line already in it, so the common `read` never parks at
			// all and never reaches a poll: `wake_readable_fds` completes the slot
			// here and `block_or_park`'s loop returns without a switch.
			wake_readable_fds();
		});
}

void event_loop::wake_readable_fds() noexcept {
	// ASKED PER WAITER RATHER THAN READ OFF THE TURN'S `revents`, which is what
	// lets this be one function with the two call sites `reap_awaited_children`
	// has - the enlist probe has no poll set to read - and costs a `select` with
	// a zero timeout per outstanding wait per wake (0.2 us; see `ready_now`).
	for (std::size_t i = 0; i < _fd_wait_count;) {
		struct pollfd one{};
		one.fd = _fd_waits[i]->fd;
		one.events = POLLIN;
		const int ready = ready_now(&one, 1, 0);
		if (ready == 0)
			{ ++i; continue; }              // nothing there yet: leave it enlisted
		if (ready < 0 && errno == EINTR)
			continue;                       // ask again for the same waiter
		// EVERYTHING ELSE MEANS "GO AND READ IT", and deliberately so. `ready > 0`
		// is POLLIN, or a POLLHUP/POLLERR that `revents_of` has always folded into
		// "go and drain it", or a POLLNVAL from a descriptor that is not open;
		// `ready < 0` is a poll that refuses the set at all. In every one of those
		// the caller's `::read` is the thing with the right answer - a byte, end of
		// file, or EBADF, which `read` reports as end of input - and a wait would
		// be for ever.
		fd_wait* const waiting = _fd_waits[i];
		// The entry goes BEFORE the waiter is woken, for `reap_awaited_children`'s
		// reason: a woken fiber enlists again on its very next statement, which for
		// this verb is the next byte of the same line.
		for (std::size_t at = i + 1; at < _fd_wait_count; ++at)
			_fd_waits[at - 1] = _fd_waits[at];
		--_fd_wait_count;
		waiting->slot.complete(1);
	}
}

std::size_t event_loop::execution_slices() const noexcept {
	return _execution != nullptr ? _execution->slices() : 0;
}

port_result event_loop::call_port(std::string_view code) {
	port_result answer;
	if (_shell == nullptr) {
		LESH_LOG(log::level::warn, log::category::exec, "port call with no shell attached");
		return answer;
	}

	// THE MODES DO NOT CHANGE (fish #7770). An action's shell code runs with the
	// editor's terminal, because restoring cooked modes around it would race new
	// input against the restore - and there is deliberately no call in tty.h
	// that would let this function do otherwise.
	//
	// NO QUIESCE EITHER, and that is not an omission: #92's lane discipline
	// refuses the forking forms, so an action's shell code does not fork. What is
	// raised is the writing flag - arbitrary shell code may define, unset or
	// export anything - and the answer is the call's, so there is nothing left for
	// a sequence number to match.
	{
		const shell_writing_flag::scope writing{_writing};
		answer.status = _shell->port_call(code);
	}
	answer.answered = true;
	return answer;
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

void event_loop::run() {
	// ON THE CALLER'S THREAD, WHICH IS MAIN (#201). This was the body of a thread
	// this class spawned, and the first statement of that body blocked the caught
	// signal set. THAT BLOCK WAS DELETED, NOT MOVED - a signal mask survives
	// `execve`, main is the thread that forks and execs, and a child born with
	// SIGINT blocked ignores the `kill -INT` meant for it (#142, #143). Main stays
	// unmasked, the handler runs here as it did before leshper existed, and the
	// poll below takes the EINTR it always could. The function that did the
	// blocking went in #203, once nothing was left to call it.
	enter_read();
	// THE PROMPT IS PAINTED BEFORE THE FIRST POLL. `enter_read` asks for a
	// render, but a turn clears that flag before it polls and the first poll
	// blocks until a key arrives - so without this the prompt would appear only
	// once the user had typed something, which is the one moment they no longer
	// need it.
	render();
	while (!_exiting && !_stopping)
		turn();
	leave_read();
}

void event_loop::request_stop() noexcept {
	// A FLAG AND NOTHING ELSE (#201). It used to ring the signal topic's pipe as
	// well, because the caller was the shell's thread and the loop was asleep in a
	// `poll` that had to be woken. Every caller is this thread now - `end_of_file`
	// and the hangup from inside a turn, an `exit` from inside the `execute` this
	// loop called - and a poll that is not running needs no wakeup. `run`'s
	// `while` and `accept_current_line` read it at the next statement either way.
	_stopping = true;
}

} // namespace lesh::ui
