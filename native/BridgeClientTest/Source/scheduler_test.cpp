// Unit tests for AnalysisScheduler.h's pure data structures -- no pipes, no
// NodeEngine, no real I/O at all. Verifies JobQueue ordering/blocking,
// JobRegistry's eviction-protects-live-jobs invariant, and
// SessionOutbox/SessionRegistry's routing + event-wake behavior in
// isolation before any of it is wired into the real multi-threaded
// desktop.
#include "../../BeatShoreDesktop/Source/AnalysisScheduler.h"
#include <thread>
#include <chrono>
#include <iostream>
#include <vector>

static int failures = 0;
static void check(bool cond, const std::string& label)
{
    std::cout << "[test] " << label << ": " << (cond ? "PASS" : "FAIL") << std::endl;
    if (!cond) failures++;
}

static std::shared_ptr<AnalysisJob> makeJob(const std::string& sessionId, const std::string& requestId)
{
    auto job = std::make_shared<AnalysisJob>();
    job->sessionId = sessionId;
    job->requestId = requestId;
    job->kind = "tempo";
    return job;
}

int main()
{
    // --- JobQueue: FIFO order, blocking waitPop, multi-producer safety ---
    {
        std::cout << "\n=== JobQueue ===" << std::endl;
        JobQueue q;
        std::atomic<bool> shouldExit { false };

        q.push(makeJob("s1", "r1"));
        q.push(makeJob("s1", "r2"));
        auto j1 = q.waitPop(shouldExit);
        auto j2 = q.waitPop(shouldExit);
        check(j1 && j1->requestId == "r1", "FIFO order: first pushed is first popped");
        check(j2 && j2->requestId == "r2", "FIFO order: second pushed is second popped");

        // waitPop blocks until something is pushed from another thread.
        std::atomic<bool> popped { false };
        std::thread popper([&]{ auto j = q.waitPop(shouldExit); popped.store(j != nullptr); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        check(!popped.load(), "waitPop genuinely blocks with an empty queue (not a busy-loop that returns early)");
        q.push(makeJob("s2", "r3"));
        popper.join();
        check(popped.load(), "waitPop unblocks once a job is pushed");

        // shouldExit unsticks a blocked waitPop with an empty queue.
        std::atomic<bool> exited { false };
        std::thread waiter([&]{ auto j = q.waitPop(shouldExit); exited.store(j == nullptr); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        shouldExit.store(true);
        q.wakeAll();
        waiter.join();
        check(exited.load(), "shouldExit + wakeAll unsticks a blocked waitPop, returning nullptr");

        JobQueue q2;
        std::atomic<bool> neverExit { false };
        check(q2.size() == 0, "size() reports 0 on an empty queue");
        q2.push(makeJob("s", "a"));
        q2.push(makeJob("s", "b"));
        check(q2.size() == 2, "size() reports the correct count after pushes");
        q2.waitPop(neverExit);
        check(q2.size() == 1, "size() reflects a pop");
    }

    // --- JobRegistry: lookup, and eviction never drops live work ---
    {
        std::cout << "\n=== JobRegistry ===" << std::endl;
        JobRegistry reg;
        auto j = makeJob("s1", "findme");
        reg.add(j);
        auto found = reg.find("findme");
        check(found && found->requestId == "findme", "find() returns the added job");
        check(reg.find("nope") == nullptr, "find() returns nullptr for an unknown requestId");

        // Fill past kMaxEntries with a mix of terminal and live jobs; only
        // terminal ones should ever be evicted, regardless of insertion
        // order.
        JobRegistry reg2;
        auto liveJob = makeJob("s1", "still-running");
        liveJob->state.store(JobState::Running);
        reg2.add(liveJob); // inserted FIRST -- would be evicted first under naive FIFO eviction
        for (size_t i = 0; i < JobRegistry::kMaxEntries + 50; ++i)
        {
            auto terminalJob = makeJob("s1", "terminal-" + std::to_string(i));
            terminalJob->state.store(JobState::Completed);
            reg2.add(terminalJob);
        }
        check(reg2.find("still-running") != nullptr, "a live (Running) job survives eviction even though it was inserted first");
        check(reg2.size() <= JobRegistry::kMaxEntries + 1, "registry stays close to the cap once only terminal jobs remain evictable");

        // findActiveForSession: only Queued/Running/CancelRequested count.
        JobRegistry reg3;
        auto queued = makeJob("target", "q1"); queued->state.store(JobState::Queued);
        auto running = makeJob("target", "r1"); running->state.store(JobState::Running);
        auto cancelReq = makeJob("target", "cr1"); cancelReq->state.store(JobState::CancelRequested);
        auto done = makeJob("target", "d1"); done->state.store(JobState::Completed);
        auto otherSession = makeJob("other", "o1"); otherSession->state.store(JobState::Running);
        reg3.add(queued); reg3.add(running); reg3.add(cancelReq); reg3.add(done); reg3.add(otherSession);
        auto active = reg3.findActiveForSession("target");
        check(active.size() == 3, "findActiveForSession returns exactly the Queued/Running/CancelRequested jobs for that session");

        auto allActive = reg3.findAllActive();
        check(allActive.size() == 4, "findAllActive returns every non-terminal job across all sessions (3 from 'target' + 1 from 'other')");
    }

    // --- SessionOutbox / SessionRegistry: push wakes the event, drain
    // clears it and returns everything in order, routing is by sessionId. ---
    {
        std::cout << "\n=== SessionOutbox / SessionRegistry ===" << std::endl;
        HANDLE wakeEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        auto outbox = std::make_shared<SessionOutbox>(wakeEvent);

        check(WaitForSingleObject(wakeEvent, 0) == WAIT_TIMEOUT, "event starts unsignaled");
        outbox->push("first");
        outbox->push("second");
        check(WaitForSingleObject(wakeEvent, 0) == WAIT_OBJECT_0, "push signals the wake event");
        auto drained = outbox->drain();
        check(drained.size() == 2 && drained[0] == "first" && drained[1] == "second", "drain returns everything pushed, in order");
        check(WaitForSingleObject(wakeEvent, 0) == WAIT_TIMEOUT, "drain resets the wake event");

        SessionRegistry registry;
        registry.add("session-a", outbox);
        check(registry.find("session-a") == outbox, "registry routes to the correct session's outbox");
        check(registry.find("session-b") == nullptr, "registry returns nullptr for an unknown/disconnected session");
        check(registry.count() == 1, "count() reflects one registered session");

        HANDLE wakeEvent2 = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        auto outbox2 = std::make_shared<SessionOutbox>(wakeEvent2);
        registry.add("session-b", outbox2);
        check(registry.count() == 2, "count() reflects two registered sessions");
        registry.broadcast("shutdown notice");
        auto drained1 = outbox->drain();
        auto drained2 = outbox2->drain();
        check(drained1.size() == 1 && drained1[0] == "shutdown notice", "broadcast reaches session-a's outbox");
        check(drained2.size() == 1 && drained2[0] == "shutdown notice", "broadcast reaches session-b's outbox too, not just one");
        CloseHandle(wakeEvent2);

        registry.remove("session-a");
        check(registry.find("session-a") == nullptr, "remove() actually removes -- a disconnected session's outbox is not reachable afterward");

        CloseHandle(wakeEvent);
    }

    // --- requestCancel: state machine transitions ---
    {
        std::cout << "\n=== requestCancel ===" << std::endl;

        auto queued = makeJob("s", "q");
        auto r1 = requestCancel(queued);
        check(r1 == JobState::Cancelled && queued->state.load() == JobState::Cancelled, "cancelling a QUEUED job goes straight to CANCELLED");

        auto running = makeJob("s", "r");
        running->state.store(JobState::Running);
        HANDLE workerEvt = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        running->assignedWorkerWakeEvent.store(workerEvt);
        auto r2 = requestCancel(running);
        check(r2 == JobState::CancelRequested && running->state.load() == JobState::CancelRequested, "cancelling a RUNNING job goes to CANCEL_REQUESTED, not straight to CANCELLED");
        check(WaitForSingleObject(workerEvt, 0) == WAIT_OBJECT_0, "cancelling a RUNNING job signals its assigned worker's wake event");
        CloseHandle(workerEvt);

        auto completed = makeJob("s", "c");
        completed->state.store(JobState::Completed);
        auto r3 = requestCancel(completed);
        check(r3 == JobState::Completed, "cancelling an already-COMPLETED job is a no-op, reports the real terminal state");

        auto alreadyCancelRequested = makeJob("s", "cr");
        alreadyCancelRequested->state.store(JobState::CancelRequested);
        auto r4 = requestCancel(alreadyCancelRequested);
        check(r4 == JobState::CancelRequested, "a second CANCEL for an already-CANCEL_REQUESTED job is a harmless no-op, not an error");

        // A cancel racing a job with no assigned worker yet (e.g. between
        // Queued->Running and the worker storing its event) must not
        // crash -- SetEvent(nullptr) would be undefined, so this must be
        // guarded.
        auto runningNoWorker = makeJob("s", "rnw");
        runningNoWorker->state.store(JobState::Running);
        auto r5 = requestCancel(runningNoWorker);
        check(r5 == JobState::CancelRequested, "cancelling a RUNNING job with no assigned worker event yet still transitions state correctly (and doesn't crash)");
    }

    std::cout << "\n" << (failures == 0 ? "[test] ALL PASSED" : "[test] FAILURES: " + std::to_string(failures)) << std::endl;
    return failures == 0 ? 0 : 1;
}
