/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2010 NorthScale, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "config.h"
#include "ep_engine.h"
#include "dispatcher.hh"

#define STATWRITER_NAMESPACE tap
#include "statwriter.hh"
#undef STATWRITER_NAMESPACE

const uint8_t TapEngineSpecific::nru(1);
const short int TapEngineSpecific::sizeRevSeqno(8);
const short int TapEngineSpecific::sizeExtra(1);
const short int TapEngineSpecific::sizeTotal(9);

void TapEngineSpecific::readSpecificData(tap_event_t ev, void *engine_specific,
                                         uint16_t nengine, uint64_t *seqnum,
                                         uint8_t *extra)
{
    uint8_t ex;
    if (ev == TAP_CHECKPOINT_START || ev == TAP_CHECKPOINT_END || ev == TAP_DELETION ||
        ev == TAP_MUTATION)
    {
        assert(nengine >= sizeRevSeqno);
        memcpy(seqnum, engine_specific, sizeRevSeqno);
        *seqnum = ntohll(*seqnum);
        if (ev == TAP_MUTATION && nengine == sizeTotal) {
            uint8_t *dptr = (uint8_t *)engine_specific + sizeRevSeqno;
            memcpy(&ex, (void *)dptr, sizeExtra);
            *extra = ex;
        }
    }
}

uint16_t TapEngineSpecific::packSpecificData(tap_event_t ev, TapProducer *tp,
                                             uint64_t seqnum, bool referenced)
{
    uint64_t seqno;
    uint16_t nengine = 0;
    if (ev == TAP_MUTATION || ev == TAP_DELETION || ev == TAP_CHECKPOINT_START) {
        seqno = htonll(seqnum);
        memcpy(tp->specificData, (void *)&seqno, sizeRevSeqno);
        if (ev == TAP_MUTATION && referenced) {
            // transfer item nru reference bit in item extra byte
            uint8_t itemNru = TapEngineSpecific::nru;
            memcpy(&tp->specificData[sizeRevSeqno], (void*)&itemNru, sizeExtra);
            nengine = sizeTotal;
        } else {
            nengine = sizeRevSeqno;
        }
    }
    return nengine;
}

Atomic<uint64_t> TapConnection::tapCounter(1);


TapConnection::TapConnection(EventuallyPersistentEngine &theEngine,
              const void *c, const std::string &n) :
    engine(theEngine),
    cookie(c),
    name(n),
    created(ep_current_time()),
    connToken(gethrtime()),
    expiryTime((rel_time_t)-1),
    connected(true),
    disconnect(false),
    supportAck(false),
    supportCheckpointSync(false),
    reserved(false),
    stats(engine.getEpStats()) { }

TapConnection::~TapConnection() {
    getLogger()->log(EXTENSION_LOG_INFO, NULL,
                     "%s Remove tap connection instance.\n", logHeader());
}

template <typename T>
void TapConnection::addStat(const char *nm, T val, ADD_STAT add_stat, const void *c) {
    std::stringstream tap;
    tap << name << ":" << nm;
    std::stringstream value;
    value << val;
    std::string n = tap.str();
    add_casted_stat(n.data(), value.str().data(), add_stat, c);
}

const void *TapConnection::getCookie() const {
    return cookie;
}

void TapConnection::releaseReference(bool force)
{
    if (force || reserved) {
        engine.releaseCookie(cookie);
        setReserved(false);
    }
}

const char *TapConnection::logHeader() {
    return logString.c_str();
}

const char *TapConnection::opaqueCmdToString(uint32_t opaque_code) {
    switch(opaque_code) {
    case TAP_OPAQUE_ENABLE_AUTO_NACK:
        return "opaque_enable_auto_nack";
    case TAP_OPAQUE_INITIAL_VBUCKET_STREAM:
        return "initial_vbucket_stream";
    case TAP_OPAQUE_ENABLE_CHECKPOINT_SYNC:
        return "enable_checkpoint_sync";
    case TAP_OPAQUE_OPEN_CHECKPOINT:
        return "open_checkpoint";
    case TAP_OPAQUE_CLOSE_TAP_STREAM:
        return "close_tap_stream";
    case TAP_OPAQUE_CLOSE_BACKFILL:
        return "close_backfill";
    case TAP_OPAQUE_COMPLETE_VB_FILTER_CHANGE:
        return "complete_vb_filter_change";
    }
    return "unknown";
}

class TapConfigChangeListener : public ValueChangedListener {
public:
    TapConfigChangeListener(TapConfig &c) : config(c) {
        // EMPTY
    }

    virtual void sizeValueChanged(const std::string &key, size_t value) {
        if (key.compare("tap_ack_grace_period") == 0) {
            config.setAckGracePeriod(value);
        } else if (key.compare("tap_ack_initial_sequence_number") == 0) {
            config.setAckInitialSequenceNumber(value);
        } else if (key.compare("tap_ack_interval") == 0) {
            config.setAckInterval(value);
        } else if (key.compare("tap_ack_window_size") == 0) {
            config.setAckWindowSize(value);
        } else if (key.compare("tap_bg_max_pending") == 0) {
            config.setBgMaxPending(value);
        } else if (key.compare("tap_backlog_limit") == 0) {
            config.setBackfillBacklogLimit(value);
        }
    }

    virtual void floatValueChanged(const std::string &key, float value) {
        if (key.compare("tap_backoff_period") == 0) {
            config.setBackoffSleepTime(value);
        } else if (key.compare("tap_requeue_sleep_time") == 0) {
            config.setRequeueSleepTime(value);
        } else if (key.compare("tap_backfill_resident") == 0) {
            config.setBackfillResidentThreshold(value);
        }
    }

private:
    TapConfig &config;
};

TapConfig::TapConfig(EventuallyPersistentEngine &e)
    : engine(e)
{
    Configuration &config = engine.getConfiguration();
    ackWindowSize = config.getTapAckWindowSize();
    ackInterval = config.getTapAckInterval();
    ackGracePeriod = config.getTapAckGracePeriod();
    ackInitialSequenceNumber = config.getTapAckInitialSequenceNumber();
    bgMaxPending = config.getTapBgMaxPending();
    backoffSleepTime = config.getTapBackoffPeriod();
    requeueSleepTime = config.getTapRequeueSleepTime();
    backfillBacklogLimit = config.getTapBacklogLimit();
    backfillResidentThreshold = config.getTapBackfillResident();
}

void TapConfig::addConfigChangeListener(EventuallyPersistentEngine &engine) {
    Configuration &configuration = engine.getConfiguration();
    configuration.addValueChangedListener("tap_ack_grace_period",
                              new TapConfigChangeListener(engine.getTapConfig()));
    configuration.addValueChangedListener("tap_ack_initial_sequence_number",
                              new TapConfigChangeListener(engine.getTapConfig()));
    configuration.addValueChangedListener("tap_ack_interval",
                              new TapConfigChangeListener(engine.getTapConfig()));
    configuration.addValueChangedListener("tap_ack_window_size",
                              new TapConfigChangeListener(engine.getTapConfig()));
    configuration.addValueChangedListener("tap_bg_max_pending",
                              new TapConfigChangeListener(engine.getTapConfig()));
    configuration.addValueChangedListener("tap_backoff_period",
                              new TapConfigChangeListener(engine.getTapConfig()));
    configuration.addValueChangedListener("tap_requeue_sleep_time",
                              new TapConfigChangeListener(engine.getTapConfig()));
    configuration.addValueChangedListener("tap_backlog_limit",
                              new TapConfigChangeListener(engine.getTapConfig()));
    configuration.addValueChangedListener("tap_backfill_resident",
                              new TapConfigChangeListener(engine.getTapConfig()));
}

TapProducer::TapProducer(EventuallyPersistentEngine &theEngine,
                         const void *c,
                         const std::string &n,
                         uint32_t f):
    TapConnection(theEngine, c, n),
    queue(NULL),
    queueSize(0),
    flags(f),
    recordsFetched(0),
    pendingFlush(false),
    reconnects(0),
    paused(false),
    backfillAge(0),
    dumpQueue(false),
    doTakeOver(false),
    takeOverCompletionPhase(false),
    doRunBackfill(false),
    backfillCompleted(true),
    pendingBackfillCounter(0),
    diskBackfillCounter(0),
    totalBackfillBacklogs(0),
    vbucketFilter(),
    queueMemSize(0),
    queueFill(0),
    queueDrain(0),
    seqno(theEngine.getTapConfig().getAckInitialSequenceNumber()),
    seqnoReceived(theEngine.getTapConfig().getAckInitialSequenceNumber() - 1),
    seqnoAckRequested(theEngine.getTapConfig().getAckInitialSequenceNumber() - 1),
    notifySent(false),
    suspended(false),
    registeredTAPClient(false),
    lastMsgTime(ep_current_time()),
    isLastAckSucceed(false),
    isSeqNumRotated(false),
    numNoops(0),
    tapFlagByteorderSupport(false),
    specificData(NULL),
    backfillTimestamp(0)
{
    evaluateFlags();
    queue = new std::list<queued_item>;
    specificData = new uint8_t[TapEngineSpecific::sizeTotal];

    if (supportAck) {
        expiryTime = ep_current_time() + engine.getTapConfig().getAckGracePeriod();
    }

    if (cookie != NULL) {
        setReserved(true);
    }

    setLogHeader("TAP (Producer) " + getName() + " -");
}

