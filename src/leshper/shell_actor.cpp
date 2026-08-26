#include "leshper/shell_actor.h"

#include "substrate/assert.h"
#include "substrate/log.h"

#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <thread>
#include <unistd.h>
#include <utility>

namespace lesh::leshper {
namespace {

// The third copy of registry.cpp's thread key, and the comment workers.cpp
// carries applies verbatim: the two must agree or every accessor on the token
// would refuse, so `run_reactor_here` asserts `token_is_live` on a token it has
// just built rather than trusting that they still do.
std::uint64_t this_thread_key() noexcept {
	const std::uint64_t key =
		static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
	return key == 0 ? 1 : key;
}

// Both ends non-blocking and close-on-exec.
//
// Non-blocking on the WRITE end matters: the shell thread posts while holding
// nothing, and a full pipe means a wakeup is already outstanding, so a blocking
// write would be the shell thread waiting on a loop that is waiting on the
// shell thread. The byte is a doorbell and losing a duplicate ring costs
// nothing (fish `fds.cpp`: "In no case do we care about the data which is
// read").
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

void ring(int fd) noexcept {
	const char byte = 0;
	ssize_t n;
	do {
		n = ::write(fd, &byte, 1);
	} while (n < 0 && errno == EINTR);
	(void)n;
}

void unring(int fd) noexcept {
	char scratch[64];
	ssize_t n;
	do {
		n = ::read(fd, scratch, sizeof(scratch));
	} while (n > 0 || (n < 0 && errno == EINTR));
}

} // namespace

// ---------------------------------------------------------------------------
// run_reactor_here
// ---------------------------------------------------------------------------

void run_reactor_here(std::string_view reactor, lesh_reactor_fn fn, void* userdata,
                      request_snapshot snapshot, const std::atomic<bool>& superseded,
                      reactor_batch& into) {
	LESH_ASSERT(fn != nullptr);

	into.reactor.assign(reactor);
	into.computed_against = snapshot.computed_against;
	into.event_kind = snapshot.event_kind;
	into.spans.clear();
	into.texts.clear();
	into.proposals.clear();

	request_token token;
	// Moved, not copied: the snapshot was taken on the loop thread and its
	// buffer has no second reader.
	token.buffer = std::move(snapshot.buffer);
	token.cursor = snapshot.cursor;
	token.selection_start = snapshot.selection_start;
	token.selection_end = snapshot.selection_end;
	token.selection_active = snapshot.selection_active;
	token.computed_against = snapshot.computed_against;
	token.event_kind = snapshot.event_kind;
	// #151: THE FIELD THAT WAS NOT COPIED. Every other member of the snapshot
	// was transcribed here and this one was not, so a highlight on the shell
	// thread ran with a null adapter and `exit`, `bind`, every alias and every
	// function resolved `unknown` while `cd` passed only because macOS ships
	// `/usr/bin/cd`. The stamp itself is `shell_actor`'s (it knows whose shell
	// this is); what belongs here is that the copy is now COMPLETE.
	token.knowledge = snapshot.knowledge;
	token.superseded = &superseded;
	token.spans = &into.spans;
	token.texts = &into.texts;
	token.proposals = &into.proposals;
	token.owner_thread = this_thread_key();
	// Any non-zero value satisfies the ABI's only requirement of a live token.
	// A counter local to this thread is enough and reaches into nothing that is
	// loop-thread-only by ADR-0008.
	static thread_local std::uint64_t calls = 0;
	token.call_token = ++calls;

	LESH_ASSERT(token_is_live(&token));

	into.status = fn(&token, userdata);

	token.call_token = 0;
	token.owner_thread = 0;
}

// ---------------------------------------------------------------------------
// shell_channel
// ---------------------------------------------------------------------------

shell_channel::shell_channel() {
	const bool made = make_wakeup_pipe(_read_fd, _write_fd);
	LESH_ASSERT(made);
	(void)made;
}

shell_channel::~shell_channel() {
	if (_read_fd >= 0)
		::close(_read_fd);
	if (_write_fd >= 0)
		::close(_write_fd);
}

void shell_channel::post(shell_message&& answer) {
	bool arm = false;
	{
		std::lock_guard lock(_mutex);
		_queue.push_back(std::move(answer));
		// ARMED ONLY ON THE EMPTY-TO-NON-EMPTY TRANSITION, so N answers cost one
		// byte and one loop turn. Disarmed by drain, under this same lock.
		if (!_armed) {
			_armed = true;
			arm = true;
		}
	}
	if (arm)
		ring(_write_fd);
}

std::size_t shell_channel::drain(std::vector<shell_message>& out) {
	std::lock_guard lock(_mutex);
	const std::size_t arrived = _queue.size();
	for (shell_message& one : _queue)
		out.push_back(std::move(one));
	_queue.clear();
	if (_armed) {
		// Consume the byte INSIDE the lock, which is what makes "consume the fd,
		// then drain the queue" one operation rather than a pair a caller could
		// get half right.
		unring(_read_fd);
		_armed = false;
	}
	return arrived;
}

shell_message shell_channel::acquire() {
	std::lock_guard lock(_mutex);
	if (_spare.empty())
		return shell_message{};
	shell_message reused = std::move(_spare.back());
	_spare.pop_back();
	return reused;
}

void shell_channel::recycle(std::vector<shell_message>& used) {
	std::lock_guard lock(_mutex);
	for (shell_message& one : used) {
		// Cleared, never shrunk: the vectors behind a batch keep their capacity
		// across keystrokes, which is the whole reason messages are recycled at
		// all (#126's pooled batches, with the ownership made local).
		one.batch.spans.clear();
		one.batch.texts.clear();
		one.batch.proposals.clear();
		one.batch.reactor.clear();
		one.sequence = 0;
		one.status = LESH_OK;
		_spare.push_back(std::move(one));
	}
	used.clear();
}

bool shell_channel::empty() const {
	std::lock_guard lock(_mutex);
	return _queue.empty();
}

std::size_t shell_channel::size() const {
	std::lock_guard lock(_mutex);
	return _queue.size();
}

bool shell_channel::armed() const {
	std::lock_guard lock(_mutex);
	return _armed;
}

// ---------------------------------------------------------------------------
// shell_actor - the loop thread's side
// ---------------------------------------------------------------------------

void shell_actor::post_execute(std::string_view line, generation computed_against) {
	{
		std::lock_guard lock(_mutex);
		_execute.line.assign(line);
		_execute.computed_against = computed_against;
		_execute.filled = true;
		// An execution outranks a highlight, and a highlight computed against a
		// line that is about to run is worthless anyway: F-22's rule at accept.
		_superseded.store(true, std::memory_order_relaxed);
	}
	_work.notify_one();
}

std::uint64_t shell_actor::post_port_call(std::string_view code, generation computed_against) {
	std::uint64_t sequence = 0;
	{
		std::lock_guard lock(_mutex);
		sequence = ++_sequence;
		_port.code.assign(code);
		_port.computed_against = computed_against;
		_port.sequence = sequence;
		_port.filled = true;
		// A port call may write shell state, so whatever the highlighter is
		// reading is about to be stale. Superseding it is not an optimisation:
		// it is what keeps a batch computed against the old tables from being
		// the answer to the new ones.
		_superseded.store(true, std::memory_order_relaxed);
	}
	_work.notify_one();
	return sequence;
}

void shell_actor::post_highlight(std::string_view reactor, lesh_reactor_fn fn, void* userdata,
                                 request_snapshot snapshot) {
	{
		std::lock_guard lock(_mutex);
		if (_highlight.filled)
			++_dropped;  // latest-wins, counted
		_highlight.reactor.assign(reactor);
		_highlight.fn = fn;
		_highlight.userdata = userdata;
		_highlight.snapshot = std::move(snapshot);
		_highlight.filled = true;
		// THE CANCELLATION. A newer highlight arriving is the only reason an
		// older one is ever abandoned, so the overwrite and the supersede are
		// one act - there is no cancel call anywhere in this seam.
		_superseded.store(true, std::memory_order_relaxed);
	}
	_work.notify_one();
}

void shell_actor::stop() {
	{
		std::lock_guard lock(_mutex);
		_stopping = true;
		_superseded.store(true, std::memory_order_relaxed);
	}
	_work.notify_all();
}

// ---------------------------------------------------------------------------
// shell_actor - the shell thread's side
// ---------------------------------------------------------------------------

bool shell_actor::serve_one() {
	execute_slot execute;
	port_slot port;
	highlight_slot highlight;

	{
		std::lock_guard lock(_mutex);
		// PRIORITY ORDER, and it is the whole scheduling policy: `execute`, then
		// `port_call`, then `highlight`. A user who pressed Enter is not waiting
		// behind a repaint of the line they just left.
		if (_execute.filled) {
			execute = std::move(_execute);
			_execute = execute_slot{};
		} else if (_port.filled) {
			port = std::move(_port);
			_port = port_slot{};
		} else if (_highlight.filled) {
			highlight = std::move(_highlight);
			_highlight = highlight_slot{};
			// Cleared as the work is TAKEN, not when it is posted: a supersede
			// set by the post that handed us this item would otherwise cancel
			// the item it was announcing.
			_superseded.store(false, std::memory_order_relaxed);
		} else {
			return false;
		}
		_busy = true;
	}

	if (execute.filled)
		serve_execute(execute);
	else if (port.filled)
		serve_port_call(port);
	else
		serve_highlight(highlight);

	{
		std::lock_guard lock(_mutex);
		++_served;
		_busy = false;
	}
	return true;
}

void shell_actor::run() {
	for (;;) {
		{
			std::unique_lock lock(_mutex);
			_work.wait(lock, [this] {
				return _stopping || _execute.filled || _port.filled || _highlight.filled;
			});
			// Stopping wins over pending work: the loop has already gone, so a
			// highlight nobody will read is not worth computing, and an
			// `execute` that arrived after `stop` never had a caller.
			if (_stopping)
				return;
		}
		while (serve_one()) {
			std::lock_guard lock(_mutex);
			if (_stopping)
				return;
		}
	}
}

void shell_actor::serve_execute(execute_slot& job) {
	LESH_LOG(log::level::debug, log::category::exec, "shell: execute %zu bytes",
	         job.line.size());
	shell_message answer = _replies.acquire();
	answer.which = shell_message::kind::execute_done;
	answer.computed_against = job.computed_against;
	{
		// ADR-0009's one writer, announced for the length of the write (#151).
		// `execute` is where a `PATH=` assignment, an `alias`, a function
		// definition or an `unset` actually happens; any read through the
		// session's adapter while it runs would be reading a table mid-rewrite,
		// and the assertion there says so instead of the reader finding out
		// later.
		const shell_writing_flag::scope writing{_writing};
		answer.status = _shell->execute(job.line);
	}
	answer.sequence = 0;
	_replies.post(std::move(answer));
}

void shell_actor::serve_port_call(port_slot& job) {
	LESH_LOG(log::level::debug, log::category::exec, "shell: port_call seq=%llu %zu bytes",
	         static_cast<unsigned long long>(job.sequence), job.code.size());
	shell_message answer = _replies.acquire();
	answer.which = shell_message::kind::port_call_done;
	answer.computed_against = job.computed_against;
	answer.sequence = job.sequence;
	{
		// The other writer: an action's shell code is arbitrary and may define,
		// unset or export anything (#92).
		const shell_writing_flag::scope writing{_writing};
		answer.status = _shell->port_call(job.code);
	}
	_replies.post(std::move(answer));
}

void shell_actor::serve_highlight(highlight_slot& job) {
	shell_message answer = _replies.acquire();
	answer.which = shell_message::kind::highlight_done;
	answer.computed_against = job.snapshot.computed_against;
	answer.sequence = 0;
	// THE STAMP (#151). Not the loop's to put on the snapshot and not this
	// function's to remember per call site: the actor serves one shell, and this
	// is that shell's knowledge, on every token it mints.
	job.snapshot.knowledge = _knowledge;
	run_reactor_here(job.reactor, job.fn, job.userdata, std::move(job.snapshot), _superseded,
	                 answer.batch);
	answer.status = answer.batch.status;
	LESH_LOG(log::level::debug, log::category::reactor,
	         "shell: highlight gen=%llu status=%d spans=%zu",
	         static_cast<unsigned long long>(answer.computed_against.value()),
	         static_cast<int>(answer.status), answer.batch.spans.size());
	_replies.post(std::move(answer));
}

std::size_t shell_actor::served() const noexcept {
	std::lock_guard lock(_mutex);
	return _served;
}

std::size_t shell_actor::dropped() const noexcept {
	std::lock_guard lock(_mutex);
	return _dropped;
}

bool shell_actor::idle() const noexcept {
	std::lock_guard lock(_mutex);
	return !_busy && !_execute.filled && !_port.filled && !_highlight.filled;
}

} // namespace lesh::leshper
