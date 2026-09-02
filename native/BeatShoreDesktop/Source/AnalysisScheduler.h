#pragma once
// Shared job queue, per-request cancellation state machine, and per-session
// outgoing-message routing for BeatShoreDesktop's multi-session
// architecture. Pure data structures plus Win32 event handles for wakeups
// -- no OverlappedPipeIO/pipe handles here at all, so these are unit-tested
// without any real named-pipe I/O (see
// native/BridgeClientTest/Source/scheduler_test.cpp).
//
// Exists to let the desktop service HEARTBEAT/CANCEL/other sessions while
// an ANALYSIS_REQUEST is in flight, without splitting a given pipe
// handle's reads and writes across threads -- see main.cpp's top-of-file
// threading comment for the empirically-found hang that constraint guards
// against. Each session's PipeSessionOwner thread is still the only thread
// that ever touches its own pipe handle; this file is how work and results
// move between that thread and the worker thread(s) actually running
// requests, without either side reaching into the other's pipe.
#include <windows.h>
#include <string>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <unordered_map>
#include <atomic>
#include <vector>

enum class JobState { Queued, Running, CancelRequested, Cancelled, Completed, Failed, TimedOut };

inline const char* jobStateName(JobState s)
{
    switch (s)
    {
        case JobState::Queued:          return "QUEUED";
        case JobState::Running:         return "RUNNING";
        case JobState::CancelRequested: return "CANCEL_REQUESTED";
        case JobState::Cancelled:       return "CANCELLED";
        case JobState::Completed:       return "COMPLETED";
        case JobState::Failed:          return "FAILED";
        case JobState::TimedOut:        return "TIMED_OUT";
    }
    return "UNKNOWN";
}

struct AnalysisJob
{
    std::string sessionId;
    std::string requestId;
    std::string kind;
    std::string audioSource;
    std::string hostTrackName;
    std::string tempAudioPath; // already-dumped bsmraw file, built by the pipe-session thread before enqueueing
    uint64_t reservedAudioBytes = 0; // counted against g_reservedAudioBytes (main.cpp) at admission time, released exactly once via releaseJobResources()
    std::atomic<bool> resourcesReleased { false }; // guards releaseJobResources() so tempAudioPath is deleted and reservedAudioBytes is released exactly once, no matter which of several possible code paths reaches this job's terminal state first
    double tempo = 0.0;        // 0 = not provided (role/tempo, same optional fields ANALYSIS_REQUEST always had)
    std::string role;

    // Optional humanization amounts (0.0-1.0, 0 = not provided/not
    // requested), forwarded to analyze.js's dsp.applyHumanization() for
    // the three MIDI-producing kinds only -- see that function's own
    // comment for what each one actually does. Same "0 = absent" convention
    // as tempo above, not a separate bool per field.
    double humanizeTiming = 0.0, humanizeVelocity = 0.0, humanizeDynamics = 0.0, humanizeArticulation = 0.0;
    bool preserveGroove = false;
    std::atomic<JobState> state { JobState::Queued };

    // Set by whichever worker thread picks this job up (Queued->Running),
    // cleared once it reaches a terminal state. A CANCEL handler that finds
    // this job Running signals exactly this event -- not a shared/global
    // one -- so only the one worker actually processing this job wakes for
    // it, which is what lets multiple workers (see STATUS.md's
    // maxConcurrentNodeJobs) each get cancelled independently without
    // spurious wakes storming every other worker on every cancellation.
    // Safe even under races: a worker that wakes because of a stale/
    // no-longer-relevant signal just re-checks its OWN current job's state
    // (never trusts "I was woken" as meaning "I was the target") and loops
    // back if it doesn't match CancelRequested.
    std::atomic<HANDLE> assignedWorkerWakeEvent { nullptr };
};