void TapProducer::evaluateFlags()
{
    std::stringstream ss;

    if (flags & TAP_CONNECT_FLAG_DUMP) {
        dumpQueue = true;
        ss << ",dump";
    }

    if (flags & TAP_CONNECT_SUPPORT_ACK) {
        TapVBucketEvent hi(TAP_OPAQUE, 0, (vbucket_state_t)htonl(TAP_OPAQUE_ENABLE_AUTO_NACK));
        addVBucketHighPriority(hi);
        supportAck = true;
        ss << ",ack";
    }

    if (flags & TAP_CONNECT_FLAG_BACKFILL) {
        ss << ",backfill";
    }

    if (flags & TAP_CONNECT_FLAG_LIST_VBUCKETS) {
        ss << ",vblist";
    }

    if (flags & TAP_CONNECT_FLAG_TAKEOVER_VBUCKETS) {
        ss << ",takeover";
    }

    if (flags & TAP_CONNECT_CHECKPOINT) {
        TapVBucketEvent event(TAP_OPAQUE, 0,
                              (vbucket_state_t)htonl(TAP_OPAQUE_ENABLE_CHECKPOINT_SYNC));
        addVBucketHighPriority(event);
        supportCheckpointSync = true;
        ss << ",checkpoints";
    }

    if (ss.str().length() > 0) {
        std::stringstream m;
        m.setf(std::ios::hex);
        m << flags << " (" << ss.str().substr(1) << ")";
        flagsText.assign(m.str());

        getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                         "%s TAP connection option flags %s\n",
                         logHeader(), m.str().c_str());
    }
}

void TapProducer::setBackfillAge(uint64_t age, bool reconnect) {
    if (reconnect) {
        if (!(flags & TAP_CONNECT_FLAG_BACKFILL)) {
            age = backfillAge;
        }

        if (age == backfillAge) {
            // we didn't change the critera...
            return;
        }
    }

    if (flags & TAP_CONNECT_FLAG_BACKFILL) {
        backfillAge = age;
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                         "%s Backfill age set to %llu\n",
                         logHeader(), age);
    }
}

void TapProducer::setVBucketFilter(const std::vector<uint16_t> &vbuckets,
                                   bool notifyCompletion)
{
    LockHolder lh(queueLock);
    VBucketFilter diff;

    // time to join the filters..
    if (flags & TAP_CONNECT_FLAG_LIST_VBUCKETS) {
        VBucketFilter filter(vbuckets);
        diff = vbucketFilter.filter_diff(filter);

        const std::set<uint16_t> &vset = diff.getVBSet();
        const VBucketMap &vbMap = engine.getEpStore()->getVBuckets();
        // Remove TAP cursors from the vbuckets that don't belong to the new vbucket filter.
        for (std::set<uint16_t>::const_iterator it = vset.begin(); it != vset.end(); ++it) {
            if (vbucketFilter(*it)) {
                RCPtr<VBucket> vb = vbMap.getBucket(*it);
                if (vb) {
                    vb->checkpointManager.removeTAPCursor(name);
                }
                backfillVBuckets.erase(*it);
                backFillVBucketFilter.removeVBucket(*it);
            }
        }

        std::stringstream ss;
        ss << logHeader() << ": Changing the vbucket filter from "
           << vbucketFilter << " to "
           << filter << " (diff: " << diff << ")" << std::endl;
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL, "%s\n",
                         ss.str().c_str());
        vbucketFilter = filter;

        std::stringstream f;
        f << vbucketFilter;
        filterText.assign(f.str());
    }

    // Note that we do re-evaluete all entries when we suck them out of the
    // queue to send them..
    if (flags & TAP_CONNECT_FLAG_TAKEOVER_VBUCKETS) {
        std::list<TapVBucketEvent> nonVBucketOpaqueMessages;
        std::list<TapVBucketEvent> vBucketOpaqueMessages;
        // Clear vbucket state change messages with a higher priority.
        while (!vBucketHighPriority.empty()) {
            TapVBucketEvent msg = vBucketHighPriority.front();
            vBucketHighPriority.pop();
            if (msg.event == TAP_OPAQUE) {
                uint32_t opaqueCode = (uint32_t) msg.state;
                if (opaqueCode == htonl(TAP_OPAQUE_ENABLE_AUTO_NACK) ||
                    opaqueCode == htonl(TAP_OPAQUE_ENABLE_CHECKPOINT_SYNC)) {
                    nonVBucketOpaqueMessages.push_back(msg);
                } else {
                    vBucketOpaqueMessages.push_back(msg);
                }
            }
        }

        // Add non-vbucket opaque messages back to the high priority queue.
        std::list<TapVBucketEvent>::iterator iter = nonVBucketOpaqueMessages.begin();
        while (iter != nonVBucketOpaqueMessages.end()) {
            addVBucketHighPriority_UNLOCKED(*iter);
            ++iter;
        }

        // Clear vbucket state changes messages with a lower priority.
        while (!vBucketLowPriority.empty()) {
            vBucketLowPriority.pop();
        }

        // Add new vbucket state change messages with a higher or lower priority.
        const std::set<uint16_t> &vset = vbucketFilter.getVBSet();
        for (std::set<uint16_t>::const_iterator it = vset.begin();
             it != vset.end(); ++it) {
            TapVBucketEvent hi(TAP_VBUCKET_SET, *it, vbucket_state_pending);
            TapVBucketEvent lo(TAP_VBUCKET_SET, *it, vbucket_state_active);
            addVBucketHighPriority_UNLOCKED(hi);
            addVBucketLowPriority_UNLOCKED(lo);
        }

        // Add vbucket opaque messages back to the high priority queue.
        iter = vBucketOpaqueMessages.begin();
        while (iter != vBucketOpaqueMessages.end()) {
            addVBucketHighPriority_UNLOCKED(*iter);
            ++iter;
        }
        doTakeOver = true;
    }

    if (notifyCompletion) {
        TapVBucketEvent notification(TAP_OPAQUE, 0,
            (vbucket_state_t)htonl(TAP_OPAQUE_COMPLETE_VB_FILTER_CHANGE));
        addVBucketHighPriority_UNLOCKED(notification);
    }
}

void TapProducer::registerTAPCursor(const std::map<uint16_t, uint64_t> &lastCheckpointIds) {
    LockHolder lh(queueLock);

    uint64_t current_time = (uint64_t)ep_real_time();
    std::vector<uint16_t> backfill_vbuckets;
    const VBucketMap &vbuckets = engine.getEpStore()->getVBuckets();
    size_t numOfVBuckets = vbuckets.getSize();
    for (size_t i = 0; i < numOfVBuckets; ++i) {
        assert(i <= std::numeric_limits<uint16_t>::max());
        uint16_t vbid = static_cast<uint16_t>(i);
        if (vbucketFilter(vbid)) {
            RCPtr<VBucket> vb = vbuckets.getBucket(vbid);
            if (!vb) {
                tapCheckpointState.erase(vbid);
                getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                 "%s VBucket %d not found for TAP cursor. Skip it...\n",
                                 logHeader(), vbid);
                continue;
            }

            uint64_t chk_id_to_start = 0;
            std::map<uint16_t, uint64_t>::const_iterator it = lastCheckpointIds.find(vbid);
            if (it != lastCheckpointIds.end()) {
                // Now, we assume that the checkpoint Id for a given vbucket is monotonically
                // increased.
                chk_id_to_start = it->second + 1;
            } else {
                // If a TAP client doesn't specify the last closed checkpoint Id for a given vbucket,
                // check if the checkpoint manager currently has the cursor for that TAP client.
                uint64_t cid = vb->checkpointManager.getCheckpointIdForTAPCursor(name);
                chk_id_to_start = cid > 0 ? cid : 1;
            }

            std::map<uint16_t, TapCheckpointState>::iterator cit = tapCheckpointState.find(vbid);
            if (cit != tapCheckpointState.end()) {
                cit->second.currentCheckpointId = chk_id_to_start;
            } else {
                TapCheckpointState st(vbid, chk_id_to_start, checkpoint_start);
                tapCheckpointState[vbid] = st;
            }

            // If backfill is currently running for this vbucket, skip the cursor registration.
            if (backfillVBuckets.find(vbid) != backfillVBuckets.end()) {
                cit = tapCheckpointState.find(vbid);
                assert(cit != tapCheckpointState.end());
                cit->second.currentCheckpointId = 0;
                cit->second.state = backfill;
                continue;
            }

            // As TAP dump option simply requires the snapshot of each vbucket, simply schedule
            // backfill and skip the checkpoint cursor registration.
            if (dumpQueue) {
                if (vb->getState() == vbucket_state_active && vb->ht.getNumItems() > 0) {
                    backfill_vbuckets.push_back(vbid);
                }
                continue;
            }

            // Check if this TAP producer completed the replication before shutdown or crash.
            bool prev_session_completed =
                engine.getTapConnMap().prevSessionReplicaCompleted(name);
            // Check if the unified queue contains the checkpoint to start with.
            bool chk_exists = vb->checkpointManager.registerTAPCursor(name,
                                                                      chk_id_to_start,
                                                                      closedCheckpointOnly,
                                                                      registeredTAPClient);
            if(!prev_session_completed || !chk_exists) {
                uint64_t chk_id;
                tap_checkpoint_state cstate;

                if (backfillAge < current_time) {
                    chk_id = 0;
                    cstate = backfill;
                    if (vb->checkpointManager.getOpenCheckpointId() > 0) {
                        // If the current open checkpoint is 0, it means that this vbucket is still
                        // receiving backfill items from another node. Once the backfill is done,
                        // we will schedule the backfill for this tap connection separately.
                        backfill_vbuckets.push_back(vbid);
                    }
                } else { // Backfill age is in the future, simply start from the first checkpoint.
                    chk_id = vb->checkpointManager.getCheckpointIdForTAPCursor(name);
                    cstate = checkpoint_start;
                    getLogger()->log(EXTENSION_LOG_INFO, NULL,
                                     "%s Backfill age is greater than current time."
                                     " Full backfill is not required for vbucket %d\n",
                                     logHeader(), vbid);
                }

                cit = tapCheckpointState.find(vbid);
                assert(cit != tapCheckpointState.end());
                cit->second.currentCheckpointId = chk_id;
                cit->second.state = cstate;
            } else {
                getLogger()->log(EXTENSION_LOG_INFO, NULL,
                                 "%s The checkpoint to start with is still in memory. "
                                 "Full backfill is not required for vbucket %d\n",
                                 logHeader(), vbid);
            }
        } else { // The vbucket doesn't belong to this tap connection anymore.
            tapCheckpointState.erase(vbid);
        }
    }

    if (backfill_vbuckets.size() > 0) {
        if (backfillAge < current_time) {
            scheduleBackfill_UNLOCKED(backfill_vbuckets);
        }
    }
}

