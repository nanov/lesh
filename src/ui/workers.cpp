#include "ui/workers.h"

// FOR THE SIGNAL MASK ONLY (#142). `block_caught_signals_on_this_thread` is
// declared with the hub whose caught set it names, and a helper thread has to
// call it before it does anything else. `loop.h` includes this header and not
// the other way round, so there is no cycle.
#include "ui/loop.h"
#include "substrate/assert.h"
#include "substrate/log.h"

#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <unistd.h>
#include <utility>

namespace lesh::ui {

namespace {

// The arena of the worker this call is running on.
//
// A thread_local pointer rather than an argument because the client is native
// reactor code reached through a C function pointer that cannot carry one -
// syntax::parse(*current_worker_arena(), snapshot) is what #124 will write.
thread_local buffer_pool* t_worker_arena = nullptr;

// A thread's identity as a plain integer.
//
// DELIBERATELY THE SAME COMPUTATION as registry.cpp's, which keeps its copy in
// an anonymous namespace and is not this ticket's file to change. The two must
// agree or every accessor on a worker's token would refuse, so `compute` below
// asserts token_is_live() on a token it has just built - the duplication is
// held to account by a check rather than by a comment.
std::uint64_t worker_thread_key() noexcept {
	const std::uint64_t key =
		static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
	return key == 0 ? 1 : key;
}

} // namespace

buffer_pool* current_worker_arena() noexcept { return t_worker_arena; }

request_snapshot snapshot_of(const leshper::state& target, std::uint32_t event_kind) {
	request_snapshot taken;
	taken.buffer.assign(target.buffer.text());
	taken.cursor = target.cursor.byte_offset();
	// The derived region, exactly as `loop_harness::react` takes it (#116 landed
	// the model after this function was written, and a snapshot that reported
	// every selection as inactive would make #129's `selection_changed` fan-out
	// wake reactors with nothing to look at). Reported even when inactive, on
	// the reasoning `lesh_selection_get` gives: the anchor outlives
	// deactivation and the flag is the separate question.
	{
		const std::size_t anchor = target.selection_anchor().byte_offset();
		const std::size_t head = taken.cursor;
		taken.selection_start = anchor < head ? anchor : head;
		taken.selection_end = anchor < head ? head : anchor;
		taken.selection_active = target.selection_active();
	}
	taken.computed_against = target.gen;
	taken.event_kind = event_kind;
	return taken;
}

// ---------------------------------------------------------------------------
// Pooled messages
// ---------------------------------------------------------------------------

completion::completion(completion&& other) noexcept
	: _owner(other._owner), _batch(other._batch) {
	other._owner = nullptr;
	other._batch = nullptr;
}

completion& completion::operator=(completion&& other) noexcept {
	if (this != &other) {
		recycle();
		_owner = other._owner;
		_batch = other._batch;
		other._owner = nullptr;
		other._batch = nullptr;
	}
	return *this;
}

completion::~completion() { recycle(); }

void completion::recycle() noexcept {
	if (_owner != nullptr && _batch != nullptr)
		_owner->release(_batch);
	_owner = nullptr;
	_batch = nullptr;
}

message_pool::~message_pool() {
	// Every message handed out must be back before the storage behind it goes.
	// ADR-0007 makes the leak gate binary, and a completion outliving its pool
	// would be the one report nobody could call a defect.
	LESH_ASSERT(_live == 0);
}

completion message_pool::acquire() {
	std::lock_guard lock(_mutex);
	++_live;
	if (!_free.empty()) {
		leshper::reactor_batch* const reused = _free.back();
		_free.pop_back();
		return completion{this, reused};
	}
	_owned.push_back(std::make_unique<leshper::reactor_batch>());
	// Room for this one to come home, reserved now: release() is noexcept and a
	// free list that could fail to grow would have nowhere to put the message.
	_free.reserve(_owned.size());
	return completion{this, _owned.back().get()};
}

void message_pool::release(leshper::reactor_batch* batch) noexcept {
	// clear(), never shrink. Keeping the vectors' capacity across reuse is the
	// whole of what "pooled" buys: a highlight batch the size of the last one
	// allocates nothing at all (N-2).
	batch->reactor.clear();
	batch->computed_against = leshper::generation{};
	batch->event_kind = 0;
	batch->status = LESH_OK;
	batch->spans.clear();
	batch->texts.clear();
	batch->proposals.clear();

	std::lock_guard lock(_mutex);
	_free.push_back(batch);
	--_live;
}

std::size_t message_pool::live() const noexcept {
	std::lock_guard lock(_mutex);
	return _live;
}

std::size_t message_pool::minted() const noexcept {
	std::lock_guard lock(_mutex);
	return _owned.size();
}

// ---------------------------------------------------------------------------
// The completion queue
// ---------------------------------------------------------------------------

completion_queue::completion_queue() {
	int fds[2] = {-1, -1};
	const int made = ::pipe(fds);
	LESH_ASSERT(made == 0);
	if (made != 0)
		return;
	_read_fd = fds[0];
	_write_fd = fds[1];
	// Non-blocking, so a worker posting a result never stalls on the pipe; and
	// close-on-exec, because #91's editing-time posix_spawn must not hand the
	// editor's wakeup to a child.
	for (const int fd : {_read_fd, _write_fd}) {
		::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
		::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
	}
}

completion_queue::~completion_queue() {
	if (_read_fd >= 0)
		::close(_read_fd);
	if (_write_fd >= 0)
		::close(_write_fd);
}

void completion_queue::post(completion done) {
	std::lock_guard lock(_mutex);
	_queue.push_back(std::move(done));
	// Armed on the empty-to-non-empty transition only, so a burst of N results
	// costs one byte and one loop wakeup rather than N of each. The pipe
	// therefore never holds more than one byte and cannot fill.
	if (!_armed && _write_fd >= 0) {
		_armed = true;
		const unsigned char poke = 1;
		while (::write(_write_fd, &poke, 1) < 0 && errno == EINTR) {}
	}
	_arrived.notify_all();
}

std::size_t completion_queue::drain_locked(std::vector<completion>& out) {
	// #128's `worker` topic is level-triggered and lossy, and its rule is
	// "consume the fd before draining the queue" so that a result arriving
	// between the two is not lost. Here the two happen under ONE lock, so
	// nothing can arrive between them and the order does not matter - a post
	// racing this call either lands before it and is drained, or lands after it
	// and re-arms the fd. That is why the loop must answer a readable fd with
	// this function rather than reading the fd itself.
	const std::size_t arrived = _queue.size();
	for (auto& done : _queue)
		out.push_back(std::move(done));
	_queue.clear();
	if (_armed) {
		_armed = false;
		unsigned char sink[16];
		for (;;) {
			const ssize_t got = ::read(_read_fd, sink, sizeof sink);
			if (got > 0)
				continue;
			if (got < 0 && errno == EINTR)
				continue;
			break;
		}
	}
	return arrived;
}

std::size_t completion_queue::drain(std::vector<completion>& out) {
	std::lock_guard lock(_mutex);
	return drain_locked(out);
}

std::size_t completion_queue::wait_and_drain(std::vector<completion>& out, std::size_t least) {
	std::unique_lock lock(_mutex);
	_arrived.wait(lock, [this, least] { return _queue.size() >= least; });
	return drain_locked(out);
}

bool completion_queue::empty() const {
	std::lock_guard lock(_mutex);
	return _queue.empty();
}

std::size_t completion_queue::size() const {
	std::lock_guard lock(_mutex);
	return _queue.size();
}

bool completion_queue::armed() const {
	std::lock_guard lock(_mutex);
	return _armed;
}

// ---------------------------------------------------------------------------
// The pool
// ---------------------------------------------------------------------------

std::size_t worker_pool::default_worker_count() noexcept {
	// FOUR, OR THE HARDWARE IF IT IS NARROWER.
	//
	// Four because four is how many independent computations leshper has at a
	// keystroke, and a fifth worker would have nothing to take: the A-4/#94 set
	// is the highlighter (F-20), the autosuggester (F-24), the completer (F-28)
	// and the history searcher (F-32). Each owns one latest-wins slot whose
	// depth is at most one by type, so the pool's ceiling is the number of
	// SLOTS, not the number of cores - concurrency here comes from how many
	// different questions are outstanding, and there are four of them.
	//
	// Bounded by the hardware because the loop thread has to answer the
	// keystroke inside N-1's millisecond: on a two-core machine, four compute
	// threads contend with the one thread whose latency is the requirement.
	//
	// And never zero: hardware_concurrency() is permitted to answer 0 when it
	// cannot tell, and a pool of no workers would accept submissions that never
	// run.
	//
	// The number is not what makes quiesce expensive. #115 measured park at
	// 3.2-4.4 us with four workers on 1 us tasks and at 100 us on 100 us tasks -
	// the cost is task granularity, and it is why the check-in matters and this
	// constant does not, much.
	constexpr std::size_t ceiling = 4;
	const unsigned hardware = std::thread::hardware_concurrency();
	const std::size_t cores = hardware == 0 ? std::size_t{1} : std::size_t{hardware};
	return cores < ceiling ? cores : ceiling;
}

worker_pool::worker_pool(std::size_t workers, std::size_t arena_bytes) {
	LESH_ASSERT(workers > 0);
	if (workers == 0)
		workers = 1;
	_workers.reserve(workers);
	for (std::size_t i = 0; i < workers; ++i)
		_workers.push_back(std::make_unique<worker>(arena_bytes));
	// Started after all of them exist, so `_workers.size()` is already final
	// when the first worker reads it at a check-in.
	for (auto& each : _workers)
		each->thread = std::thread([this, w = each.get()] { run(*w); });
}

worker_pool::~worker_pool() {
	{
		std::lock_guard lock(_mutex);
		_stopping = true;
		// A pool torn down while parked still has to let its workers out.
		_park_requests = 0;
	}
	_work.notify_all();
	_released.notify_all();
	for (auto& each : _workers) {
		if (each->thread.joinable())
			each->thread.join();
	}
	// Everything else frees itself in declaration order: the queue goes before
	// the message pool, so an undrained completion is handed back to storage
	// that is still alive, and the pool then frees every batch it ever minted
	// (ADR-0007, leak gate zero).
}

void worker_pool::submit(std::string_view key, request_snapshot snapshot,
                         lesh_reactor_fn fn, void* userdata) {
	LESH_ASSERT(fn != nullptr);
	if (fn == nullptr)
		return;

	std::lock_guard lock(_mutex);
	auto found = _slots.find(key);
	if (found == _slots.end()) {
		found = _slots.try_emplace(std::string{key}).first;
		found->second.name = found->first;
	}
	slot& s = found->second;

	// Latest-wins. What was pending is DROPPED rather than queued behind - "if
	// something is waiting then we drop it, as we need only the latest" - and
	// what is in flight is told, through the poll the ABI already has, that
	// nobody wants its answer any more.
	if (s.pending.valid)
		++_dropped;
	s.pending.snapshot = std::move(snapshot);
	s.pending.fn = fn;
	s.pending.userdata = userdata;
	s.pending.valid = true;
	if (s.in_flight)
		s.superseded.store(true, std::memory_order_relaxed);

	// A slot that is in flight is not runnable: that is what makes queue depth
	// at most one a property of the type rather than a policy anyone enforces.
	if (!s.in_flight && !s.queued) {
		s.queued = true;
		_ready.push_back(&s);
		_work.notify_one();
	}
}

void worker_pool::supersede_all() noexcept {
	std::lock_guard lock(_mutex);
	for (auto& [name, s] : _slots) {
		if (s.pending.valid) {
			s.pending = task{};
			++_dropped;
		}
		if (s.in_flight)
			s.superseded.store(true, std::memory_order_relaxed);
	}
	for (slot* const ready : _ready)
		ready->queued = false;
	_ready.clear();
}

void worker_pool::park_all() {
	std::unique_lock lock(_mutex);
	++_park_requests;
	if (_park_requests == 1) {
		// #115's lever, and the whole of the fine-grained check-in: parking
		// SUPERSEDES what is in flight, so a compute that polls returns now
		// rather than at the end of its task. F-22 abandons a pending highlight
		// at accept anyway, which is the moment this is called.
		for (auto& [name, s] : _slots) {
			if (s.in_flight)
				s.superseded.store(true, std::memory_order_relaxed);
		}
	}
	_work.notify_all();
	_parked.wait(lock, [this] {
		return _stopping || _parked_count == _workers.size();
	});
}

void worker_pool::resume() {
	std::unique_lock lock(_mutex);
	LESH_ASSERT(_park_requests != 0);
	if (_park_requests == 0)
		return;
	--_park_requests;
	if (_park_requests != 0)
		return;
	lock.unlock();
	_released.notify_all();
}

bool worker_pool::is_quiesced() const noexcept {
	std::lock_guard lock(_mutex);
	return _park_requests != 0 && _parked_count == _workers.size();
}

void worker_pool::assert_quiesced() const noexcept {
	// #91 chose crash-on-violation over pthread_atfork: the fork site added by
	// someone who never read that ticket fails under the sanitized gate instead
	// of producing a child holding a mutex nobody will unlock.
	LESH_ASSERT(is_quiesced());
}

std::size_t worker_pool::started() const noexcept {
	std::lock_guard lock(_mutex);
	return _started;
}

std::size_t worker_pool::completed() const noexcept {
	std::lock_guard lock(_mutex);
	return _completed;
}

std::size_t worker_pool::dropped() const noexcept {
	std::lock_guard lock(_mutex);
	return _dropped;
}

void worker_pool::run(worker& me) {
	// FIRST THING IN THE BODY (#142), before the arena and before the mutex. A
	// process-directed signal lands on any one thread that does not block it, and
	// a helper is the worst possible choice: it would run the shell's handler
	// while parked in a highlight, on a thread that owns no shell state at all.
	// Blocked here and on the loop thread, delivery is pinned to main - which is
	// the shell thread, the one that writes the dispositions and the one that
	// must stay unmasked because a mask survives `execve` and main is the only
	// thread that forks (ADR-0009). A helper that ever gains children must lose
	// this line.
	//
	// A failure is logged and not fatal: the worst case is the nondeterministic
	// delivery this whole session used to have.
	if (!block_caught_signals_on_this_thread())
		LESH_LOG(log::level::warn, log::category::loop,
		         "a helper thread could not block the caught signals");

	t_worker_arena = &me.arena;

	std::unique_lock lock(_mutex);
	for (;;) {
		// THE CHECK-IN. Everything a worker does between two passes through here
		// is one task, which is why park latency is task granularity, and why
		// park_all() supersedes what is in flight rather than waiting it out.
		//
		// The wait releases the mutex, so a parked worker holds nothing: that is
		// #91's requirement, and it is a property of condition_variable rather
		// than of anything written here.
		while (_park_requests != 0 && !_stopping) {
			++_parked_count;
			_parked.notify_all();
			_released.wait(lock);
			--_parked_count;
		}
		if (_stopping)
			break;
		if (_ready.empty()) {
			_work.wait(lock);
			continue;
		}

		slot* const owner = _ready.front();
		_ready.pop_front();
		owner->queued = false;
		task job = std::move(owner->pending);
		owner->pending = task{};
		owner->in_flight = true;
		owner->superseded.store(false, std::memory_order_relaxed);
		++_started;

		lock.unlock();

		completion done = _messages.acquire();
		compute(job, *owner, done.batch());
		// Request end. The arena rewinds to its base, so nothing the compute
		// allocated survives - #90's lifetime rule, which is also N-4 for free:
		// the memory behind a stale answer is gone rather than merely ignored.
		me.arena.reset(me.arena_base);
		_completions.post(std::move(done));

		lock.lock();
		owner->in_flight = false;
		++_completed;
		if (owner->pending.valid && !owner->queued) {
			owner->queued = true;
			_ready.push_back(owner);
			_work.notify_one();
		}
	}

	t_worker_arena = nullptr;
}

void worker_pool::compute(task& job, slot& owner, leshper::reactor_batch& into) {
	into.reactor = owner.name;
	into.computed_against = job.snapshot.computed_against;
	into.event_kind = job.snapshot.event_kind;

	// The token is registry.h's, unchanged. This ticket runs it on a worker; it
	// does not redesign it.
	leshper::request_token token;
	token.buffer = std::move(job.snapshot.buffer);
	token.cursor = job.snapshot.cursor;
	token.selection_start = job.snapshot.selection_start;
	token.selection_end = job.snapshot.selection_end;
	token.selection_active = job.snapshot.selection_active;
	token.computed_against = job.snapshot.computed_against;
	token.event_kind = job.snapshot.event_kind;
	token.superseded = &owner.superseded;
	token.host = job.snapshot.host;
	token.spans = &into.spans;
	token.texts = &into.texts;
	token.proposals = &into.proposals;
	token.owner_thread = worker_thread_key();
	token.call_token = _call_tokens.fetch_add(1, std::memory_order_relaxed);

	// The one thing that would silently break if the duplicated thread key
	// above ever disagreed with registry.cpp's: every accessor would refuse.
	LESH_ASSERT(leshper::token_is_live(&token));

	into.status = job.fn(&token, job.userdata);

	// Dead the moment the compute returns, exactly as on the loop thread.
	token.call_token = 0;
	token.owner_thread = 0;
}

} // namespace lesh::ui
