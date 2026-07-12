/*
  ==============================================================================

    XOA — tenth-order Ambisonics spatial audio processor.
    RvReAnalysisService — runs rV/rE analysis off the message thread (WP10 C8,
    D30). submit() (called on the message thread from the decoder-rebuild callback)
    hands the worker a copy of the decode matrix + layout; the worker analyzes the
    ~2664-point grid and publishes a shared_ptr result, then triggers an async
    update. Newest-wins: a submit during a run supersedes the in-flight job.

    This file is part of XOA, released under the GNU General Public License
    v3.0. See LICENSE for details.

  ==============================================================================
*/

#pragma once

#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

#include "RvReAnalysisCore.h"

namespace xoa::ui
{

class RvReAnalysisService : private juce::Thread,
                            private juce::AsyncUpdater
{
public:
    RvReAnalysisService() : juce::Thread ("xoa-rvre") { startThread(); }

    ~RvReAnalysisService() override
    {
        signalThreadShouldExit();
        wake.signal();
        stopThread (2000);
        cancelPendingUpdate();
    }

    /** Fired on the message thread when a new result is published. */
    std::function<void()> onResultReady;

    /** Queue an analysis (message thread). Supersedes any pending job. */
    void submit (decoder::DecoderMatrix matrix, const decoder::SpeakerLayout& layout)
    {
        {
            const std::lock_guard<std::mutex> lk (jobLock);
            pendingMatrix = std::move (matrix);
            pendingLayout = layout;
            pendingGen    = ++submitCounter;
            hasPending    = true;
        }
        wake.signal();
    }

    /** The most recently published result (message thread). May be null. */
    std::shared_ptr<const AnalysisResult> latest() const
    {
        const std::lock_guard<std::mutex> lk (resultLock);
        return published;
    }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            wake.wait (-1);

            for (;;)
            {
                if (threadShouldExit())
                    return;

                decoder::DecoderMatrix m;
                decoder::SpeakerLayout l;
                std::uint64_t gen = 0;
                bool got = false;
                {
                    const std::lock_guard<std::mutex> lk (jobLock);
                    if (hasPending)
                    {
                        m = std::move (pendingMatrix);
                        l = pendingLayout;
                        gen = pendingGen;
                        hasPending = false;
                        got = true;
                    }
                }
                if (! got)
                    break;

                auto res = std::make_shared<AnalysisResult> (runAnalysis (std::move (m), l, gen));
                {
                    const std::lock_guard<std::mutex> lk (resultLock);
                    published = res;
                }
                triggerAsyncUpdate();
            }
        }
    }

    void handleAsyncUpdate() override
    {
        if (onResultReady)
            onResultReady();
    }

    juce::WaitableEvent wake;

    mutable std::mutex jobLock;
    decoder::DecoderMatrix pendingMatrix;
    decoder::SpeakerLayout pendingLayout;
    std::uint64_t pendingGen = 0;
    std::uint64_t submitCounter = 0;
    bool hasPending = false;

    mutable std::mutex resultLock;
    std::shared_ptr<const AnalysisResult> published;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RvReAnalysisService)
};

} // namespace xoa::ui