bool TapProducer::windowIsFull() {
    if (!supportAck) {
        return false;
    }

    const TapConfig &config = engine.getTapConfig();
    uint32_t limit = config.getAckWindowSize() * config.getAckInterval();
    if (seqno >= seqnoReceived) {

        if ((seqno - seqnoReceived) <= limit) {
            return false;
        }
    } else {
        uint32_t n = static_cast<uint32_t>(-1) - seqnoReceived + seqno;
        if (n <= limit) {
            return false;
        }
    }

    return true;
}

bool TapProducer::requestAck(tap_event_t event, uint16_t vbucket) {
    LockHolder lh(queueLock);

    if (!supportAck) {
        // If backfill was scheduled before, check if the backfill is completed or not.
        checkBackfillCompletion_UNLOCKED();
        return false;
    }

    bool explicitEvent = false;
    if (supportCheckpointSync && (event == TAP_MUTATION || event == TAP_DELETION)) {
        std::map<uint16_t, TapCheckpointState>::iterator map_it =
            tapCheckpointState.find(vbucket);
        if (map_it != tapCheckpointState.end()) {
            map_it->second.lastSeqNum = seqno;
            if (map_it->second.lastItem || map_it->second.state == checkpoint_end) {
                // Always ack for the last item or any items that were NAcked after the cursor
                // reaches to the checkpoint end.
                explicitEvent = true;
            }
        }
    }

    ++seqno;
    if (seqno == 0) {
        isSeqNumRotated = true;
        seqno = 1;
    }

    if (event == TAP_VBUCKET_SET ||
        event == TAP_OPAQUE ||
        event == TAP_CHECKPOINT_START ||
        event == TAP_CHECKPOINT_END) {
        explicitEvent = true;
    }

    const TapConfig &config = engine.getTapConfig();
    uint32_t ackInterval = config.getAckInterval();

    return explicitEvent ||
           (seqno - 1) % ackInterval == 0 || // ack at a regular interval
           (!backfillCompleted && getBackfillQueueSize_UNLOCKED() == 0) ||
           emptyQueue_UNLOCKED(); // but if we're almost up to date, ack more often
}

void TapProducer::clearQueues_UNLOCKED() {
    size_t mem_overhead = 0;
    // Clear fg-fetched items.
    queue->clear();
    mem_overhead += (queueSize * sizeof(queued_item));
    queueSize = 0;
    queueMemSize = 0;

    // Clear bg-fetched items.
    while (!backfilledItems.empty()) {
        Item *i(backfilledItems.front());
        assert(i);
        delete i;
        backfilledItems.pop();
    }
    mem_overhead += (bgResultSize * sizeof(Item *));
    bgResultSize = 0;

    // Reset bg result size in a checkpoint state.
    std::map<uint16_t, TapCheckpointState>::iterator it = tapCheckpointState.begin();
    for (; it != tapCheckpointState.end(); ++it) {
        it->second.bgResultSize = 0;
    }

    // Clear the checkpoint message queue as well
    while (!checkpointMsgs.empty()) {
        checkpointMsgs.pop();
    }
    // Clear the vbucket state message queues
    while (!vBucketHighPriority.empty()) {
        vBucketHighPriority.pop();
    }
    while (!vBucketLowPriority.empty()) {
        vBucketLowPriority.pop();
    }

    // Clear the tap logs
    mem_overhead += (tapLog.size() * sizeof(TapLogElement));
    tapLog.clear();

    stats.memOverhead.decr(mem_overhead);
    assert(stats.memOverhead.get() < GIGANTOR);

    getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                     "%s Clear the tap queues by force\n",
                     logHeader());
}

void TapProducer::rollback() {
    LockHolder lh(queueLock);
    if (registeredTAPClient && closedCheckpointOnly && backfillCompleted) {
        // If the connection is for a registered TAP client that is only interested in closed
        // checkpoints, we don't need to resend unACKed items to the client because its replication
        // cursor is reset to the beginning of the checkpoint to which the cursor currently belongs.
        clearQueues_UNLOCKED();
        seqno = engine.getTapConfig().getAckInitialSequenceNumber();
        seqnoReceived = seqno -1;
        seqnoAckRequested = seqno - 1;
        checkpointMsgCounter = 0;

        return;
    }

    getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                     "%s Connection is re-established. Rollback unacked messages...",
                     logHeader());

    size_t checkpoint_msg_sent = 0;
    size_t tapLogSize = 0;
    size_t opaque_msg_sent = 0;
    std::list<TapLogElement>::iterator i = tapLog.begin();
    while (i != tapLog.end()) {
        switch (i->event) {
        case TAP_VBUCKET_SET:
            {
                TapVBucketEvent e(i->event, i->vbucket, i->state);
                if (i->state == vbucket_state_pending) {
                    addVBucketHighPriority_UNLOCKED(e);
                } else {
                    addVBucketLowPriority_UNLOCKED(e);
                }
            }
            break;
        case TAP_CHECKPOINT_START:
        case TAP_CHECKPOINT_END:
            ++checkpoint_msg_sent;
            addCheckpointMessage_UNLOCKED(i->item);
            break;
        case TAP_FLUSH:
            addEvent_UNLOCKED(i->item);
            break;
        case TAP_DELETION:
        case TAP_MUTATION:
            {
                if (supportCheckpointSync) {
                    std::map<uint16_t, TapCheckpointState>::iterator map_it =
                        tapCheckpointState.find(i->vbucket);
                    if (map_it != tapCheckpointState.end()) {
                        map_it->second.lastSeqNum = std::numeric_limits<uint32_t>::max();
                    } else {
                        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                            "%s Checkpoint State for VBucket %d Not Found",
                            logHeader(), i->vbucket);
                    }
                }
                addEvent_UNLOCKED(i->item);
            }
            break;
        case TAP_OPAQUE:
            {
                uint32_t val = ntohl((uint32_t)i->state);
                switch (val) {
                case TAP_OPAQUE_ENABLE_AUTO_NACK:
                case TAP_OPAQUE_ENABLE_CHECKPOINT_SYNC:
                case TAP_OPAQUE_INITIAL_VBUCKET_STREAM:
                case TAP_OPAQUE_CLOSE_BACKFILL:
                case TAP_OPAQUE_OPEN_CHECKPOINT:
                case TAP_OPAQUE_COMPLETE_VB_FILTER_CHANGE:
                    {
                        ++opaque_msg_sent;
                        TapVBucketEvent e(i->event, i->vbucket, i->state);
                        addVBucketHighPriority_UNLOCKED(e);
                    }
                    break;
                default:
                    getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                     "%s Internal error in rollback()."
                                     " Tap opaque value %d not implemented",
                                     logHeader(), val);
                    abort();
                }
            }
            break;
        default:
            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                             "%s Internal error in rollback()."
                             " Tap opcode value %d not implemented",
                             logHeader(), i->event);
            abort();
        }
        tapLog.erase(i);
        i = tapLog.begin();
        ++tapLogSize;
    }

    stats.memOverhead.decr(tapLogSize * sizeof(TapLogElement));
    assert(stats.memOverhead.get() < GIGANTOR);

    seqnoReceived = seqno - 1;
    seqnoAckRequested = seqno - 1;
    checkpointMsgCounter -= checkpoint_msg_sent;
    opaqueMsgCounter -= opaque_msg_sent;
}

/**
 * Dispatcher task to wake a tap connection.
 */
class TapResumeCallback : public DispatcherCallback {
public:
    TapResumeCallback(EventuallyPersistentEngine &e, TapProducer &c)
        : engine(e), connection(c) {
        std::stringstream ss;
        ss << "Resuming suspended tap connection: " << connection.getName();
        descr = ss.str();
    }

    bool callback(Dispatcher &, TaskId &) {
        if (engine.isShutdownMode()) {
            return false;
        }
        connection.setSuspended(false);
        // The notify io thread will pick up this connection and resume it
        // Since we was suspended I guess we can wait a little bit
        // longer ;)
        return false;
    }

    std::string description() {
        return descr;
    }

private:
    EventuallyPersistentEngine &engine;
    TapProducer &connection;
    std::string descr;
};

bool TapProducer::isSuspended() const {
    return suspended;
}

void TapProducer::setSuspended_UNLOCKED(bool value)
{
    if (value) {
        const TapConfig &config = engine.getTapConfig();
        if (config.getBackoffSleepTime() > 0 && !suspended) {
            Dispatcher *d = engine.getEpStore()->getNonIODispatcher();
            d->schedule(shared_ptr<DispatcherCallback>
                        (new TapResumeCallback(engine, *this)),
                        NULL, Priority::TapResumePriority, config.getBackoffSleepTime(),
                        false);
            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                             "%s Suspend for %.2f secs\n", logHeader(),
                             config.getBackoffSleepTime());
        } else {
            // backoff disabled, or already in a suspended state
            return;
        }
    } else {
        getLogger()->log(EXTENSION_LOG_INFO, NULL,
                         "%s Unlocked from the suspended state\n", logHeader());
    }
    suspended = value;
}

