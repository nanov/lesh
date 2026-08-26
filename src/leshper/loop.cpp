#include "leshper/loop.h"

#include "leshper/keymap.h"
#include "substrate/assert.h"
#include "substrate/fork_guard.h"
#include "substrate/log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace lesh::leshper {
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
// A plain pointer and not an atomic: it is written on the loop thread before
// `sigaction` publishes the handler and cleared after `sigaction` unpublishes
// it, so a handler that runs at all runs after the write and before the clear.
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

// POLLIN AND POLLHUP TOGETHER, always. fish `fds.cpp`: "If a pipe is widowed
// with no data, Linux sets POLLHUP but not POLLIN, so test for both." POLLERR
// joins them because a topic whose fd has errored still has to be answered -
// with a drain that finds nothing and a queue that stops being armed.
constexpr short kReadable = POLLIN | POLLHUP | POLLERR;

constexpr short revents_of(const struct pollfd& one) noexcept {
	return static_cast<short>(one.revents & kReadable);
}

} // namespace

const char* name_of(topic which) noexcept {
	switch (which) {
		case topic::tty: return "tty";
		case topic::signal: return "signal";
		case topic::worker: return "worker";
		case topic::timer: return "timer";
		case topic::shell: return "shell";
		case topic::count_: break;
	}
	return "?";
}

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
	if (signo <= 0 || signo >= kMaxTrackedSignal || !_saved_valid[signo])
		return;
	const struct sigaction& previous = _saved[signo];
	if ((previous.sa_flags & SA_SIGINFO) != 0)
		return;  // no siginfo_t to give it; see the header
	if (previous.sa_handler == SIG_DFL || previous.sa_handler == SIG_IGN)
		return;  // a disposition, not a handler: there is nothing to call
	previous.sa_handler(signo);
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
	// ONCE PER SIGNAL. See `reassert` in the header: the chain target must stay
	// the handler that was there before us, and re-saving would capture either
	// our own handler or the shell's newest one.
	const auto save = [this](int signo) {
		if (signo <= 0 || signo >= kMaxTrackedSignal || _saved_valid[signo])
			return;
		_saved_valid[signo] = ::sigaction(signo, nullptr, &_saved[signo]) == 0;
	};
	const auto catch_it = [&](int signo, int flags) {
		save(signo);
		struct sigaction action{};
		action.sa_handler = &fish_style_handler;
		sigemptyset(&action.sa_mask);
		action.sa_flags = flags;
		if (::sigaction(signo, &action, nullptr) != 0)
			complete = false;
	};
	const auto ignore_it = [&](int signo) {
		save(signo);
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
	// syscalls" (fish `signal.cpp`). The reap is the shell thread's, and there
	// is nothing here that a child exiting should tear.
	catch_it(SIGCHLD, SA_RESTART);
	catch_it(SIGWINCH, SA_RESTART);

	// SIGHUP ONLY IF IT IS CURRENTLY SIG_DFL. `nohup` sets it to SIG_IGN before
	// exec, and a shell that installed a handler over that would be undoing what
	// the user asked for.
	struct sigaction hup_now{};
	if (::sigaction(SIGHUP, nullptr, &hup_now) == 0
	    && (hup_now.sa_handler == SIG_DFL || hup_now.sa_handler == &fish_style_handler))
		catch_it(SIGHUP, SA_RESTART);

	// "We are a shell, we know what is best for the user" (fish). SIGTTOU and
	// SIGTTIN also go through tty.h's `ignore_background_write_signals`, because
	// they must be ignored before a mode change even in a process that never
	// installed a hub.
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
		if (!_saved_valid[signo])
			continue;
		::sigaction(signo, &_saved[signo], nullptr);
		_saved_valid[signo] = false;
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
	  _blitter(_pool, _options.capabilities) {
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
	_deferred.reserve(8);
	_signal_numbers.reserve(8);
	_completions.reserve(8);
	_inbox.reserve(8);
	_out.reserve(4096);
	_accepted.reserve(256);

	install_fork_child_detection();
}

event_loop::~event_loop() {
	stop();
	// The messages `drain` handed us go back before the actor that owns their
	// storage does (ADR-0007).
	if (_shell != nullptr)
		_shell->replies().recycle(_inbox);
}

void event_loop::attach_helpers(worker_pool& pool) noexcept { _helpers = &pool; }

void event_loop::attach_registry(registry& reg) noexcept {
	_registry = &reg;
	_dispatch.emplace(reg);
}

void event_loop::attach_shell(shell_actor& shell) noexcept { _shell = &shell; }

void event_loop::attach_shell_knowledge(const shell_knowledge* knowledge) noexcept {
	_knowledge = knowledge;
}

void event_loop::attach_signals(signal_hub& hub) noexcept {
	_signals = &hub;
	_resizes_seen = hub.resize_count();
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
	if (_signals != nullptr) {
		_resizes_seen = _signals->resize_count();
		// TAKEN AGAIN, because the shell may have given them away (#134). A
		// `trap ... INT` run at the prompt installs the shell's own handler over
		// ours, after which Ctrl-C sets `g_pending` and rings no pipe. Re-asserting
		// is nine `sigaction` calls once per read entry, and `reassert` keeps what
		// it saved the first time so the chain still reaches the shell's handler.
		_signals->reassert();
	}
	refresh_size_from_terminal();
	_needs_render = true;
}

void event_loop::leave_read() {
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

turn_result event_loop::turn() { return turn(poll_timeout_ms()); }

turn_result event_loop::turn(int timeout_ms) {
	turn_result result;
	_events.clear();
	_needs_render = false;

	// Anything a blocked wait deferred is delivered first, in the order it
	// arrived: a resize that landed while a command ran has been waiting for the
	// editor to exist again.
	if (!_deferred.empty()) {
		for (event& one : _deferred)
			_events.push_back(std::move(one));
		_deferred.clear();
	}

	int at = 0;
	int tty_at = -1, signal_at = -1, worker_at = -1, shell_at = -1;
	const auto watch = [&](int fd) {
		_poll[static_cast<std::size_t>(at)].fd = fd;
		_poll[static_cast<std::size_t>(at)].events = POLLIN;
		_poll[static_cast<std::size_t>(at)].revents = 0;
		return at++;
	};
	if (_fds.input >= 0)
		tty_at = watch(_fds.input);
	if (_signals != nullptr)
		signal_at = watch(_signals->wakeup_fd());
	if (_helpers != nullptr)
		worker_at = watch(_helpers->completions().wakeup_fd());
	if (_shell != nullptr)
		shell_at = watch(_shell->replies().wakeup_fd());

	const int ready = ::poll(_poll.data(), static_cast<nfds_t>(at), timeout_ms);
	if (ready < 0) {
		if (errno != EINTR) {
			// #128's trap 1: a non-EINTR poll error is the terminal having gone,
			// not something to retry. fish treats exactly this as "the tty has
			// been closed".
			LESH_LOG(log::level::error, log::category::loop, "poll failed: %s",
			         std::strerror(errno));
			_exiting = true;
			result.exiting = true;
			return result;
		}
		// EINTR means a signal landed. The self-pipe has the byte; fall through
		// and drain the topics with everything marked not-ready, which the
		// signal drain does not need.
		for (int i = 0; i < at; ++i)
			_poll[static_cast<std::size_t>(i)].revents = 0;
	}
	result.timed_out = ready == 0;

	const input_instant now = clock::now();

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
	// resolve a sequence that is still legitimately in flight (#111).
	_decoder.expire(now, _events);

	if (signal_at >= 0
	    && (ready < 0 || revents_of(_poll[static_cast<std::size_t>(signal_at)]) != 0))
		drain_signal_topic(result);
	fire_timers(now, result);
	if (worker_at >= 0 && revents_of(_poll[static_cast<std::size_t>(worker_at)]) != 0)
		drain_worker_topic(result);
	if (shell_at >= 0 && revents_of(_poll[static_cast<std::size_t>(shell_at)]) != 0)
		drain_shell_topic(result);

	for (const event& one : _events) {
		handle(one, result);
		if (_exiting)
			break;
	}
	result.events = _events.size();

	if (_needs_render && !_exiting) {
		render();
		result.rendered = true;
	}
	result.exiting = _exiting;
	return result;
}

void event_loop::drain_tty(input_instant now, turn_result& result) {
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
			struct pollfd again{};
			again.fd = _fds.input;
			again.events = POLLIN;
			if (::poll(&again, 1, 0) == 1 && (again.revents & kReadable) != 0)
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
		_events.push_back(signal_event{SIGHUP});
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

	if (resizes != _resizes_seen) {
		// #128's trap 12: the counter was read BEFORE this ioctl.
		_resizes_seen = resizes;
		const terminal_size size = _terminal.size();
		_events.push_back(resize_event{size.columns, size.rows});
	}

	for (int signo : _signal_numbers) {
		LESH_LOG(log::level::debug, log::category::loop, "topic=signal signo=%d", signo);
		_events.push_back(signal_event{signo});
	}
}

void event_loop::fire_timers(input_instant now, turn_result& result) {
	if (_registry == nullptr || _registry->timers.empty()) {
		_timers.clear();
		return;
	}

	// Arm anything the registry has that we have not seen, and forget anything
	// it no longer has. The registry knows no clock, which is why the due
	// instants live here and not beside the intervals.
	for (const auto& armed : _registry->timers) {
		const auto known = std::find_if(_timers.begin(), _timers.end(),
		                                [&](const timer_due& d) { return d.id == armed.id; });
		if (known == _timers.end())
			_timers.push_back(timer_due{armed.id, now + std::chrono::milliseconds(armed.interval_ms)});
	}
	_timers.erase(std::remove_if(_timers.begin(), _timers.end(),
	                             [&](const timer_due& d) {
		                             return std::none_of(_registry->timers.begin(),
		                                                 _registry->timers.end(),
		                                                 [&](const auto& t) { return t.id == d.id; });
	                             }),
	              _timers.end());

	for (timer_due& armed : _timers) {
		if (armed.due > now)
			continue;
		const auto entry = std::find_if(_registry->timers.begin(), _registry->timers.end(),
		                                [&](const auto& t) { return t.id == armed.id; });
		if (entry == _registry->timers.end())
			continue;
		// Rearmed from NOW rather than from the missed due instant: a loop that
		// was blocked in an execution for a minute must not then fire a
		// one-second timer sixty times.
		armed.due = now + std::chrono::milliseconds(entry->interval_ms);
		++result.topics_drained;
		++_timer_dispatches;
		LESH_LOG(log::level::debug, log::category::loop, "topic=timer id=%llu action=%s",
		         static_cast<unsigned long long>(entry->id), entry->action.c_str());
		if (_dispatch.has_value()) {
			const action_result what = _dispatch->invoke(_state, entry->action, invocation{});
			apply_outcome(what, result);
			if (_exiting)
				return;
		}
	}
}

void event_loop::take_batch(reactor_batch& answer) {
	// THE DROP RULE, and this is the only place it is decided (N-4, ADR-0008).
	// A batch computed against a generation the editor has moved past is not
	// rejected so much as it has nowhere to go: nothing else applies batches.
	if (!(answer.computed_against == _state.gen)) {
		++_dropped;
		LESH_LOG(log::level::debug, log::category::reactor,
		         "dropped %s: gen=%llu editor=%llu", answer.reactor.c_str(),
		         static_cast<unsigned long long>(answer.computed_against.value()),
		         static_cast<unsigned long long>(_state.gen.value()));
		return;
	}

	// AND THIS IS WHERE IT LANDS (#141). The batch's spans and virtual text go
	// into the editor's own `marks`, which is what `lay_out` reads - so the trail
	// from a reactor's emit to a coloured cell ends here rather than in a vector
	// beside the loop. #129 wrote that this is where the application would go
	// when `state::decorations` gained a type, and this is that.
	//
	// The emitting reactor IS the decoration namespace (ADR-0008): a new batch
	// replaces that reactor's previous one and touches nobody else's. `apply`
	// SWAPS rather than copying, because the batch we are handed is pooled
	// storage (#126's message pool, or the shell channel's recycler) and moving
	// out of it would hand the pool back an empty vector with no capacity,
	// defeating the pooling on the very path it was built for.
	//
	// THE PROPOSALS DO NOT COME WITH IT, and that is not an oversight. A
	// proposal is what an action would ACCEPT (#133), not something anchored to
	// a buffer position, and `lesh_proposal_read` reads it from the applied
	// batches the dispatching harness holds. This is the decoration store.
	_state.marks.apply(answer.reactor, answer.spans, answer.texts);
	++_applied;
	_needs_render = true;
}

void event_loop::drain_worker_topic(turn_result& result) {
	if (_helpers == nullptr)
		return;
	++result.topics_drained;

	// #126's rule, written in its header: ANSWER THE READABLE FD WITH `drain()`.
	// It consumes the byte and empties the queue under one lock. Reading the fd
	// here instead would leave the queue armed, so no further byte would ever be
	// written and the next wakeup would be lost permanently.
	_completions.clear();
	_helpers->completions().drain(_completions);

	for (completion& done : _completions) {
		if (done.empty())
			continue;
		reactor_batch& answer = done.batch();
		LESH_LOG(log::level::debug, log::category::event,
		         "topic=worker reactor=%s gen=%llu status=%d", answer.reactor.c_str(),
		         static_cast<unsigned long long>(answer.computed_against.value()),
		         static_cast<int>(answer.status));
		const generation at = answer.computed_against;
		take_batch(answer);
		// The editor sees the arrival too: `step` carries the same drop rule and
		// emits the redraw, and the replay file records it (#109's `event`).
		_events.push_back(worker_result{at});
	}
	// Every message goes home the moment its batch has been taken.
	_completions.clear();
}

void event_loop::drain_shell_topic(turn_result& result) {
	if (_shell == nullptr)
		return;
	++result.topics_drained;

	// Same contract as the worker topic: `drain` consumes the byte and empties
	// the queue under one lock (ADR-0009 point 3, #126's rule).
	_shell->replies().drain(_inbox);
	for (shell_message& answer : _inbox)
		handle_shell_message(answer);
	_shell->replies().recycle(_inbox);
}

void event_loop::handle_shell_message(shell_message& answer) {
	switch (answer.which) {
		case shell_message::kind::highlight_done:
			LESH_LOG(log::level::debug, log::category::event,
			         "topic=shell highlight gen=%llu status=%d",
			         static_cast<unsigned long long>(answer.computed_against.value()),
			         static_cast<int>(answer.status));
			take_batch(answer.batch);
			_events.push_back(worker_result{answer.computed_against});
			break;
		case shell_message::kind::port_call_done:
			// Nobody is waiting: the action that asked has already given up, or
			// this is a reply that outlived its `call_port`. Dropped, loudly
			// enough to find in a log.
			LESH_LOG(log::level::warn, log::category::event,
			         "topic=shell unmatched port_call seq=%llu",
			         static_cast<unsigned long long>(answer.sequence));
			break;
		case shell_message::kind::execute_done:
			LESH_LOG(log::level::info, log::category::event,
			         "topic=shell execute_done status=%d", static_cast<int>(answer.status));
			_exit_status = answer.status;
			break;
	}
}

// ---------------------------------------------------------------------------
// One event
// ---------------------------------------------------------------------------

void event_loop::handle(const event& incoming, turn_result& result) {
	// EVERYTHING BEFORE AND AFTER, so `selection_changed` can be fired by
	// COMPARISON. #116 deliberately left the state emitting no events - it is a
	// data structure - so noticing that a region moved is the loop's job, and
	// doing it by comparison rather than by having every action remember to
	// announce itself is what makes it impossible to forget.
	const generation before_gen = _state.gen;
	const position before_cursor = _state.cursor;
	const position before_anchor = _state.selection_anchor();
	const bool before_active = _state.selection_active();

	// KEYBOARD INTERRUPT, BEFORE THE EDITOR SEES IT (#98 decision 2). Ctrl-C in
	// raw mode never arrives as a byte - ISIG stays on precisely so the kernel
	// turns it into SIGINT - so it can never reach a keymap, and `step` says as
	// much: the entrance for a signal exists and the binding does not. The
	// binding is HERE, by action name, so it is still the rebindable
	// `cancel_line` of F-13 and not a hardcoded behaviour.
	if (const auto* signal = std::get_if<signal_event>(&incoming);
	    signal != nullptr && signal->signal_number == SIGINT && !_options.interrupt_action.empty()) {
		editing_context& context = context_of(_state);
		const action_result what = context.loop().invoke(_state, _options.interrupt_action,
		                                                 invocation{});
		if (what.status == LESH_ERR_NOTFOUND)
			LESH_LOG(log::level::warn, log::category::dispatch,
			         "no action registered for the interrupt: %s",
			         _options.interrupt_action.c_str());
	}

	// `step` logs the event itself, at #109's `event` category and into the
	// replay file - the one event serialization, and the reason this loop does
	// not write a second one for the events that reach the editor.
	const effects produced = step(_state, incoming);

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

	carry_out(produced, result);

	// THE OUTCOME AN ACTION ASKED FOR, taken here (#134). Dispatch on the
	// keystroke path happens inside `step`, which is a pure function over state
	// and events and has nowhere to put a request to accept, cancel or exit - so
	// the harness latches it and the loop, which is the only thing that can carry
	// one out, takes it the moment the event is done. The timer path reads its
	// outcome off `invoke`'s return value and never reaches this.
	const loop_harness::requested_outcome asked = context_of(_state).loop().take_outcome();
	if (asked.what != loop_outcome::none) {
		action_result carried;
		carried.outcome = asked.what;
		carried.exit_status = asked.exit_status;
		apply_outcome(carried, result);
	}
}

void event_loop::carry_out(const effects& produced, turn_result& result) {
	for (const effect& one : produced) {
		if (std::holds_alternative<render_request>(one)) {
			_needs_render = true;
		} else if (std::holds_alternative<worker_request>(one)) {
			// The fan-out already happened in `handle`, from the comparison -
			// which is strictly more information than this effect carries, since
			// it knows WHICH of the three kinds changed. Nothing to do.
		} else if (const auto* spawn = std::get_if<spawn_request>(&one)) {
			// #92's lane 2. The implementation is the shell side's (#134 wires
			// it); what the loop owes is not to silently drop it.
			LESH_LOG(log::level::warn, log::category::spawn,
			         "spawn_request with no spawner attached: %s",
			         spawn->argv.empty() ? "" : spawn->argv.front().c_str());
		}
	}
	(void)result;
}

void event_loop::notify_reactors(std::uint32_t kinds) {
	if (_registry == nullptr)
		return;
	for (const auto& [name, entry] : _registry->reactors) {
		const std::uint32_t served = entry.event_mask & kinds;
		if (served == 0)
			continue;

		// ADR-0009: the highlighter runs on the SHELL thread, because it reads
		// the alias, function and builtin tables and shell state has exactly one
		// owner. Everything else is state-free - history search, the
		// autosuggester, path checks - and stays on the stateless helper pool.
		if (_shell != nullptr && name == _options.shell_thread_reactor) {
			request_snapshot asking = snapshot_of(_state, served);
			// #135's door, opened here and nowhere else. Forgetting it does not
			// break the highlighter - it degrades to `environment_knowledge`,
			// which is the `getenv` behaviour #124 recorded as wrong about a
			// `PATH=` the user has just typed - so the assignment is silent and
			// its absence would be too. See `attach_shell_knowledge`.
			asking.knowledge = _knowledge;
			_shell->post_highlight(name, entry.fn, entry.userdata, std::move(asking));
			continue;
		}
		if (_helpers != nullptr)
			_helpers->submit(name, snapshot_of(_state, served), entry.fn, entry.userdata);
	}
}

void event_loop::apply_outcome(const action_result& what, turn_result& result) {
	switch (what.outcome) {
		case loop_outcome::none:
			break;
		case loop_outcome::accept_line:
			accept_current_line();
			break;
		case loop_outcome::cancel_line:
			// #98 decision 2: discard the buffer, paint the indicator, fresh
			// prompt. `$?` = 130 and the user's INT trap are the SHELL's, and
			// `finish_cancelled_line` is how they are asked for - the loop still
			// does not own exit statuses, it owns the handoff.
			if (_options.manage_terminal || _fds.output >= 0)
				write_all(_fds.output, "^C\r\n");
			apply_edit(_state, position{}, _state.buffer.end_position(), "");
			_state.undo.break_coalescing();
			_state.marks.clear();   // the same rule accept follows
			_have_previous = false;
			_needs_render = true;
			finish_cancelled_line();
			break;
		case loop_outcome::exit:
			_exiting = true;
			_exit_status = what.exit_status;
			break;
		case loop_outcome::recursive_edit:
			// F-18's recovery shape. Nothing in v1 nests reads, and inventing
			// half of it here would be building the thing its ticket must.
			LESH_LOG(log::level::warn, log::category::dispatch,
			         "recursive_edit requested; not implemented in v1");
			break;
	}
	if (what.buffer_changed || what.cursor_moved)
		_needs_render = true;
	(void)result;
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

	layout_input in = input_for(_state, _options.prompt, _options.continuation);
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

	layout desired = lay_out(_pool, in);

	_out.clear();
	if (_have_previous && can_diff(_previous, desired))
		_blitter.update_into(_previous.screen, desired.screen, _out);
	else
		_blitter.paint_into(desired.screen, _out);

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
	LESH_LOG_TRACE(log::category::render, "rendered %zu bytes", _out.size());
}

// ---------------------------------------------------------------------------
// Quiesce
// ---------------------------------------------------------------------------

void event_loop::quiesce() {
	// The helpers' half is #126's and nests; the terminal's half is ours.
	if (_helpers != nullptr)
		_helpers->park_all();
	if (_park_depth == 0 && _options.manage_terminal)
		_terminal.leave_raw();
	if (_park_depth == 0)
		_decoder.reset();
	++_park_depth;
}

void event_loop::resume_after_execution() {
	LESH_ASSERT(_park_depth > 0);
	--_park_depth;
	if (_park_depth == 0 && _options.manage_terminal) {
		// The order is the read-entry order, because that is what this is: the
		// terminal comes back, then the modes, then the size.
		ignore_background_write_signals();
		_terminal.reclaim();
		_terminal.enter_raw();
	}
	if (_park_depth == 0) {
		if (_signals != nullptr) {
			_resizes_seen = _signals->resize_count();
			// The command that just ran may have been a `trap`; see enter_read.
			_signals->reassert();
		}
		refresh_size_from_terminal();
		// The screen is whatever the command left behind, so there is nothing to
		// diff against: the next render is a full repaint.
		_have_previous = false;
		_needs_render = true;
	}
	if (_helpers != nullptr)
		_helpers->resume();
}

void event_loop::assert_quiesced() const noexcept {
	LESH_ASSERT(_park_depth > 0);
	if (_helpers != nullptr)
		_helpers->assert_quiesced();
	// The other half, and the one only the loop can check: a fork taken with the
	// terminal still in raw mode gives the child a terminal it cannot use.
	LESH_ASSERT(!_options.manage_terminal || !_terminal.raw());
}

// ---------------------------------------------------------------------------
// Accept, and the port
// ---------------------------------------------------------------------------

std::optional<std::int32_t> event_loop::accept_current_line() {
	_accepted.assign(_state.buffer.text());
	const generation at = _state.gen;

	// The cursor is left wherever the layout put it; the command's output has to
	// start on a fresh row below the edit line (F-39 scrolls output above the
	// prompt, which only works if the prompt is the last thing on screen).
	if (_fds.output >= 0)
		write_all(_fds.output, "\r\n");
	_have_previous = false;

	// PARK, THEN RESTORE, THEN POST. In that order: the fork is the shell
	// thread's and it happens inside `execute`, so the helpers must be parked
	// before the message is even visible to it.
	quiesce();
	assert_quiesced();

	std::optional<std::int32_t> status;
	if (_shell != nullptr) {
		_shell->post_execute(_accepted, at);
		status = wait_on_shell(shell_message::kind::execute_done, 0);
	}

	resume_after_execution();

	// The decorations go with the line they were computed for. Not because they
	// are stale - the drop rule would sort that out - but because they are
	// anchored to BYTE OFFSETS in a buffer that is about to be emptied, and a
	// suggestion left standing over an empty prompt is a suggestion for a line
	// that has already run.
	_state.marks.clear();

	// A fresh line: the accepted text is gone, and it is gone as ONE edit so
	// that undo does not walk back into a command that has already run.
	if (!_state.buffer.text().empty()) {
		apply_edit(_state, position{}, _state.buffer.end_position(), "");
		_state.undo.break_coalescing();
	}
	_needs_render = true;
	render();
	return status;
}

void event_loop::finish_cancelled_line() {
	// #98 decision 3, the zsh way: Ctrl-C at the prompt cancels the line AND
	// fires the user's INT trap, with `$?` = 130. Both are the shell's, and the
	// only door to the shell thread is the `execute` slot - so a cancel is posted
	// as an EMPTY line, which is exactly what it is: nothing to run, at a command
	// boundary, which is where #33 says a trap body belongs.
	//
	// THE QUIESCE IS NOT CEREMONY. A trap body is arbitrary shell code and may
	// fork, so the helpers have to be parked and the terminal handed back before
	// the shell thread touches it, on the identical path an accepted line takes.
	if (_shell == nullptr)
		return;
	quiesce();
	assert_quiesced();
	_shell->post_execute(std::string_view{}, _state.gen);
	wait_on_shell(shell_message::kind::execute_done, 0);
	resume_after_execution();
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
	const std::uint64_t sequence = _shell->post_port_call(code, _state.gen);
	const std::optional<std::int32_t> status =
		wait_on_shell(shell_message::kind::port_call_done, sequence);
	if (status.has_value()) {
		answer.status = *status;
		answer.answered = true;
	}
	return answer;
}

std::optional<std::int32_t> event_loop::wait_on_shell(shell_message::kind until,
                                                      std::uint64_t sequence) {
	LESH_ASSERT(_shell != nullptr);

	for (;;) {
		// TWO TOPICS ONLY: `shell` and `signal`. The tty is not ours while a
		// command runs, the helpers are parked, and a timer that fired here would
		// dispatch an action into an editor with no terminal. ADR-0009: "during
		// execution the loop blocks in that same poll, so SIGWINCH and the
		// terminal-restore path still flow through it."
		int at = 0;
		int signal_at = -1;
		int shell_at = -1;
		const auto watch = [&](int fd) {
			_poll[static_cast<std::size_t>(at)].fd = fd;
			_poll[static_cast<std::size_t>(at)].events = POLLIN;
			_poll[static_cast<std::size_t>(at)].revents = 0;
			return at++;
		};
		if (_signals != nullptr)
			signal_at = watch(_signals->wakeup_fd());
		shell_at = watch(_shell->replies().wakeup_fd());

		const int ready = ::poll(_poll.data(), static_cast<nfds_t>(at), -1);
		if (ready < 0 && errno != EINTR) {
			LESH_LOG(log::level::error, log::category::loop, "poll failed while waiting: %s",
			         std::strerror(errno));
			return std::nullopt;
		}

		if (_signals != nullptr
		    && (ready < 0 || revents_of(_poll[static_cast<std::size_t>(signal_at)]) != 0)) {
			_signal_numbers.clear();
			_signals->drain(_signal_numbers);
			// The RESIZE IS NOT QUERIED HERE (#98 decision 6: "during a command
			// it is not tracked at all"). The counter is realigned at the next
			// read entry, which re-queries unconditionally - which is what makes
			// a missed resize structurally impossible rather than handled.
			for (int signo : _signal_numbers)
				_deferred.push_back(signal_event{signo});
		}

		if (ready > 0 && revents_of(_poll[static_cast<std::size_t>(shell_at)]) != 0) {
			_shell->replies().drain(_inbox);
			std::optional<std::int32_t> found;
			for (shell_message& answer : _inbox) {
				if (answer.which == until && (until != shell_message::kind::port_call_done
				                              || answer.sequence == sequence)) {
					found = answer.status;
					if (until == shell_message::kind::execute_done)
						_exit_status = answer.status;
					continue;
				}
				// A highlight computed against the line that is now running, or a
				// port call nobody is waiting for. The generation rule drops the
				// first; the second is logged.
				handle_shell_message(answer);
			}
			_shell->replies().recycle(_inbox);
			if (found.has_value())
				return found;
		}

		if (_stopping.load(std::memory_order_relaxed))
			return std::nullopt;
	}
}

// ---------------------------------------------------------------------------
// Running
// ---------------------------------------------------------------------------

void event_loop::run() {
	enter_read();
	// THE PROMPT IS PAINTED BEFORE THE FIRST POLL. `enter_read` asks for a
	// render, but a turn clears that flag before it polls and the first poll
	// blocks until a key arrives - so without this the prompt would appear only
	// once the user had typed something, which is the one moment they no longer
	// need it.
	render();
	while (!_exiting && !_stopping.load(std::memory_order_relaxed))
		turn();
	leave_read();
}

void event_loop::start() {
	LESH_ASSERT(!_thread.joinable());
	_stopping.store(false, std::memory_order_relaxed);
	_thread = std::thread([this] {
		run();
		// THE LOOP THREAD RELEASES THE SHELL THREAD, and nothing else can
		// (ADR-0009, #134). The shell is the main thread and is parked in
		// `shell_actor::run` on a condition variable with no descriptor to
		// watch; when the editor is finished - Ctrl-D, an `exit` action, a
		// hangup - the only side that knows is this one. `stop()` is documented
		// as the loop thread's call, and this is the loop thread making it.
		if (_shell != nullptr)
			_shell->stop();
	});
}

void event_loop::request_stop() noexcept {
	_stopping.store(true, std::memory_order_relaxed);
	// The one wakeup that always exists: ring the signal topic's own pipe. No
	// signal is raised, so nothing else in the process notices.
	if (_signals != nullptr)
		_signals->poke();
}

void event_loop::stop() {
	request_stop();
	if (_thread.joinable())
		_thread.join();
}

bool event_loop::running() const noexcept { return _thread.joinable(); }

} // namespace lesh::leshper