// Marks a Queued job Cancelled immediately (nothing was running to
// interrupt), or a Running job CancelRequested and wakes the specific
// worker processing it (see assignedWorkerWakeEvent above). No-op --
// correctly -- if the job already reached any other state (already
// terminal, or a second CANCEL racing the first). Returns the resulting
// state so the caller can report ALREADY_COMPLETED vs. a genuine
// CANCELLED/CANCEL_REQUESTED back to whoever sent the CANCEL.
inline JobState requestCancel(const std::shared_ptr<AnalysisJob>& job)
{
    JobState expected = JobState::Queued;
    if (job->state.compare_exchange_strong(expected, JobState::Cancelled))
        return JobState::Cancelled;

    expected = JobState::Running;
    if (job->state.compare_exchange_strong(expected, JobState::CancelRequested))
    {
        HANDLE workerEvt = job->assignedWorkerWakeEvent.load();
        if (workerEvt != nullptr) SetEvent(workerEvt);
        return JobState::CancelRequested;
    }

    return job->state.load(); // already terminal, or already CancelRequested from an earlier CANCEL
}

// FIFO of pending jobs, consumed by worker thread(s). push() is called from
// any PipeSessionOwner thread; waitPop() by worker threads only -- safe for
// more than one waitPop() caller (a std::condition_variable with multiple
// waiters is fine; each push wakes exactly one).
class JobQueue
{
public:
    void push(std::shared_ptr<AnalysisJob> job)
    {
        { std::lock_guard<std::mutex> lock(mutex_); queue_.push_back(std::move(job)); }
        cv_.notify_one();
    }

    // Blocks until a job is available or shouldExit becomes true. A
    // spurious wake with an empty queue and shouldExit still false just
    // means the predicate re-checks and waits again -- normal
    // condition_variable usage, not a bug.
    std::shared_ptr<AnalysisJob> waitPop(std::atomic<bool>& shouldExit)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]{ return !queue_.empty() || shouldExit.load(); });
        if (queue_.empty()) return nullptr;
        auto job = queue_.front();
        queue_.pop_front();
        return job;
    }

    void wakeAll() { cv_.notify_all(); } // unsticks waitPop() when shouldExit flips with an empty queue

    // Snapshot, not a guarantee -- another push()/waitPop() can race
    // immediately after this returns. Used only for a soft admission
    // check (reject a new request if the queue is already very deep), not
    // anything requiring exact synchronization.
    size_t size()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<AnalysisJob>> queue_;
};

// All jobs, keyed by requestId, for CANCEL lookups and ALREADY_COMPLETED/
// REQUEST_NOT_FOUND reporting. Bounded like the single-session desktop's
// previous 50-entry recentlyCompleted cache (main.cpp, now replaced by
// this): a job is only evicted once it's reached a terminal state AND the
// registry is over kMaxEntries, oldest first -- never evicts something
// still QUEUED/RUNNING/CANCEL_REQUESTED, even if that means briefly
// exceeding the cap under heavy concurrent load (a soft cap on stale
// history, not a hard limit on in-flight work). An evicted terminal job
// just means a very late CANCEL for it reports REQUEST_NOT_FOUND instead
// of ALREADY_COMPLETED -- both mean the same thing to a caller ("nothing
// left to cancel"), so this is an honest simplification, not a lie.
class JobRegistry
{
public:
    static constexpr size_t kMaxEntries = 500;

    void add(std::shared_ptr<AnalysisJob> job)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        insertionOrder_.push_back(job->requestId);
        jobs_[job->requestId] = std::move(job);
        evictIfNeeded();
    }

    std::shared_ptr<AnalysisJob> find(const std::string& requestId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = jobs_.find(requestId);
        return it != jobs_.end() ? it->second : nullptr;
    }

    // Every non-terminal job belonging to a session -- used to cancel a
    // disconnected session's outstanding work so it doesn't run forever
    // unbounded for a client that's no longer listening.
    std::vector<std::shared_ptr<AnalysisJob>> findActiveForSession(const std::string& sessionId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::shared_ptr<AnalysisJob>> result;
        for (auto& entry : jobs_)
        {
            auto& job = entry.second;
            if (job->sessionId != sessionId) continue;
            auto s = job->state.load();
            if (s == JobState::Queued || s == JobState::Running || s == JobState::CancelRequested)
                result.push_back(job);
        }
        return result;
    }

    // Every non-terminal job across every session -- used by graceful
    // shutdown to cancel all outstanding work system-wide, not just one
    // session's.
    std::vector<std::shared_ptr<AnalysisJob>> findAllActive()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::shared_ptr<AnalysisJob>> result;
        for (auto& entry : jobs_)
        {
            auto s = entry.second->state.load();
            if (s == JobState::Queued || s == JobState::Running || s == JobState::CancelRequested)
                result.push_back(entry.second);
        }
        return result;
    }

    size_t size() { std::lock_guard<std::mutex> lock(mutex_); return jobs_.size(); }