void TapProducer::setSuspended(bool value) {
    LockHolder lh(queueLock);
    setSuspended_UNLOCKED(value);
}

void TapProducer::reschedule_UNLOCKED(const std::list<TapLogElement>::iterator &iter)
{
    switch (iter->event) {
    case TAP_VBUCKET_SET:
        {
            TapVBucketEvent e(iter->event, iter->vbucket, iter->state);
            if (iter->state == vbucket_state_pending) {
                addVBucketHighPriority_UNLOCKED(e);
            } else {
                addVBucketLowPriority_UNLOCKED(e);
            }
        }
        break;
    case TAP_CHECKPOINT_START:
    case TAP_CHECKPOINT_END:
        --checkpointMsgCounter;
        addCheckpointMessage_UNLOCKED(iter->item);
        break;
    case TAP_FLUSH:
        addEvent_UNLOCKED(iter->item);
        break;
    case TAP_DELETION:
    case TAP_MUTATION:
        {
            if (supportCheckpointSync) {
                std::map<uint16_t, TapCheckpointState>::iterator map_it =
                    tapCheckpointState.find(iter->vbucket);
                if (map_it != tapCheckpointState.end()) {
                    map_it->second.lastSeqNum = std::numeric_limits<uint32_t>::max();
                }
            }
            addEvent_UNLOCKED(iter->item);
            if (!isBackfillCompleted_UNLOCKED()) {
                ++totalBackfillBacklogs;
            }
        }
        break;
    case TAP_OPAQUE:
        {
            --opaqueMsgCounter;
            TapVBucketEvent ev(iter->event, iter->vbucket,
                                         (vbucket_state_t)iter->state);
            addVBucketHighPriority_UNLOCKED(ev);
        }
        break;
    default:
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "%s Internal error in reschedule_UNLOCKED()."
                         " Tap opcode value %d not implemented",
                         logHeader(), iter->event);
        abort();
    }
}

ENGINE_ERROR_CODE TapProducer::processAck(uint32_t s,
                                          uint16_t status,
                                          const std::string &msg)
{
    LockHolder lh(queueLock);
    std::list<TapLogElement>::iterator iter = tapLog.begin();
    ENGINE_ERROR_CODE ret = ENGINE_SUCCESS;

    const TapConfig &config = engine.getTapConfig();
    rel_time_t ackGracePeriod = config.getAckGracePeriod();

    expiryTime = ep_current_time() + ackGracePeriod;
    if (isSeqNumRotated && s < seqnoReceived) {
        // if the ack seq number is rotated, reset the last seq number of each vbucket to 0.
        std::map<uint16_t, TapCheckpointState>::iterator it = tapCheckpointState.begin();
        for (; it != tapCheckpointState.end(); ++it) {
            it->second.lastSeqNum = 0;
        }
        isSeqNumRotated = false;
    }
    seqnoReceived = s;
    isLastAckSucceed = false;

    size_t num_logs = 0;
    /* Implicit ack _every_ message up until this message */
    while (iter != tapLog.end() && iter->seqno != s) {
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                         "%s Implicit ack (#%u)\n",
                         logHeader(), iter->seqno);
        ++iter;
        ++num_logs;
    }

    bool notifyTapNotificationThread = false;

    switch (status) {
    case PROTOCOL_BINARY_RESPONSE_SUCCESS:
        /* And explicit ack this message! */
        if (iter != tapLog.end()) {
            // If this ACK is for TAP_CHECKPOINT messages, indicate that the checkpoint
            // is synced between the master and slave nodes.
            if ((iter->event == TAP_CHECKPOINT_START || iter->event == TAP_CHECKPOINT_END)
                && supportCheckpointSync) {
                std::map<uint16_t, TapCheckpointState>::iterator map_it =
                    tapCheckpointState.find(iter->vbucket);
                if (iter->event == TAP_CHECKPOINT_END && map_it != tapCheckpointState.end()) {
                    map_it->second.state = checkpoint_end_synced;
                }
                --checkpointMsgCounter;
                notifyTapNotificationThread = true;
            } else if (iter->event == TAP_OPAQUE) {
                --opaqueMsgCounter;
                notifyTapNotificationThread = true;
            }
            getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                             "%s Explicit ack (#%u)\n",
                             logHeader(), iter->seqno);
            ++num_logs;
            ++iter;
            tapLog.erase(tapLog.begin(), iter);
            isLastAckSucceed = true;
        } else {
            num_logs = 0;
            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                             "%s Explicit ack of nonexisting entry (#%u)\n",
                             logHeader(), s);
        }

        if (checkBackfillCompletion_UNLOCKED() || (doTakeOver && tapLog.empty())) {
            notifyTapNotificationThread = true;
        }

        lh.unlock(); // Release the lock to avoid the deadlock with the notify thread

        if (notifyTapNotificationThread) {
            engine.notifyNotificationThread();
        }

        lh.lock();
        if (mayCompleteDumpOrTakeover_UNLOCKED() && idle_UNLOCKED()) {
            // We've got all of the ack's need, now we can shut down the
            // stream
            std::stringstream ss;
            if (dumpQueue) {
                ss << "TAP dump is completed. ";
            } else if (doTakeOver) {
                ss << "TAP takeover is completed. ";
            }
            ss << "Disconnecting tap stream <" << getName() << ">";
            getLogger()->log(EXTENSION_LOG_WARNING, NULL, "%s\n",
                             ss.str().c_str());

            setDisconnect(true);
            expiryTime = 0;
            ret = ENGINE_DISCONNECT;
        }
        break;

    case PROTOCOL_BINARY_RESPONSE_EBUSY:
    case PROTOCOL_BINARY_RESPONSE_ETMPFAIL:
        if (!takeOverCompletionPhase) {
            setSuspended_UNLOCKED(true);
        }
        ++numTapNack;
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                         "%s Received temporary TAP nack (#%u): Code: %u (%s)\n",
                         logHeader(), seqnoReceived, status, msg.c_str());

        // Reschedule _this_ sequence number..
        if (iter != tapLog.end()) {
            reschedule_UNLOCKED(iter);
            ++num_logs;
            ++iter;
        }
        tapLog.erase(tapLog.begin(), iter);
        break;
    default:
        tapLog.erase(tapLog.begin(), iter);
        ++numTapNack;
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "%s Received negative TAP ack (#%u): Code: %u (%s)\n",
                         logHeader(), seqnoReceived, status, msg.c_str());
        setDisconnect(true);
        expiryTime = 0;
        ret = ENGINE_DISCONNECT;
    }

    stats.memOverhead.decr(num_logs * sizeof(TapLogElement));
    assert(stats.memOverhead.get() < GIGANTOR);

    return ret;
}

bool TapProducer::checkBackfillCompletion_UNLOCKED() {
    bool rv = false;
    if (!backfillCompleted && !isPendingBackfill_UNLOCKED() &&
        getBackfillQueueSize_UNLOCKED() == 0 && tapLog.empty()) {

        backfillCompleted = true;
        std::stringstream ss;
        ss << "Backfill is completed with VBuckets ";
        std::set<uint16_t>::iterator it = backfillVBuckets.begin();
        for (; it != backfillVBuckets.end(); ++it) {
            ss << *it << ", ";
            TapVBucketEvent backfillEnd(TAP_OPAQUE, *it,
                                        (vbucket_state_t)htonl(TAP_OPAQUE_CLOSE_BACKFILL));
            addVBucketHighPriority_UNLOCKED(backfillEnd);
        }
        backfillVBuckets.clear();
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "%s %s\n",
                         logHeader(), ss.str().c_str());

        rv = true;
    }
    return rv;
}

void TapProducer::encodeVBucketStateTransition(const TapVBucketEvent &ev, void **es,
                                                 uint16_t *nes, uint16_t *vbucket) const
{
    *vbucket = ev.vbucket;
    switch (ev.state) {
    case vbucket_state_active:
        *es = const_cast<void*>(static_cast<const void*>(&VBucket::ACTIVE));
        break;
    case vbucket_state_replica:
        *es = const_cast<void*>(static_cast<const void*>(&VBucket::REPLICA));
        break;
    case vbucket_state_pending:
        *es = const_cast<void*>(static_cast<const void*>(&VBucket::PENDING));
        break;
    case vbucket_state_dead:
        *es = const_cast<void*>(static_cast<const void*>(&VBucket::DEAD));
        break;
    default:
        // Illegal vbucket state
        abort();
    }
    *nes = sizeof(vbucket_state_t);
}

bool TapProducer::waitForCheckpointMsgAck() {
    return supportAck && checkpointMsgCounter > 0;
}

bool TapProducer::waitForOpaqueMsgAck() {
    return supportAck && opaqueMsgCounter > 0;
}

/**
 * A Dispatcher job that performs a background fetch on behalf of tap.
 */
class TapBGFetchCallback : public DispatcherCallback {
public:
    TapBGFetchCallback(EventuallyPersistentEngine *e, const std::string &n,
                       const std::string &k, uint16_t vbid,
                       uint64_t r, hrtime_t token) :
        name(n), key(k), epe(e), init(gethrtime()),
        connToken(token), rowid(r), vbucket(vbid)
    {
        assert(epe);
    }

