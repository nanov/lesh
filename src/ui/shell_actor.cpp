#include "ui/shell_actor.h"

#include "substrate/assert.h"
#include "substrate/log.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace lesh::ui {
namespace {

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

bool shell_channel::armed() const {
	std::lock_guard lock(_mutex);
	return _armed;
}

// ---------------------------------------------------------------------------
// shell_actor - the loop thread's side
// ---------------------------------------------------------------------------

void shell_actor::post_execute(std::string_view line, leshper::generation computed_against) {
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

std::uint64_t shell_actor::post_port_call(std::string_view code, leshper::generation computed_against) {
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

void shell_actor::post_highlight(std::string_view reactor, lesh_reactor_fn fn, void* userdata,
                                 const leshper::state& target, std::uint32_t event_kind) {
	{
		std::lock_guard lock(_mutex);
		if (_highlight.filled)
			++_dropped;  // latest-wins, counted
		_highlight.reactor.assign(reactor);
		_highlight.fn = fn;
		_highlight.userdata = userdata;
		// STRAIGHT INTO THE SLOT'S OWN STORAGE. Not built and then moved: written
		// where it already lives, so the buffer the previous round left behind is
		// reused rather than freed. `host` is stamped in `serve_highlight`, which
		// is where it has always been decided (#151).
		take_snapshot(_highlight.snapshot, target, event_kind);
		_highlight.filled = true;
		// The same cancellation the overload above documents: a newer highlight
		// arriving IS the abandonment of the older one.
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
	// SWAPPED OUT, NOT MOVED OUT. The three `_serving_*` members carry the
	// previous round's storage into the slot they take from, so the strings the
	// loop thread grew are still there for the next `post_*` to assign into -
	// which is what the banner at the top of this file promises. `= slot{}` here
	// freed them, once per keystroke on the `highlight` slot.
	//
	// WHICH ONE WAS TAKEN IS A VARIABLE NOW, because a retained member's `filled`
	// is left over from the last round and can no longer answer the question.
	enum class took : std::uint8_t { nothing, execute, port, highlight };
	took what = took::nothing;

	{
		std::lock_guard lock(_mutex);
		// PRIORITY ORDER, and it is the whole scheduling policy: `execute`, then
		// `port_call`, then `highlight`. A user who pressed Enter is not waiting
		// behind a repaint of the line they just left.
		if (_execute.filled) {
			std::swap(_serving_execute, _execute);
			_execute.filled = false;
			what = took::execute;
		} else if (_port.filled) {
			std::swap(_serving_port, _port);
			_port.filled = false;
			what = took::port;
		} else if (_highlight.filled) {
			std::swap(_serving_highlight, _highlight);
			_highlight.filled = false;
			// The function pointer and its context go with `filled`: a slot that
			// is not filled must not look like it names a reactor.
			_highlight.fn = nullptr;
			_highlight.userdata = nullptr;
			what = took::highlight;
			// Cleared as the work is TAKEN, not when it is posted: a supersede
			// set by the post that handed us this item would otherwise cancel
			// the item it was announcing.
			_superseded.store(false, std::memory_order_relaxed);
		} else {
			return false;
		}
		_busy = true;
	}

	// Outside the lock, and safe there because `_serving_*` are the SHELL
	// THREAD's alone - nothing else in this file names them.
	switch (what) {
		case took::execute:
			serve_execute(_serving_execute);
			break;
		case took::port:
			serve_port_call(_serving_port);
			break;
		case took::highlight:
			serve_highlight(_serving_highlight);
			break;
		case took::nothing:
			break;
	}

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
	// is that shell's host, on every token it mints.
	job.snapshot.host = _host;
	run_reactor_here(job.reactor, job.fn, job.userdata, job.snapshot, _superseded,
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

} // namespace lesh::ui
