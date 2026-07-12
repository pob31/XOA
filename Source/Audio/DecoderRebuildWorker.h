#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <utility>

#include "DSP/AmbiDecoderDesigner.h"

//==============================================================================
// XOA - background decoder design worker (WP7). AllRAD at order 10 on a large
// rig is seconds of pure math; running it on the message thread would block the
// UI (PRD sec.7: rebuild must be non-blocking). This one persistent normal-
// priority thread runs decoder::design() off the message thread; the caller
// marshals the cheap buffer-fill + publish back via onResultReady (which fires
// on the WORKER thread and is expected to hop to the message thread, e.g. via
// juce::AsyncUpdater).
//
// One-slot latest-wins: submitting again while a design runs replaces the
// pending job, so a drag storm collapses to (at most) the current design plus
// the newest queued one. A generation stamp lets the consumer discard results
// that a later submit or a synchronous flush has superseded.
//==============================================================================

namespace xoa
{

class DecoderRebuildWorker : private juce::Thread
{
public:
    struct Job
    {
        decoder::SpeakerLayout layout;
        decoder::DesignOptions options;
        juce::uint64 generation = 0;
    };

    explicit DecoderRebuildWorker (std::function<void()> onResultReady)
        : juce::Thread ("xoa decoder rebuild"), notifyReady (std::move (onResultReady))
    {
        startThread (juce::Thread::Priority::normal);
    }

    ~DecoderRebuildWorker() override { stop(); }

    /** Signal + join the worker. Idempotent; call it from the owner's
        destructor BEFORE tearing down anything onResultReady touches. */
    void stop()
    {
        signalThreadShouldExit();
        notify();               // wake run() out of wait(-1)
        stopThread (5000);
    }

    /** Queue a design (message thread). Latest-wins: a newer job replaces any
        not-yet-started one. */
    void submit (const Job& job)
    {
        {
            const juce::ScopedLock sl (lock);
            pending = job;
            hasPending = true;
            busy.store (true, std::memory_order_release);
        }
        notify();
    }

    /** Take the most recent completed design, if any (message thread). */
    bool takeCompleted (juce::uint64& generationOut, decoder::DesignResult& out)
    {
        const juce::ScopedLock sl (lock);
        if (! hasCompleted)
            return false;
        generationOut = completedGeneration;
        out = std::move (completed);
        hasCompleted = false;
        return true;
    }

    /** True while a job is queued or a design is running. */
    bool isBusy() const noexcept { return busy.load (std::memory_order_acquire); }

    /** Block until no job is queued/running (headless test seam - no message
        loop needed). */
    void waitUntilIdle (int timeoutMs = 15000)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) juce::jmax (0, timeoutMs);
        while (isBusy() && juce::Time::getMillisecondCounter() < deadline)
            juce::Thread::sleep (1);
    }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            Job job;
            bool have = false;
            {
                const juce::ScopedLock sl (lock);
                if (hasPending)
                {
                    job = pending;
                    hasPending = false;
                    have = true;
                }
                else
                {
                    busy.store (false, std::memory_order_release);
                }
            }

            if (! have)
            {
                wait (-1);
                continue;
            }

            auto result = decoder::design (job.layout, job.options);   // the slow part, off-thread

            if (threadShouldExit())
                return;

            {
                const juce::ScopedLock sl (lock);
                completed = std::move (result);
                completedGeneration = job.generation;
                hasCompleted = true;
                busy.store (hasPending, std::memory_order_release);   // stay busy if a newer job queued
            }

            if (notifyReady)
                notifyReady();
        }
    }

    std::function<void()> notifyReady;

    juce::CriticalSection lock;
    Job pending;
    bool hasPending = false;
    decoder::DesignResult completed;
    juce::uint64 completedGeneration = 0;
    bool hasCompleted = false;
    std::atomic<bool> busy { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DecoderRebuildWorker)
};

} // namespace xoa