    bool callback(Dispatcher & d, TaskId &t) {
        hrtime_t start = gethrtime();
        RememberingCallback<GetValue> gcb;

        EPStats &stats = epe->getEpStats();
        EventuallyPersistentStore *epstore = epe->getEpStore();
        assert(epstore);

        epstore->getAuxUnderlying()->get(key, rowid, vbucket, gcb);
        gcb.waitForValue();
        assert(gcb.fired);

        // If a tap bg fetch job is failed, schedule it again.
        if (gcb.val.getStatus() != ENGINE_SUCCESS) {
            RCPtr<VBucket> vb = epstore->getVBucket(vbucket);
            if (vb) {
                int bucket_num(0);
                LockHolder lh = vb->ht.getLockedBucket(key, &bucket_num);
                StoredValue *v = epstore->fetchValidValue(vb, key, bucket_num);
                if (v) {
                    rowid = v->getId();
                    const TapConfig &config = epe->getTapConfig();
                    d.snooze(t, config.getRequeueSleepTime());
                    ++stats.numTapBGFetchRequeued;
                    return true;
                } else {
                    CompletedBGFetchTapOperation tapop(connToken, vbucket);
                    epe->getTapConnMap().performTapOp(name, tapop, gcb.val.getValue());
                    // As an item is deleted from hash table, push the item
                    // deletion event into the TAP queue.
                    queued_item qitem(new QueuedItem(key, vbucket, queue_op_del));
                    std::list<queued_item> del_items;
                    del_items.push_back(qitem);
                    epe->getTapConnMap().setEvents(name, &del_items);
                    return false;
                }
            } else {
                CompletedBGFetchTapOperation tapop(connToken, vbucket);
                epe->getTapConnMap().performTapOp(name, tapop, gcb.val.getValue());
                getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                 "VBucket %d not exist!!! TAP BG fetch failed for TAP %s\n",
                                 vbucket, name.c_str());
                return false;
            }
        }

        CompletedBGFetchTapOperation tapop(connToken, vbucket);
        if (!epe->getTapConnMap().performTapOp(name, tapop, gcb.val.getValue())) {
            delete gcb.val.getValue(); // Tap connection is closed. Free an item instance.
        }

        hrtime_t stop = gethrtime();

        if (stop > start && start > init) {
            // skip the measurement if the counter wrapped...
            ++stats.tapBgNumOperations;
            hrtime_t w = (start - init) / 1000;
            stats.tapBgWait += w;
            stats.tapBgWaitHisto.add(w);
            stats.tapBgMinWait.setIfLess(w);
            stats.tapBgMaxWait.setIfBigger(w);

            hrtime_t l = (stop - start) / 1000;
            stats.tapBgLoad += l;
            stats.tapBgLoadHisto.add(l);
            stats.tapBgMinLoad.setIfLess(l);
            stats.tapBgMaxLoad.setIfBigger(l);
        }

        return false;
    }

    std::string description() {
        std::stringstream ss;
        ss << "Fetching item from disk for tap:  " << key;
        return ss.str();
    }

private:
    const std::string name;
    const std::string key;
    EventuallyPersistentEngine *epe;
    hrtime_t init;
    hrtime_t connToken;
    uint64_t rowid;
    uint16_t vbucket;
};

void TapProducer::queueBGFetch_UNLOCKED(const std::string &key, uint64_t id, uint16_t vb) {
    shared_ptr<TapBGFetchCallback> dcb(new TapBGFetchCallback(&engine,
                                                              getName(), key,
                                                              vb, id, getConnectionToken()));
    engine.getEpStore()->getAuxIODispatcher()->schedule(dcb, NULL, Priority::TapBgFetcherPriority);
    ++bgJobIssued;
    std::map<uint16_t, TapCheckpointState>::iterator it = tapCheckpointState.find(vb);
    if (it != tapCheckpointState.end()) {
        ++(it->second.bgJobIssued);
    }
    assert(bgJobIssued > bgJobCompleted);
}

void TapProducer::completeBGFetchJob(Item *itm, uint16_t vbid, bool implicitEnqueue) {
    LockHolder lh(queueLock);
    std::map<uint16_t, TapCheckpointState>::iterator it = tapCheckpointState.find(vbid);

    // implicitEnqueue is used for the optimized disk fetch wherein we
    // receive the item and want the stats to reflect an
    // enqueue/execute cycle.
    if (implicitEnqueue) {
        ++bgJobIssued;
        if (it != tapCheckpointState.end()) {
            ++(it->second.bgJobIssued);
        }
    }
    ++bgJobCompleted;
    if (it != tapCheckpointState.end()) {
        ++(it->second.bgJobCompleted);
    }
    assert(bgJobIssued >= bgJobCompleted);

    if (itm && vbucketFilter(itm->getVBucketId())) {
        backfilledItems.push(itm);
        ++bgResultSize;
        if (it != tapCheckpointState.end()) {
            ++(it->second.bgResultSize);
        }
        stats.memOverhead.incr(sizeof(Item *));
        assert(stats.memOverhead.get() < GIGANTOR);
    } else {
        delete itm;
    }
}

Item* TapProducer::nextBgFetchedItem_UNLOCKED() {
    assert(!backfilledItems.empty());
    Item *rv = backfilledItems.front();
    assert(rv);
    backfilledItems.pop();
    --bgResultSize;

    std::map<uint16_t, TapCheckpointState>::iterator it =
        tapCheckpointState.find(rv->getVBucketId());
    if (it != tapCheckpointState.end()) {
        --(it->second.bgResultSize);
    }

    stats.memOverhead.decr(sizeof(Item *));
    assert(stats.memOverhead.get() < GIGANTOR);

    return rv;
}

void TapProducer::addStats(ADD_STAT add_stat, const void *c) {
    TapConnection::addStats(add_stat, c);

    LockHolder lh(queueLock);
    addStat("qlen", getQueueSize_UNLOCKED(), add_stat, c);
    addStat("qlen_high_pri", vBucketHighPriority.size(), add_stat, c);
    addStat("qlen_low_pri", vBucketLowPriority.size(), add_stat, c);
    addStat("vb_filters", vbucketFilter.size(), add_stat, c);
    addStat("vb_filter", filterText.c_str(), add_stat, c);
    addStat("rec_fetched", recordsFetched, add_stat, c);
    if (recordsSkipped > 0) {
        addStat("rec_skipped", recordsSkipped, add_stat, c);
    }
    addStat("idle", idle_UNLOCKED(), add_stat, c);
    addStat("has_queued_item", !emptyQueue_UNLOCKED(), add_stat, c);
    addStat("bg_result_size", bgResultSize, add_stat, c);
    addStat("bg_jobs_issued", bgJobIssued, add_stat, c);
    addStat("bg_jobs_completed", bgJobCompleted, add_stat, c);
    addStat("flags", flagsText, add_stat, c);
    addStat("suspended", isSuspended(), add_stat, c);
    addStat("paused", paused, add_stat, c);
    addStat("pending_backfill", isPendingBackfill_UNLOCKED(), add_stat, c);
    addStat("pending_disk_backfill", diskBackfillCounter > 0, add_stat, c);
    addStat("backfill_completed", isBackfillCompleted_UNLOCKED(), add_stat, c);
    addStat("backfill_start_timestamp", backfillTimestamp, add_stat, c);

    addStat("queue_memory", getQueueMemory(), add_stat, c);
    addStat("queue_fill", getQueueFillTotal(), add_stat, c);
    addStat("queue_drain", getQueueDrainTotal(), add_stat, c);
    addStat("queue_backoff", getQueueBackoff(), add_stat, c);
    addStat("queue_backfillremaining", getBackfillRemaining_UNLOCKED(), add_stat, c);
    addStat("queue_itemondisk", bgJobIssued - bgJobCompleted, add_stat, c);
    addStat("total_backlog_size",
            getBackfillRemaining_UNLOCKED() + getRemainingOnCheckpoints_UNLOCKED(),
            add_stat, c);
    addStat("total_noops", numNoops, add_stat, c);

    if (reconnects > 0) {
        addStat("reconnects", reconnects, add_stat, c);
    }
    if (backfillAge != 0) {
        addStat("backfill_age", (size_t)backfillAge, add_stat, c);
    }

    if (supportAck) {
        addStat("ack_seqno", seqno, add_stat, c);
        addStat("recv_ack_seqno", seqnoReceived, add_stat, c);
        addStat("seqno_ack_requested", seqnoAckRequested, add_stat, c);
        addStat("ack_log_size", tapLog.size(), add_stat, c);
        addStat("ack_window_full", windowIsFull(), add_stat, c);
        if (windowIsFull()) {
            addStat("expires", expiryTime - ep_current_time(), add_stat, c);
        }
    }

    if (tapFlagByteorderSupport) {
        addStat("flag_byteorder_support", true, add_stat, c);
    }
}

void TapProducer::aggregateQueueStats(TapCounter* aggregator) {
    LockHolder lh(queueLock);
    if (!aggregator) {
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "%s Pointer to the queue stats aggregator is NULL!!!\n",
                         logHeader());
        return;
    }
    aggregator->tap_queue += getQueueSize_UNLOCKED();
    aggregator->tap_queueFill += queueFill;
    aggregator->tap_queueDrain += queueDrain;
    aggregator->tap_queueBackoff += numTapNack;
    aggregator->tap_queueBackfillRemaining += getBackfillRemaining_UNLOCKED();
    aggregator->tap_queueItemOnDisk += (bgJobIssued - bgJobCompleted);
    aggregator->tap_totalBacklogSize += getBackfillRemaining_UNLOCKED() +
                                        getRemainingOnCheckpoints_UNLOCKED();
}

void TapProducer::processedEvent(tap_event_t event, ENGINE_ERROR_CODE)
{
    assert(event == TAP_ACK);
}

/**************** TAP Consumer **********************************************/
TapConsumer::TapConsumer(EventuallyPersistentEngine &theEngine,
                         const void *c,
                         const std::string &n) :
    TapConnection(theEngine, c, n)
{
    setSupportAck(true);
    setLogHeader("TAP (Consumer) " + getName() + " -");
}

