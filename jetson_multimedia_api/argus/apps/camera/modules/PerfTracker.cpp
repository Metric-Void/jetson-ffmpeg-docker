/*
 * Copyright (c) 2016-2017, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "PerfTracker.h"

#include <stdio.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include <limits>

#include "Dispatcher.h"
#include "EventThread.h"
#include "InitOnce.h"

namespace ArgusSamples
{

PerfTracker::PerfTracker()
    : m_sessionId(0)
{
}

PerfTracker::~PerfTracker()
{
}

bool PerfTracker::initialize()
{
    m_sessionId = 0;
    m_displayCount = 0;
    return true;
}

PerfTracker &PerfTracker::getInstance()
{
    static InitOnce initOnce;
    static PerfTracker instance;

    if (initOnce.begin())
    {
        if (instance.initialize())
        {
            initOnce.complete();
        }
        else
        {
            initOnce.failed();
            REPORT_ERROR("Initalization failed");
        }
    }

    return instance;
}

bool PerfTracker::onEvent(GlobalEvent event)
{
    switch (event)
    {
    case GLOBAL_EVENT_APP_START:
        m_appStartTime = getCurrentTime();
        break;
    case GLOBAL_EVENT_APP_INITIALIZED:
        m_appInitializedTime = getCurrentTime();
        break;
    case GLOBAL_EVENT_DISPLAY:
        if (!Dispatcher::getInstance().m_kpi)
            return true;

        if (m_firstDisplayTime == TimeValue())
        {
            // start measurement
            m_firstDisplayTime = getCurrentTime();
        }
        else
        {
            const TimeValue curTime = getCurrentTime();

            m_displayCount++;

            // print every second
            if (curTime - m_firstDisplayTime > TimeValue::fromSec(1.f))
            {
                const float frameRate = static_cast<float>(m_displayCount) *
                    (curTime - m_firstDisplayTime).toCyclesPerSec();
                printf("PerfTracker: display frame rate %.2f frames per second\n", frameRate);

                // restart measurement
                m_firstDisplayTime = getCurrentTime();
                m_displayCount = 0;
            }
        }
        break;
    default:
        ORIGINATE_ERROR("Unhandled global event");
        break;
    }

    return true;
}

SessionPerfTracker::SessionPerfTracker()
    : m_id(PerfTracker::getInstance().getNewSessionID())
    , m_session(NULL)
    , m_numberframesReceived(0)
    , m_lastFrameCount(0)
    , m_totalFrameDrop(0)
    , m_minLatency(std::numeric_limits<uint64_t>::max())
    , m_maxLatency(0)
    , m_sumLatency(0)
    , m_countLatency(0)
{
}

SessionPerfTracker::~SessionPerfTracker()
{
    PROPAGATE_ERROR_CONTINUE(shutdown());
}

bool SessionPerfTracker::shutdown()
{
    if (m_eventThread)
    {
        PROPAGATE_ERROR_CONTINUE(m_eventThread->shutdown());
        m_eventThread.reset();
    }

    return true;
}

bool SessionPerfTracker::setSession(Argus::CaptureSession *session)
{
    m_session = session;
    return true;
}

bool SessionPerfTracker::onEvent(SessionEvent event, uint64_t value)
{
    if (!Dispatcher::getInstance().m_kpi)
        return true;

    switch (event)
    {
    case SESSION_EVENT_TASK_START:
        m_taskStartTime = getCurrentTime();
        printf("PerfTracker: app initial %" PRIu64 " ms\n",
            (PerfTracker::getInstance().appInitializedTime() -
             PerfTracker::getInstance().appStartTime()).toMSec());
        printf("PerfTracker %d: app intialized to task start %" PRIu64 " ms\n", m_id,
            (m_taskStartTime - PerfTracker::getInstance().appInitializedTime()).toMSec());
        break;
    case SESSION_EVENT_ISSUE_CAPTURE:
        // captures are about to start, create the event thread which collects information on the
        // captures received
        if (!m_eventThread)
        {
            m_eventThread.reset(new EventThread(m_session, this));
            if (!m_eventThread)
                ORIGINATE_ERROR("Failed to allocate EventThread");

            PROPAGATE_ERROR(m_eventThread->initialize());
            PROPAGATE_ERROR(m_eventThread->waitRunning());
        }

        m_issueCaptureTime = getCurrentTime();
        printf("PerfTracker %d: task start to issue capture %" PRIu64 " ms\n", m_id,
            (m_issueCaptureTime - m_taskStartTime).toMSec());
        break;
    case SESSION_EVENT_REQUEST_RECEIVED:
        m_requestReceivedTime = getCurrentTime();
        if (m_numberframesReceived == 0)
        {
            m_firstRequestReceivedTime = m_requestReceivedTime;
            printf("PerfTracker %d: first request %" PRIu64 " ms\n", m_id,
                (m_firstRequestReceivedTime - m_issueCaptureTime).toMSec());
            printf("PerfTracker %d: total launch time %" PRIu64 " ms\n", m_id,
                (m_firstRequestReceivedTime - PerfTracker::getInstance().appStartTime()).toMSec());
        }

        m_numberframesReceived++;
        if ((m_numberframesReceived % 30) == 2)
        {
            const float frameRate =
                static_cast<float>(m_numberframesReceived - 1) *
                (m_requestReceivedTime - m_firstRequestReceivedTime).toCyclesPerSec();
            printf("PerfTracker %d: frameRate %.2f frames per second at %" PRIu64 " Seconds\n",
                   m_id,
                   frameRate,
                   (m_requestReceivedTime - m_firstRequestReceivedTime).toSec());
        }
        break;
    case SESSION_EVENT_REQUEST_LATENCY:
        // can do some stats
        m_minLatency = (m_minLatency < value) ? m_minLatency : value;
        m_maxLatency = (m_maxLatency < value) ? value : m_maxLatency;
        m_sumLatency += value;
        m_countLatency += 1;
        if ((m_numberframesReceived % 30) == 6)
        {
            const uint64_t latencyAverage = (m_sumLatency + m_countLatency/2) / m_countLatency;
            printf("PerfTracker %d: latency %" PRIu64 " ms average, min %" PRIu64
                   " max %" PRIu64 "\n",
                   m_id, latencyAverage,
                   m_minLatency,
                   m_maxLatency);

            m_countLatency = 0;
            m_sumLatency = 0;
            m_maxLatency = 0;
            m_minLatency = std::numeric_limits<uint64_t>::max();
        }
        break;
    case SESSION_EVENT_FRAME_COUNT:
        {
            const TimeValue currentTime = getCurrentTime();
            const uint64_t currentFrameCount = value;
            int64_t currentFrameDrop = 0;
            // start frame drop count from 2nd frame
            if (m_lastFrameCount > 0)
            {
                // currentFrameDrop can be negative when metadata comes out of order
                currentFrameDrop = currentFrameCount - m_lastFrameCount - 1;
                m_totalFrameDrop += currentFrameDrop;
            }
            if (currentFrameDrop != 0)
            {
                printf("PerfTracker %d: framedrop current request %" PRId64 ", total %" PRId64
                       ", Drop at %" PRIu64 " Seconds !\n",
                        m_id, currentFrameDrop,
                        m_totalFrameDrop,
                        (currentTime - m_firstRequestReceivedTime).toSec());
            }
            else
            {
                if ((m_numberframesReceived % 30) == 4)
                {
                    printf("PerfTracker %d: framedrop current request %" PRId64 ", total %"
                           PRId64 "\n",
                           m_id, currentFrameDrop,
                           m_totalFrameDrop);
                }
            }

            m_lastFrameCount = currentFrameCount;
            break;
        }
    case SESSION_EVENT_CLOSE_REQUESTED:
        m_closeRequestedTime = getCurrentTime();
        // captures are about to be stopped, destroy the event thread.
        if (m_eventThread)
        {
            PROPAGATE_ERROR(m_eventThread->shutdown());
            m_eventThread.reset();
        }
        break;
    case SESSION_EVENT_FLUSH_DONE:
        m_flushDoneTime = getCurrentTime();
        printf("PerfTracker %d: flush takes %" PRIu64 " ms\n", m_id,
            (m_flushDoneTime - m_closeRequestedTime).toMSec());
        break;
    case SESSION_EVENT_CLOSE_DONE:
        m_closeDoneTime = getCurrentTime();
        printf("PerfTracker %d: device close takes %" PRIu64 " ms\n", m_id,
            (m_closeDoneTime - m_flushDoneTime).toMSec());
        printf("PerfTracker %d: total close takes %" PRIu64 " ms\n", m_id,
            (m_closeDoneTime - m_closeRequestedTime).toMSec());
        break;
    default:
        ORIGINATE_ERROR("Unhandled session event");
        break;
    }

    return true;
}

}; // namespace ArgusSamples
