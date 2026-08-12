/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    BinauralDesignWorker — background SOFA load + SH→ear design (WP15).

    Cloned from DecoderRebuildWorker: one persistent thread, one-slot
    latest-wins, generation-stamped so the consumer can discard a result a
    newer request has superseded. The work here is a libmysofa parse plus a
    1368-point projection at order 10 — tens of milliseconds to a second,
    which is exactly the kind of thing that must not run on the message
    thread while an operator drags a combo.

    onResultReady fires on the WORKER thread and is expected to hop to the
    message thread (juce::AsyncUpdater), like the decoder worker.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

#include "spatcore/binaural/SofaLoader.h"

#include "DSP/AmbiBinauralDecoder.h"

namespace xoa
{

class BinauralDesignWorker : private juce::Thread
{
public:
    struct Job
    {
        juce::File sofaFile;              // empty = nothing to load
        double sampleRate = 48000.0;
        binaural::BinauralDesignOptions options;
        juce::uint64 generation = 0;
    };

    struct Result
    {
        binaural::BinauralDesignResult design;
        juce::String status;              // human-readable, shown in the UI
        bool loadFailed = false;
        juce::uint64 generation = 0;
    };

    explicit BinauralDesignWorker (std::function<void()> onResultReady)
        : juce::Thread ("xoa binaural design"), notifyReady (std::move (onResultReady))
    {
        startThread (juce::Thread::Priority::normal);
    }

    ~BinauralDesignWorker() override { stop(); }

    /** Signal + join. Idempotent; call from the owner's destructor BEFORE
        tearing down anything onResultReady touches. */
    void stop()
    {
        signalThreadShouldExit();
        notify();
        stopThread (5000);
    }

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

    bool takeCompleted (Result& out)
    {
        const juce::ScopedLock sl (lock);
        if (! hasCompleted)
            return false;
        out = std::move (completed);
        hasCompleted = false;
        return true;
    }

    bool isBusy() const noexcept { return busy.load (std::memory_order_acquire); }

    /** Headless test seam — no message loop needed. */
    void waitUntilIdle (int timeoutMs = 30000)
    {
        const auto deadline = juce::Time::getMillisecondCounter()
                            + (juce::uint32) juce::jmax (0, timeoutMs);
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

            Result result;
            result.generation = job.generation;

            const auto loaded = spatcore::binaural::sofa::loadSofaFile (job.sofaFile,
                                                                        job.sampleRate);
            if (loaded.database == nullptr)
            {
                result.loadFailed = true;
                result.status = "HRTF load failed: " + loaded.status;
            }
            else
            {
                result.design = binaural::designShFilters (*loaded.database, job.options);
                if (! result.design.isValid())
                {
                    result.loadFailed = true;
                    result.status = "HRTF design failed: " + result.design.warning;
                }
                else
                {
                    result.status = loaded.status;
                    if (result.design.warning.isNotEmpty())
                        result.status += " — " + result.design.warning;
                }
            }

            if (threadShouldExit())
                return;

            {
                const juce::ScopedLock sl (lock);
                completed = std::move (result);
                hasCompleted = true;
                busy.store (hasPending, std::memory_order_release);
            }

            if (notifyReady)
                notifyReady();
        }
    }

    std::function<void()> notifyReady;

    juce::CriticalSection lock;
    Job pending;
    bool hasPending = false;
    Result completed;
    bool hasCompleted = false;
    std::atomic<bool> busy { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BinauralDesignWorker)
};

} // namespace xoa