void TapConsumer::addStats(ADD_STAT add_stat, const void *c) {
    TapConnection::addStats(add_stat, c);
    addStat("num_delete", numDelete, add_stat, c);
    addStat("num_delete_failed", numDeleteFailed, add_stat, c);
    addStat("num_flush", numFlush, add_stat, c);
    addStat("num_flush_failed", numFlushFailed, add_stat, c);
    addStat("num_mutation", numMutation, add_stat, c);
    addStat("num_mutation_failed", numMutationFailed, add_stat, c);
    addStat("num_opaque", numOpaque, add_stat, c);
    addStat("num_opaque_failed", numOpaqueFailed, add_stat, c);
    addStat("num_vbucket_set", numVbucketSet, add_stat, c);
    addStat("num_vbucket_set_failed", numVbucketSetFailed, add_stat, c);
    addStat("num_checkpoint_start", numCheckpointStart, add_stat, c);
    addStat("num_checkpoint_start_failed", numCheckpointStartFailed, add_stat, c);
    addStat("num_checkpoint_end", numCheckpointEnd, add_stat, c);
    addStat("num_checkpoint_end_failed", numCheckpointEndFailed, add_stat, c);
    addStat("num_unknown", numUnknown, add_stat, c);
}

void TapConsumer::setBackfillPhase(bool isBackfill, uint16_t vbucket) {
    const VBucketMap &vbuckets = engine.getEpStore()->getVBuckets();
    RCPtr<VBucket> vb = vbuckets.getBucket(vbucket);
    if (!(vb && supportCheckpointSync)) {
        return;
    }

    vb->setBackfillPhase(isBackfill);
    if (isBackfill) {
        // set the open checkpoint id to 0 to indicate the backfill phase.
        vb->checkpointManager.setOpenCheckpointId(0);
        // Note that when backfill is started, the destination always resets the vbucket
        // and its checkpoint datastructure.
    } else {
        // If backfill is completed for a given vbucket subscribed by this consumer, schedule
        // backfill for all TAP connections that are currently replicating that vbucket,
        // so that replica chain can be synchronized.
        std::set<uint16_t> backfillVB;
        backfillVB.insert(vbucket);
        TapConnMap &connMap = engine.getTapConnMap();
        connMap.scheduleBackfill(backfillVB);
    }
}

bool TapConsumer::isBackfillPhase(uint16_t vbucket) {
    const VBucketMap &vbuckets = engine.getEpStore()->getVBuckets();
    RCPtr<VBucket> vb = vbuckets.getBucket(vbucket);
    if (vb && vb->isBackfillPhase()) {
        return true;
    }
    return false;
}

void TapConsumer::processedEvent(tap_event_t event, ENGINE_ERROR_CODE ret)
{
    switch (event) {
    case TAP_ACK:
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                        "%s Consumer should never recieve a tap ack\n",
                        logHeader());
        abort();
        break;

    case TAP_FLUSH:
        if (ret == ENGINE_SUCCESS) {
            ++numFlush;
        } else {
            ++numFlushFailed;
        }
        break;

    case TAP_DELETION:
        if (ret == ENGINE_SUCCESS) {
            ++numDelete;
        } else {
            ++numDeleteFailed;
        }
        break;

    case TAP_MUTATION:
        if (ret == ENGINE_SUCCESS) {
            ++numMutation;
        } else {
            ++numMutationFailed;
        }
        break;

    case TAP_OPAQUE:
        if (ret == ENGINE_SUCCESS) {
            ++numOpaque;
        } else {
            ++numOpaqueFailed;
        }
        break;

    case TAP_VBUCKET_SET:
        if (ret == ENGINE_SUCCESS) {
            ++numVbucketSet;
        } else {
            ++numVbucketSetFailed;
        }
        break;

    case TAP_CHECKPOINT_START:
        if (ret == ENGINE_SUCCESS) {
            ++numCheckpointStart;
        } else {
            ++numCheckpointStartFailed;
        }
        break;

    case TAP_CHECKPOINT_END:
        if (ret == ENGINE_SUCCESS) {
            ++numCheckpointEnd;
        } else {
            ++numCheckpointEndFailed;
        }
        break;

    default:
        ++numUnknown;
    }
}

bool TapConsumer::processCheckpointCommand(tap_event_t event, uint16_t vbucket,
                                           uint64_t checkpointId) {
    const VBucketMap &vbuckets = engine.getEpStore()->getVBuckets();
    RCPtr<VBucket> vb = vbuckets.getBucket(vbucket);
    if (!vb) {
        return false;
    }

    // If the vbucket is in active, but not allowed to accept checkpoint messaages, simply ignore
    // those messages.
    if (vb->getState() == vbucket_state_active &&
        !engine.getCheckpointConfig().isInconsistentSlaveCheckpoint()) {
        getLogger()->log(EXTENSION_LOG_INFO, NULL,
                         "%s Checkpoint %llu ignored because vbucket %d is in active state\n",
                         logHeader(), checkpointId, vbucket);
        return true;
    }

    bool ret = true;
    switch (event) {
    case TAP_CHECKPOINT_START:
        {
            getLogger()->log(EXTENSION_LOG_INFO, NULL,
                             "%s Received checkpoint_start message with id %llu for vbucket %d\n",
                             logHeader(), checkpointId, vbucket);
            if (vb->isBackfillPhase() && checkpointId > 0) {
                setBackfillPhase(false, vbucket);
            }

            vb->checkpointManager.checkAndAddNewCheckpoint(checkpointId);
        }
        break;
    case TAP_CHECKPOINT_END:
        getLogger()->log(EXTENSION_LOG_INFO, NULL,
                         "%s Received checkpoint_end message with id %llu for vbucket %d\n",
                         logHeader(), checkpointId, vbucket);
        ret = vb->checkpointManager.closeOpenCheckpoint(checkpointId);
        break;
    default:
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "%s Invalid checkpoint message type (%d) for vbucket %d\n",
                         logHeader(), event, vbucket);
        ret = false;
        break;
    }
    return ret;
}

void TapConsumer::checkVBOpenCheckpoint(uint16_t vbucket) {
    const VBucketMap &vbuckets = engine.getEpStore()->getVBuckets();
    RCPtr<VBucket> vb = vbuckets.getBucket(vbucket);
    if (!vb || vb->getState() == vbucket_state_active) {
        return;
    }
    vb->checkpointManager.checkOpenCheckpoint(false, true);
}

bool TapProducer::isTimeForNoop() {
    bool rv = noop.swap(false);
    if (rv) {
        ++numNoops;
    }
    return rv;
}

void TapProducer::setTimeForNoop()
{
    rel_time_t now = ep_current_time();
    noop = (lastMsgTime + engine.getTapConnMap().getTapNoopInterval()) < now ? true : false;
}

queued_item TapProducer::nextFgFetched_UNLOCKED(bool &shouldPause) {
    shouldPause = false;

    if (!isBackfillCompleted_UNLOCKED()) {
        checkBackfillCompletion_UNLOCKED();
    }

    if (queue->empty() && isBackfillCompleted_UNLOCKED()) {
        const VBucketMap &vbuckets = engine.getEpStore()->getVBuckets();
        uint16_t invalid_count = 0;
        uint16_t open_checkpoint_count = 0;
        uint16_t wait_for_ack_count = 0;

        std::map<uint16_t, TapCheckpointState>::iterator it = tapCheckpointState.begin();
        for (; it != tapCheckpointState.end(); ++it) {
            uint16_t vbid = it->first;
            RCPtr<VBucket> vb = vbuckets.getBucket(vbid);
            if (!vb || (vb->getState() == vbucket_state_dead && !doTakeOver)) {
                getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                 "%s Skip vbucket %d checkpoint queue as it's in invalid state.\n",
                                 logHeader(), vbid);
                ++invalid_count;
                continue;
            }

            bool isLastItem = false;
            queued_item qi = vb->checkpointManager.nextItem(name, isLastItem);
            switch(qi->getOperation()) {
            case queue_op_set:
            case queue_op_del:
                if (supportCheckpointSync && isLastItem) {
                    it->second.lastItem = true;
                } else {
                    it->second.lastItem = false;
                }
                addEvent_UNLOCKED(qi);
                break;
            case queue_op_checkpoint_start:
                {
                    it->second.currentCheckpointId = qi->getSeqno();
                    if (supportCheckpointSync) {
                        it->second.state = checkpoint_start;
                        addCheckpointMessage_UNLOCKED(qi);
                    }
                }
                break;
            case queue_op_checkpoint_end:
                if (supportCheckpointSync) {
                    it->second.state = checkpoint_end;
                    uint32_t seqno_acked;
                    if (seqnoReceived == 0) {
                        seqno_acked = 0;
                    } else {
                        seqno_acked = isLastAckSucceed ? seqnoReceived : seqnoReceived - 1;
                    }
                    if (it->second.lastSeqNum <= seqno_acked &&
                        it->second.isBgFetchCompleted()) {
                        // All resident and non-resident items in a checkpoint are sent
                        // and acked. CHEKCPOINT_END message is going to be sent.
                        addCheckpointMessage_UNLOCKED(qi);
                    } else {
                        vb->checkpointManager.decrTapCursorFromCheckpointEnd(name);
                        ++wait_for_ack_count;
                    }
                }
                break;
            case queue_op_empty:
                {
                    ++open_checkpoint_count;
                    if (closedCheckpointOnly) {
                        // If all the cursors are at the open checkpoints, send the OPAQUE message
                        // to the TAP client so that it can close the connection if necessary.
                        if (open_checkpoint_count == (tapCheckpointState.size() - invalid_count)) {
                            TapVBucketEvent ev(TAP_OPAQUE, qi->getVBucketId(),
                                               (vbucket_state_t)htonl(TAP_OPAQUE_OPEN_CHECKPOINT));
                            addVBucketHighPriority_UNLOCKED(ev);
                        }
                    }
                }
                break;
            default:
                break;
            }
        }

        if (wait_for_ack_count == (tapCheckpointState.size() - invalid_count)) {
            // All the TAP cursors are now at their checkpoint end position and should wait until
            // they are implicitly acked for all items belonging to their corresponding checkpoint.
            shouldPause = true;
        } else if ((wait_for_ack_count + open_checkpoint_count) ==
                   (tapCheckpointState.size() - invalid_count)) {
            // All the TAP cursors are either at their checkpoint end position to wait for acks or
            // reaches to the end of the current open checkpoint.
            shouldPause = true;
        }
    }

    if (!queue->empty()) {
        queued_item qi = queue->front();
        queue->pop_front();
        queueSize = queue->empty() ? 0 : queueSize - 1;
        if (queueMemSize > sizeof(queued_item)) {
            queueMemSize.decr(sizeof(queued_item));
        } else {
            queueMemSize.set(0);
        }
        stats.memOverhead.decr(sizeof(queued_item));
        assert(stats.memOverhead.get() < GIGANTOR);
        ++recordsFetched;
        return qi;
    }

    if (!isBackfillCompleted_UNLOCKED()) {
        shouldPause = true;
    }
    queued_item empty_item(NULL);
    return empty_item;
}