private:
    // Scans in original insertion order (oldest first), evicting the
    // oldest *terminal* entries until under the cap -- skipping over any
    // live entry it encounters rather than stopping at it. A live job
    // sitting near the front (started long ago, still running) must not
    // block eviction of terminal jobs that come after it in insertion
    // order; the earlier version of this method used `break` on the first
    // live entry and got stuck permanently once that happened, letting the
    // registry grow unboundedly -- caught by SchedulerTest, not by
    // reasoning about the code.
    void evictIfNeeded()
    {
        if (insertionOrder_.size() <= kMaxEntries) return;
        size_t toEvict = insertionOrder_.size() - kMaxEntries;
        std::deque<std::string> kept;
        for (auto& id : insertionOrder_)
        {
            if (toEvict > 0)
            {
                auto it = jobs_.find(id);
                if (it == jobs_.end()) { --toEvict; continue; } // stale order entry, already gone
                auto s = it->second->state.load();
                if (s != JobState::Queued && s != JobState::Running && s != JobState::CancelRequested)
                {
                    jobs_.erase(it);
                    --toEvict;
                    continue;
                }
            }
            kept.push_back(id);
        }
        insertionOrder_.swap(kept);
    }

    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<AnalysisJob>> jobs_;
    std::deque<std::string> insertionOrder_;
};

// One outgoing-message mailbox per connected session. Any thread (a worker
// finishing a job, another session's CANCEL handler, etc.) can hand a
// message to "whichever thread owns session X's pipe" via this, without
// ever touching that pipe handle -- only the session's own
// PipeSessionOwner thread ever calls WriteFile on it, preserving the
// single-thread-per-pipe-handle rule.
class SessionOutbox
{
public:
    explicit SessionOutbox(HANDLE wakeEvent) : wakeEvent_(wakeEvent) {}

    void push(std::string jsonLine)
    {
        { std::lock_guard<std::mutex> lock(mutex_); queue_.push_back(std::move(jsonLine)); }
        SetEvent(wakeEvent_);
    }

    // Drains everything currently queued -- called by the session's own
    // thread after its wait wakes on wakeEvent_. Resets the event as part
    // of the same locked section a concurrent push() also touches, so a
    // push() that arrives between "we finished draining" and "we reset the
    // event" is never lost (it re-signals after our reset, guaranteeing
    // the next wait sees it).
    std::vector<std::string> drain()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> out = std::move(queue_);
        queue_.clear();
        ResetEvent(wakeEvent_);
        return out;
    }

private:
    HANDLE wakeEvent_;
    std::mutex mutex_;
    std::vector<std::string> queue_;
};

// Maps sessionId -> that session's outbox, so a worker thread that just
// finished a job can route a result to the right session without holding a
// direct reference to that session's PipeSessionOwner object (which may
// have already been destroyed if the session disconnected -- find()
// returning nullptr is exactly that case, and callers must treat a
// nullptr result as "nowhere to deliver this, discard it" rather than a
// bug).
class SessionRegistry
{
public:
    void add(const std::string& sessionId, std::shared_ptr<SessionOutbox> outbox)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_[sessionId] = std::move(outbox);
    }

    void remove(const std::string& sessionId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.erase(sessionId);
    }

    std::shared_ptr<SessionOutbox> find(const std::string& sessionId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(sessionId);
        return it != sessions_.end() ? it->second : nullptr;
    }

    // Pushes the same message to every currently-connected session's
    // outbox -- used by graceful shutdown to tell every connected client
    // the broker is going away, not just the one that happened to send
    // the request that triggered it.
    void broadcast(const std::string& jsonLine)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : sessions_) entry.second->push(jsonLine);
    }

    size_t count()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return sessions_.size();
    }

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<SessionOutbox>> sessions_;
};