size_t TapProducer::getRemainingOnCheckpoints_UNLOCKED() {
    size_t numItems = 0;
    const VBucketMap &vbuckets = engine.getEpStore()->getVBuckets();
    std::map<uint16_t, TapCheckpointState>::iterator it = tapCheckpointState.begin();
    for (; it != tapCheckpointState.end(); ++it) {
        uint16_t vbid = it->first;
        RCPtr<VBucket> vb = vbuckets.getBucket(vbid);
        if (!vb || (vb->getState() == vbucket_state_dead && !doTakeOver)) {
            continue;
        }
        numItems += vb->checkpointManager.getNumItemsForTAPConnection(name);
    }
    return numItems;
}

bool TapProducer::hasNextFromCheckpoints_UNLOCKED() {
    bool hasNext = false;
    const VBucketMap &vbuckets = engine.getEpStore()->getVBuckets();
    std::map<uint16_t, TapCheckpointState>::iterator it = tapCheckpointState.begin();
    for (; it != tapCheckpointState.end(); ++it) {
        uint16_t vbid = it->first;
        RCPtr<VBucket> vb = vbuckets.getBucket(vbid);
        if (!vb || (vb->getState() == vbucket_state_dead && !doTakeOver)) {
            continue;
        }
        hasNext = vb->checkpointManager.hasNext(name);
        if (hasNext) {
            break;
        }
    }
    return hasNext;
}

bool TapProducer::SetCursorToOpenCheckpoint(uint16_t vbid) {
    LockHolder lh(queueLock);
    const VBucketMap &vbuckets = engine.getEpStore()->getVBuckets();
    RCPtr<VBucket> vb = vbuckets.getBucket(vbid);
    if (!vb) {
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "%s Failed to set the TAP cursor to the open checkpoint"
                         " because vbucket %d does not exist anymore\n",
                         logHeader(), vbid);
        return false;
    }

    uint64_t checkpointId = vb->checkpointManager.getOpenCheckpointId();
    std::map<uint16_t, TapCheckpointState>::iterator it = tapCheckpointState.find(vbid);
    if (it == tapCheckpointState.end()) {
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "%s Failed to set the TAP cursor to the open checkpoint"
                         " because the TAP checkpoint state for vbucket %d does not exist\n",
                         logHeader(), vbid);
        return false;
    } else if (dumpQueue) {
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "%s Skip the TAP checkpoint cursor registration "
                         " because the TAP producer is connected with DUMP flag\n",
                         logHeader(), vbid);
        return false;
    }

    vb->checkpointManager.registerTAPCursor(name, checkpointId, closedCheckpointOnly, true);
    it->second.currentCheckpointId = checkpointId;
    return true;
}

void TapProducer::setRegisteredClient(bool isRegisteredClient) {
    registeredTAPClient = isRegisteredClient;
}

void TapProducer::setClosedCheckpointOnlyFlag(bool isClosedCheckpointOnly) {
    closedCheckpointOnly = isClosedCheckpointOnly;
}

void TapProducer::scheduleBackfill_UNLOCKED(const std::vector<uint16_t> &vblist) {
    if (backfillAge > (uint64_t)ep_real_time()) {
        return;
    }

    std::vector<uint16_t> new_vblist;
    const VBucketMap &vbuckets = engine.getEpStore()->getVBuckets();
    std::vector<uint16_t>::const_iterator vbit = vblist.begin();
    // Skip all the vbuckets that are (1) receiving backfill from their master nodes
    // or (2) already scheduled for backfill.
    for (; vbit != vblist.end(); ++vbit) {
        RCPtr<VBucket> vb = vbuckets.getBucket(*vbit);
        if (!vb || vb->isBackfillPhase() ||
            backfillVBuckets.find(*vbit) != backfillVBuckets.end()) {
            continue;
        }
        backfillVBuckets.insert(*vbit);
        if (backFillVBucketFilter.addVBucket(*vbit)) {
            new_vblist.push_back(*vbit);
        }
    }

    std::vector<uint16_t>::iterator it = new_vblist.begin();
    for (; it != new_vblist.end(); ++it) {
        RCPtr<VBucket> vb = vbuckets.getBucket(*it);
        if (!vb) {
            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                             "%s VBucket %d not exist for backfill. Skip it...\n",
                             logHeader(), *it);
            continue;
        }
        // As we set the cursor to the beginning of the open checkpoint when backfill
        // is scheduled, we can simply remove the cursor now.
        vb->checkpointManager.removeTAPCursor(name);
        // Send an initial_vbucket_stream message to the destination node so that it can
        // reset the corresponding vbucket before receiving the backfill stream.
        TapVBucketEvent hi(TAP_OPAQUE, *it,
                           (vbucket_state_t)htonl(TAP_OPAQUE_INITIAL_VBUCKET_STREAM));
        addVBucketHighPriority_UNLOCKED(hi);
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "%s Schedule the backfill for vbucket %d\n",
                         logHeader(), *it);
    }

    if (new_vblist.size() > 0) {
        doRunBackfill = true;
        backfillCompleted = false;
        backfillTimestamp = ep_real_time();
    }
}

Item* TapProducer::getNextItem(const void *c, uint16_t *vbucket, tap_event_t &ret,
                               bool &referenced) {
    LockHolder lh(queueLock);
    Item *itm = NULL;

    // Check if there are any checkpoint start / end messages to be sent to the TAP client.
    queued_item checkpoint_msg = nextCheckpointMessage_UNLOCKED();
    if (checkpoint_msg.get() != NULL) {
        switch (checkpoint_msg->getOperation()) {
        case queue_op_checkpoint_start:
            ret = TAP_CHECKPOINT_START;
            break;
        case queue_op_checkpoint_end:
            ret = TAP_CHECKPOINT_END;
            break;
        default:
            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                             "%s Checkpoint start or end msg with incorrect"
                             " opcode %d\n",
                             logHeader(), checkpoint_msg->getOperation());
            ret = TAP_DISCONNECT;
            return NULL;
        }
        *vbucket = checkpoint_msg->getVBucketId();
        uint64_t cid = htonll(checkpoint_msg->getSeqno());
        value_t vblob(Blob::New((const char*)&cid, sizeof(cid)));
        itm = new Item(checkpoint_msg->getKey(), 0, 0, vblob,
                       0, -1, checkpoint_msg->getVBucketId());
        return itm;
    }

    queued_item qi;

    // Check if there are any items fetched from disk for backfill operations.
    if (hasItemFromDisk_UNLOCKED()) {
        ret = TAP_MUTATION;
        itm = nextBgFetchedItem_UNLOCKED();
        *vbucket = itm->getVBucketId();
        if (!vbucketFilter(*vbucket)) {
            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                             "%s Drop a backfill item because vbucket %d "
                             "is no longer valid against vbucket filter.\n",
                             logHeader(), *vbucket);
            // We were going to use the item that we received from
            // disk, but the filter says not to, so we need to get rid
            // of it now.
            delete itm;
            ret = TAP_NOOP;
            return NULL;
        }

        // If there's a better version in memory, grab it,
        // else go with what we pulled from disk.
        GetValue gv(engine.getEpStore()->get(itm->getKey(), itm->getVBucketId(),
                                             c, false, false, false));
        if (gv.getStatus() == ENGINE_SUCCESS) {
            delete itm;
            itm = gv.getValue();
        } else if (gv.getStatus() == ENGINE_KEY_ENOENT || itm->isExpired(ep_real_time())) {
            ret = TAP_DELETION;
        }

        if (gv.isReferenced()) {
            referenced = true;
        }

        ++stats.numTapBGFetched;
        qi = queued_item(new QueuedItem(itm->getKey(), itm->getVBucketId(),
                                        ret == TAP_MUTATION ? queue_op_set : queue_op_del,
                                        itm->getSeqno()));
    } else if (hasItemFromVBHashtable_UNLOCKED()) { // Item from memory backfill or checkpoints
        if (waitForCheckpointMsgAck()) {
            getLogger()->log(EXTENSION_LOG_INFO, NULL,
                             "%s Waiting for an ack for checkpoint_start/checkpoint_end"
                             " messages.\n",
                             logHeader());
            ret = TAP_PAUSE;
            return NULL;
        }

        bool shouldPause = false;
        qi = nextFgFetched_UNLOCKED(shouldPause);
        if (qi.get() == NULL) {
            ret = shouldPause ? TAP_PAUSE : TAP_NOOP;
            return NULL;
        }
        *vbucket = qi->getVBucketId();
        if (!vbucketFilter(*vbucket)) {
            ret = TAP_NOOP;
            return NULL;
        }

        if (qi->getOperation() == queue_op_set) {
            GetValue gv(engine.getEpStore()->get(qi->getKey(), qi->getVBucketId(),
                                                 c, false, false, false));
            ENGINE_ERROR_CODE r = gv.getStatus();
            if (r == ENGINE_SUCCESS) {
                itm = gv.getValue();
                assert(itm);
                if (gv.isReferenced()) {
                    referenced = true;
                }
                ret = TAP_MUTATION;
            } else if (r == ENGINE_KEY_ENOENT) {
                // Item was deleted and set a message type to tap_deletion.
                itm = new Item(qi->getKey().c_str(), qi->getKey().length(), 0,
                               0, 0, 0, -1, qi->getVBucketId());
                itm->setSeqno(qi->getSeqno());
                ret = TAP_DELETION;
            } else if (r == ENGINE_EWOULDBLOCK) {
                queueBGFetch_UNLOCKED(qi->getKey(), gv.getId(), *vbucket);
                // If there's an item ready, return NOOP so we'll come
                // back immediately, otherwise pause the connection
                // while we wait.
                if (hasItemFromVBHashtable_UNLOCKED() || hasItemFromDisk_UNLOCKED()) {
                    ret = TAP_NOOP;
                } else {
                    ret = TAP_PAUSE;
                }
                return NULL;
            } else {
                if (r == ENGINE_NOT_MY_VBUCKET) {
                    getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                     "%s Trying to fetch an item for vbucket %d that "
                                     "doesn't exist on this server.\n",
                                     logHeader(), qi->getVBucketId());
                    ret = TAP_NOOP;
                } else {
                    getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                     "%s Tap internal error with status %d. "
                                     "Disconnecting\n", logHeader(), r);
                    ret = TAP_DISCONNECT;
                }
                return NULL;
            }
            ++stats.numTapFGFetched;
        } else if (qi->getOperation() == queue_op_del) {
            itm = new Item(qi->getKey().c_str(), qi->getKey().length(), 0,
                           0, 0, 0, -1, qi->getVBucketId());
            itm->setSeqno(qi->getSeqno());
            ret = TAP_DELETION;
            ++stats.numTapDeletes;
        }
    }

    if (ret == TAP_MUTATION || ret == TAP_DELETION) {
        ++queueDrain;
        addTapLogElement_UNLOCKED(qi);
        if (!isBackfillCompleted_UNLOCKED() && totalBackfillBacklogs > 0) {
            --totalBackfillBacklogs;
        }
    }

    return itm;
}

TapVBucketEvent TapProducer::checkDumpOrTakeOverCompletion() {
    LockHolder lh(queueLock);
    TapVBucketEvent ev(TAP_PAUSE, 0, vbucket_state_active);

    checkBackfillCompletion_UNLOCKED();
    if (mayCompleteDumpOrTakeover_UNLOCKED()) {
        ev = nextVBucketLowPriority_UNLOCKED();
        if (ev.event != TAP_PAUSE) {
            RCPtr<VBucket> vb = engine.getVBucket(ev.vbucket);
            vbucket_state_t myState(vb ? vb->getState() : vbucket_state_dead);
            assert(ev.event == TAP_VBUCKET_SET);
            if (ev.state == vbucket_state_active && myState == vbucket_state_active &&
                tapLog.size() < MAX_TAKEOVER_TAP_LOG_SIZE) {
                // Set vbucket state to dead if the number of items waiting for
                // implicit acks is less than the threshold.
                getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                 "%s VBucket <%d> is going dead to complete "
                                 "vbucket takeover.\n",
                                 logHeader(), ev.vbucket);
                engine.getEpStore()->setVBucketState(ev.vbucket, vbucket_state_dead);
                setTakeOverCompletionPhase(true);
            }
            if (tapLog.size() > 1) {
                // We're still waiting for acks for regular items.
                // Pop the tap log for this vbucket_state_active message and requeue it.
                tapLog.pop_back();
                TapVBucketEvent lo(TAP_VBUCKET_SET, ev.vbucket, vbucket_state_active);
                addVBucketLowPriority_UNLOCKED(lo);
                ev.event = TAP_PAUSE;
            }
        } else if (!tapLog.empty()) {
            ev.event = TAP_PAUSE;
        } else {
            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                             "%s Disconnecting tap stream.",
                             logHeader());
            setDisconnect(true);
            ev.event = TAP_DISCONNECT;
        }
    }

    return ev;
}

bool TapProducer::addEvent_UNLOCKED(const queued_item &it) {
    if (vbucketFilter(it->getVBucketId())) {
        bool wasEmpty = queue->empty();
        queue->push_back(it);
        ++queueSize;
        queueMemSize.incr(sizeof(queued_item));
        stats.memOverhead.incr(sizeof(queued_item));
        assert(stats.memOverhead.get() < GIGANTOR);
        return wasEmpty;
    } else {
        return queue->empty();
    }
}

TapVBucketEvent TapProducer::nextVBucketHighPriority_UNLOCKED() {
    TapVBucketEvent ret(TAP_PAUSE, 0, vbucket_state_active);
    if (!vBucketHighPriority.empty()) {
        ret = vBucketHighPriority.front();
        vBucketHighPriority.pop();

        // We might have objects in our queue that aren't in our filter
        // If so, just skip them..
        switch (ret.event) {
        case TAP_OPAQUE:
            opaqueCommandCode = (uint32_t)ret.state;
            if (opaqueCommandCode == htonl(TAP_OPAQUE_ENABLE_AUTO_NACK) ||
                opaqueCommandCode == htonl(TAP_OPAQUE_ENABLE_CHECKPOINT_SYNC) ||
                opaqueCommandCode == htonl(TAP_OPAQUE_CLOSE_BACKFILL) ||
                opaqueCommandCode == htonl(TAP_OPAQUE_COMPLETE_VB_FILTER_CHANGE)) {
                break;
            }
            // FALLTHROUGH
        default:
            if (!vbucketFilter(ret.vbucket)) {
                return nextVBucketHighPriority_UNLOCKED();
            }
        }

        if (ret.event == TAP_OPAQUE) {
            ++opaqueMsgCounter;
        }
        ++recordsFetched;
        ++seqno;
        addTapLogElement_UNLOCKED(ret);
    }
    return ret;
}

TapVBucketEvent TapProducer::nextVBucketLowPriority_UNLOCKED() {
    TapVBucketEvent ret(TAP_PAUSE, 0, vbucket_state_active);
    if (!vBucketLowPriority.empty()) {
        ret = vBucketLowPriority.front();
        vBucketLowPriority.pop();
        // We might have objects in our queue that aren't in our filter
        // If so, just skip them..
        if (!vbucketFilter(ret.vbucket)) {
            return nextVBucketHighPriority_UNLOCKED();
        }
        ++recordsFetched;
        ++seqno;
        addTapLogElement_UNLOCKED(ret);
    }
    return ret;
}

queued_item TapProducer::nextCheckpointMessage_UNLOCKED() {
    queued_item an_item(NULL);
    if (!checkpointMsgs.empty()) {
        an_item = checkpointMsgs.front();
        checkpointMsgs.pop();
        if (!vbucketFilter(an_item->getVBucketId())) {
            return nextCheckpointMessage_UNLOCKED();
        }
        ++checkpointMsgCounter;
        ++recordsFetched;
        addTapLogElement_UNLOCKED(an_item);
    }
    return an_item;
}

size_t TapProducer::getBackfillRemaining_UNLOCKED() {
    return backfillCompleted ? 0 : totalBackfillBacklogs;
}

size_t TapProducer::getBackfillQueueSize_UNLOCKED() {
    return backfillCompleted ? 0 : getQueueSize_UNLOCKED();
}

size_t TapProducer::getQueueSize_UNLOCKED() {
    bgResultSize = backfilledItems.empty() ? 0 : bgResultSize.get();
    queueSize = queue->empty() ? 0 : queueSize;
    return bgResultSize + (bgJobIssued - bgJobCompleted) + queueSize;
}

void TapProducer::incrBackfillRemaining(size_t incr) {
    LockHolder lh(queueLock);
    totalBackfillBacklogs += incr;
}

void TapProducer::flush() {
    LockHolder lh(queueLock);

    getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                     "%s Clear tap queues as part of flush operation.\n",
                     logHeader());

    pendingFlush = true;
    clearQueues_UNLOCKED();
}

void TapProducer::appendQueue(std::list<queued_item> *q) {
    LockHolder lh(queueLock);
    size_t count = 0;
    std::list<queued_item>::iterator it = q->begin();
    for (; it != q->end(); ++it) {
        if (vbucketFilter((*it)->getVBucketId())) {
            queue->push_back(*it);
            ++count;
        }
    }
    queueSize += count;
    stats.memOverhead.incr(count * sizeof(queued_item));
    assert(stats.memOverhead.get() < GIGANTOR);
    queueMemSize.incr(count * sizeof(queued_item));
    q->clear();
}

bool TapProducer::runBackfill(VBucketFilter &vbFilter) {
    LockHolder lh(queueLock);
    bool rv = doRunBackfill;
    if (doRunBackfill) {
        doRunBackfill = false;
        ++pendingBackfillCounter; // Will be decremented when each backfill thread is completed
        vbFilter = backFillVBucketFilter;
        backFillVBucketFilter.reset();
    }
    return rv;
}
