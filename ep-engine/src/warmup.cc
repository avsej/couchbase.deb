/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2012 Couchbase, Inc.
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
#include "warmup.hh"
#include "ep_engine.h"

#define STATWRITER_NAMESPACE warmup
#include "statwriter.hh"
#undef STATWRITER_NAMESPACE

const int WarmupState::Initialize = 0;
const int WarmupState::LoadingMutationLog = 1;
const int WarmupState::EstimateDatabaseItemCount = 2;
const int WarmupState::KeyDump = 3;
const int WarmupState::CheckForAccessLog = 4;
const int WarmupState::LoadingAccessLog = 5;
const int WarmupState::LoadingKVPairs = 6;
const int WarmupState::LoadingData = 7;
const int WarmupState::Done = 8;

const char *WarmupState::toString(void) const {
    return getStateDescription(state);
}

const char *WarmupState::getStateDescription(int st) const {
    switch (st) {
    case Initialize:
        return "initialize";
    case LoadingMutationLog:
        return "loading mutation log";
    case EstimateDatabaseItemCount:
        return "estimating database item count";
    case KeyDump:
        return "loading keys";
    case CheckForAccessLog:
        return "determine access log availability";
    case LoadingAccessLog:
        return "loading access log";
    case LoadingKVPairs:
        return "loading k/v pairs";
    case LoadingData:
        return "loading data";
    case Done:
        return "done";
    default:
        return "Illegal state";
    }
}

void WarmupState::transition(int to, bool allowAnystate) {
    if (allowAnystate || legalTransition(to)) {
        std::stringstream ss;
        ss << "Warmup transition from state \""
           << getStateDescription(state) << "\" to \""
           << getStateDescription(to) << "\"";
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL, "%s",
                         ss.str().c_str());
        state = to;
    } else {
        // Throw an exception to make it possible to test the logic ;)
        std::stringstream ss;
        ss << "Illegal state transition from \"" << *this << "\" to " << to;
        throw std::runtime_error(ss.str());
    }
}

bool WarmupState::legalTransition(int to) const {
    switch (state) {
    case Initialize:
        return to == LoadingMutationLog;
    case LoadingMutationLog:
        return (to == CheckForAccessLog ||
                to == EstimateDatabaseItemCount);
    case EstimateDatabaseItemCount:
        return (to == KeyDump);
    case KeyDump:
        return (to == LoadingKVPairs ||
                to == CheckForAccessLog);
    case CheckForAccessLog:
        return (to == LoadingAccessLog || to == LoadingData);
    case LoadingAccessLog:
        return (to == Done || to == LoadingData);
    case LoadingKVPairs:
        return (to == Done);
    case LoadingData:
        return (to == Done);

    default:
        return false;
    }
}

std::ostream& operator <<(std::ostream &out, const WarmupState &state)
{
    out << state.toString();
    return out;
}

//////////////////////////////////////////////////////////////////////////////
//                                                                          //
//    Helper class used to insert data into the epstore                     //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////

/**
 * Helper class used to insert items into the storage by using
 * the KVStore::dump method to load items from the database
 */
class LoadStorageKVPairCallback : public Callback<GetValue> {
public:
    LoadStorageKVPairCallback(EventuallyPersistentStore *ep,
                              bool _maybeEnableTraffic, int _warmupState)
        : vbuckets(ep->vbuckets), stats(ep->getEPEngine().getEpStats()),
          epstore(ep), startTime(ep_real_time()),
          hasPurged(false), maybeEnableTraffic(_maybeEnableTraffic),
          warmupState(_warmupState)
    {
        assert(epstore);
    }

    void initVBucket(uint16_t vbid,
                     const vbucket_state &vbstate);

    void callback(GetValue &val);

private:

    bool shouldEject() {
        return stats.getTotalMemoryUsed() >= stats.mem_low_wat;
    }

    void purge();

    VBucketMap &vbuckets;
    EPStats    &stats;
    EventuallyPersistentStore *epstore;
    time_t      startTime;
    bool        hasPurged;
    bool        maybeEnableTraffic;
    int         warmupState;
};

void LoadStorageKVPairCallback::initVBucket(uint16_t vbid,
                                            const vbucket_state &vbs) {
    RCPtr<VBucket> vb = vbuckets.getBucket(vbid);
    if (!vb) {
        vb.reset(new VBucket(vbid, vbucket_state_dead, stats,
                             epstore->getEPEngine().getCheckpointConfig()));
        vbuckets.addBucket(vb);
    }
    // Set the past initial state of each vbucket.
    vb->setInitialState(vbs.state);
    // Pass the open checkpoint Id for each vbucket.
    vb->checkpointManager.setOpenCheckpointId(vbs.checkpointId);
    // Pass the max deleted seqno for each vbucket.
    vb->ht.setMaxDeletedSeqno(vbs.maxDeletedSeqno);
    // For each vbucket, set its latest checkpoint Id that was
    // successfully persisted.
    vbuckets.setPersistenceCheckpointId(vbid, vbs.checkpointId - 1);
}

void LoadStorageKVPairCallback::callback(GetValue &val) {
    Item *i = val.getValue();
    if (i != NULL) {
        RCPtr<VBucket> vb = vbuckets.getBucket(i->getVBucketId());
        if (!vb) {
            vb.reset(new VBucket(i->getVBucketId(), vbucket_state_dead, stats,
                                 epstore->getEPEngine().getCheckpointConfig()));
            vbuckets.addBucket(vb);
        }
        bool succeeded(false);
        int retry = 2;
        do {
            switch (vb->ht.insert(*i, shouldEject(), val.isPartial())) {
            case NOMEM:
                if (retry == 2) {
                    if (hasPurged) {
                        if (++stats.warmOOM == 1) {
                            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                             "Warmup dataload failure: max_size too low.");
                        }
                    } else {
                        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                         "Emergency startup purge to free space for load.");
                        purge();
                    }
                } else {
                    getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                     "Cannot store an item after emergency purge.");
                    ++stats.warmOOM;
                }
                break;
            case INVALID_CAS:
                if (epstore->getROUnderlying()->isKeyDumpSupported()) {
                    getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                        "Value changed in memory before restore from disk. Ignored disk value for: %s.",
                         i->getKey().c_str());
                } else {
                    getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                        "Warmup dataload error: Duplicate key: %s.",
                        i->getKey().c_str());
                }
                ++stats.warmDups;
                succeeded = true;
                break;
            case NOT_FOUND:
                succeeded = true;
                break;
            default:
                abort();
            }
        } while (!succeeded && retry-- > 0);

        bool expired = i->isExpired(startTime);
        if (succeeded && expired) {
            ItemMetaData itemMeta;

            ++stats.warmupExpired;
            epstore->incExpirationStat(vb, false);
            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                             "Item was expired at load:  %s",
                             i->getKey().c_str());
            uint64_t cas = 0;
            epstore->deleteItem(i->getKey(),
                                &cas,
                                i->getVBucketId(), NULL,
                                true, false, // force, use_meta
                                &itemMeta);
        }
        if (succeeded && epstore->warmupTask->doReconstructLog() && !expired) {
            epstore->mutationLog.newItem(i->getVBucketId(), i->getKey(), i->getId());
        }
        delete i;
        val.setValue(NULL);

        if (maybeEnableTraffic) {
            epstore->maybeEnableTraffic();
        }
    }

    switch (warmupState) {
        case WarmupState::KeyDump:
        case WarmupState::LoadingMutationLog:
            ++stats.warmedUpKeys;
            break;
        case WarmupState::LoadingData:
        case WarmupState::LoadingAccessLog:
            ++stats.warmedUpValues;
            break;
        default:
            ++stats.warmedUpKeys;
            ++stats.warmedUpValues;
    }
}

void LoadStorageKVPairCallback::purge() {
    class EmergencyPurgeVisitor : public VBucketVisitor {
    public:
        EmergencyPurgeVisitor(EPStats &s) : stats(s) {}

        void visit(StoredValue *v) {
            v->ejectValue(stats, currentBucket->ht);
        }
    private:
        EPStats &stats;
    };

    std::vector<int> vbucketIds(vbuckets.getBuckets());
    std::vector<int>::iterator it;
    EmergencyPurgeVisitor epv(stats);
    for (it = vbucketIds.begin(); it != vbucketIds.end(); ++it) {
        int vbid = *it;
        RCPtr<VBucket> vb = vbuckets.getBucket(vbid);
        if (vb && epv.visitBucket(vb)) {
            vb->ht.visit(epv);
        }
    }
    hasPurged = true;
}

//////////////////////////////////////////////////////////////////////////////
//                                                                          //
//    Implementation of the warmup class                                    //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////


Warmup::Warmup(EventuallyPersistentStore *st, Dispatcher *d) :
    state(), store(st), dispatcher(d), startTime(0), metadata(0), warmup(0),
    reconstructLog(false), estimateTime(0),
    estimatedItemCount(std::numeric_limits<size_t>::max()),
    corruptMutationLog(false),
    corruptAccessLog(false),
    estimatedWarmupCount(std::numeric_limits<size_t>::max())
{

}

void Warmup::setEstimatedItemCount(size_t to)
{
    estimatedItemCount = to;
}

void Warmup::setEstimatedWarmupCount(size_t to)
{
    estimatedWarmupCount = to;
}

void Warmup::setReconstructLog(bool val)
{
    reconstructLog = val;
}

void Warmup::start(void)
{
    store->stats.warmupComplete.set(false);
    dispatcher->schedule(shared_ptr<WarmupStepper>(new WarmupStepper(this)),
                         &task, Priority::WarmupPriority);
}

void Warmup::stop(void)
{
    if (task.get()) {
        dispatcher->cancel(task);
        // immediately transition to completion so that
        // the warmup listener also breaks away from the waiting
        transition(WarmupState::Done, true);
        done(*dispatcher, task);
    }
}

bool Warmup::initialize(Dispatcher&, TaskId &)
{
    startTime = gethrtime();
    initialVbState = store->loadVBucketState();
    store->loadSessionStats();
    transition(WarmupState::LoadingMutationLog);
    return true;
}

bool Warmup::loadingMutationLog(Dispatcher&, TaskId &)
{
    shared_ptr<Callback<GetValue> > cb(createLKVPCB(initialVbState, false,
                                                    state.getState()));
    bool success = false;

    try {
        success = store->warmupFromLog(initialVbState, cb);
    } catch (MutationLog::ReadException e) {
        corruptMutationLog = true;
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "Error reading warmup log:  %s", e.what());
    }

    if (success) {
        transition(WarmupState::CheckForAccessLog);
    } else {
        try {
            if (store->mutationLog.reset()) {
                setReconstructLog(true);
            }
        } catch (MutationLog::ReadException e) {
            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                             "Failed to reset mutation log:  %s", e.what());
        }

        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "Failed to load mutation log, falling back to key dump");
        transition(WarmupState::EstimateDatabaseItemCount);
    }

    return true;
}

bool Warmup::estimateDatabaseItemCount(Dispatcher&, TaskId &)
{
    hrtime_t st = gethrtime();
    store->roUnderlying->getEstimatedItemCount(estimatedItemCount);
    estimateTime = gethrtime() - st;

    transition(WarmupState::KeyDump);
    return true;
}

bool Warmup::keyDump(Dispatcher&, TaskId &)
{
    bool success = false;
    if (store->roUnderlying->isKeyDumpSupported()) {
        shared_ptr<Callback<GetValue> > cb(createLKVPCB(initialVbState, false,
                                                        state.getState()));
        std::map<uint16_t, vbucket_state>::const_iterator it;
        std::vector<uint16_t> vbids;
        for (it = initialVbState.begin(); it != initialVbState.end(); ++it) {
            uint16_t vbid = it->first;
            vbucket_state vbs = it->second;
            if (vbs.state == vbucket_state_active || vbs.state == vbucket_state_replica) {
                vbids.push_back(vbid);
            }
        }

        store->roUnderlying->dumpKeys(vbids, cb);
        success = true;
    }

    if (success) {
        transition(WarmupState::CheckForAccessLog);
    } else {
        if (store->roUnderlying->isKeyDumpSupported()) {
            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                             "Failed to dump keys, falling back to full dump");
        }
        transition(WarmupState::LoadingKVPairs);
    }

    return true;
}

class EstimateWarmupSize : public Callback<size_t> {
public:
    EstimateWarmupSize(Warmup &w) : warmup(w) {}

    void callback(size_t &val) {
        warmup.setEstimatedWarmupCount(val);
    }

private:
    Warmup &warmup;
};

bool Warmup::checkForAccessLog(Dispatcher&, TaskId &)
{
    metadata = gethrtime() - startTime;
    getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                     "metadata loaded in %s",
                     hrtime2text(metadata).c_str());

    std::string curr = store->accessLog.getLogFile();
    std::string old = store->accessLog.getLogFile();
    old.append(".old");
    if (access(curr.c_str(), F_OK) == 0 || access(old.c_str(), F_OK) == 0) {
        transition(WarmupState::LoadingAccessLog);
    } else {
        transition(WarmupState::LoadingData);
    }

    return true;
}

bool Warmup::loadingAccessLog(Dispatcher&, TaskId &)
{
    EstimateWarmupSize w(*this);
    LoadStorageKVPairCallback *load_cb = createLKVPCB(initialVbState, true,
                                                      state.getState());
    bool success = false;
    hrtime_t stTime = gethrtime();
    if (store->accessLog.exists()) {
        try {
            store->accessLog.open();
            if (store->roUnderlying->warmup(store->accessLog, initialVbState,
                                            *load_cb, w) != (size_t)-1) {
                success = true;
            }
        } catch (MutationLog::ReadException e) {
            corruptAccessLog = true;
        }
    }

    if (!success) {
        // Do we have the previous file?
        std::string nm = store->accessLog.getLogFile();
        nm.append(".old");
        MutationLog old(nm);
        if (old.exists()) {
            try {
                old.open();
                if (store->roUnderlying->warmup(old, initialVbState,
                                                *load_cb, w) != (size_t)-1) {
                    success = true;
                }
            } catch (MutationLog::ReadException e) {
                corruptAccessLog = true;
            }
        }
    }

    size_t numItems = store->getEPEngine().getEpStats().warmedUpValues;
    if (success && numItems) {
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "%d items loaded from access log, completed in %s",
                         numItems,
                         hrtime2text((gethrtime() - stTime) / 1000).c_str());
        if (doReconstructLog()) {
            store->mutationLog.commit1();
            store->mutationLog.commit2();
            setReconstructLog(false);
        }
        transition(WarmupState::Done);
    } else {
        size_t estimatedCount = store->getEPEngine().getEpStats().warmedUpKeys;
        setEstimatedWarmupCount(estimatedCount);
        transition(WarmupState::LoadingData);
    }

    delete load_cb;
    return true;
}

bool Warmup::loadingKVPairs(Dispatcher&, TaskId &)
{
    shared_ptr<Callback<GetValue> > cb(createLKVPCB(initialVbState, false,
                                                    state.getState()));
    store->roUnderlying->dump(cb);

    if (doReconstructLog()) {
        store->mutationLog.commit1();
        store->mutationLog.commit2();
        setReconstructLog(false);
    }
    transition(WarmupState::Done);
    return true;
}

bool Warmup::loadingData(Dispatcher&, TaskId &)
{
    size_t estimatedCount = store->getEPEngine().getEpStats().warmedUpKeys;
    setEstimatedWarmupCount(estimatedCount);

    shared_ptr<Callback<GetValue> > cb(createLKVPCB(initialVbState, true,
                                       state.getState()));
    store->roUnderlying->dump(cb);
    transition(WarmupState::Done);
    return true;
}

bool Warmup::done(Dispatcher&, TaskId &)
{
    warmup = gethrtime() - startTime;
    store->warmupCompleted();
    store->stats.warmupComplete.set(true);
    getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                     "warmup completed in %s",
                     hrtime2text(warmup).c_str());
    return false;
}

bool Warmup::step(Dispatcher &d, TaskId &t) {
    try {
        switch (state.getState()) {
        case WarmupState::Initialize:
            return initialize(d, t);
        case WarmupState::LoadingMutationLog:
            return loadingMutationLog(d, t);
        case WarmupState::EstimateDatabaseItemCount:
            return estimateDatabaseItemCount(d, t);
        case WarmupState::KeyDump:
            return keyDump(d, t);
        case WarmupState::CheckForAccessLog:
            return checkForAccessLog(d, t);
        case WarmupState::LoadingAccessLog:
            return loadingAccessLog(d, t);
        case WarmupState::LoadingKVPairs:
            return loadingKVPairs(d, t);
        case WarmupState::LoadingData:
            return loadingData(d, t);
        case WarmupState::Done:
            return done(d, t);
        default:
            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                             "Internal error.. Illegal warmup state %d",
                             state.getState());
            abort();
        }
    } catch(std::runtime_error &e) {
        std::stringstream ss;
        ss << "Exception in warmup loop: " << e.what() << std::endl;
        getLogger()->log(EXTENSION_LOG_WARNING, NULL, "%s",
                         ss.str().c_str());
        abort();
    }
}

void Warmup::transition(int to, bool force) {
    int old = state.getState();
    if (old != WarmupState::Done) {
        state.transition(to, force);
        fireStateChange(old, to);
    }
}

void Warmup::addWarmupStateListener(WarmupStateListener *listener) {
    LockHolder lh(stateListeners.mutex);
    stateListeners.listeners.push_back(listener);
}

void Warmup::removeWarmupStateListener(WarmupStateListener *listener) {
    LockHolder lh(stateListeners.mutex);
    stateListeners.listeners.remove(listener);
}

void Warmup::fireStateChange(const int from, const int to)
{
    LockHolder lh(stateListeners.mutex);
    std::list<WarmupStateListener*>::iterator ii;
    for (ii = stateListeners.listeners.begin();
         ii != stateListeners.listeners.end();
         ++ii) {
        (*ii)->stateChanged(from, to);
    }
}

template <typename T>
void Warmup::addStat(const char *nm, T val, ADD_STAT add_stat,
                     const void *c) const {
    std::string name = "ep_warmup";
    if (nm != NULL) {
        name.append("_");
        name.append(nm);
    }

    std::stringstream value;
    value << val;
    add_casted_stat(name.data(), value.str().data(), add_stat, c);
}

void Warmup::addStats(ADD_STAT add_stat, const void *c) const
{
    if (store->getEPEngine().getConfiguration().isWarmup()) {
        EPStats &stats = store->getEPEngine().getEpStats();
        addStat(NULL, "enabled", add_stat, c);
        const char *stateName = state.toString();
        addStat("state", stateName, add_stat, c);
        if (stats.warmupComplete) {
            addStat("thread", "complete", add_stat, c);
        } else {
            addStat("thread", "running", add_stat, c);
        }
        addStat("key_count", stats.warmedUpKeys, add_stat, c);
        addStat("value_count", stats.warmedUpValues, add_stat, c);
        addStat("dups", stats.warmDups, add_stat, c);
        addStat("oom", stats.warmOOM, add_stat, c);
        addStat("item_expired", stats.warmupExpired, add_stat, c);
        addStat("min_memory_threshold",
                stats.warmupMemUsedCap * 100.0, add_stat, c);
        addStat("min_item_threshold",
                stats.warmupNumReadCap * 100.0, add_stat, c);

        if (metadata > 0) {
            addStat("keys_time", metadata / 1000, add_stat, c);
        }

        if (warmup > 0) {
            addStat("time", warmup / 1000, add_stat, c);
        }

        if (estimatedItemCount == std::numeric_limits<size_t>::max()) {
            addStat("estimated_key_count", "unknown", add_stat, c);
        } else {
            if (estimateTime != 0) {
                addStat("estimate_time", estimateTime / 1000, add_stat, c);
            }
            addStat("estimated_key_count", estimatedItemCount, add_stat, c);
        }

        if (corruptMutationLog) {
            addStat("mutation_log", "corrupt", add_stat, c);
        }

        if (corruptAccessLog) {
            addStat("access_log", "corrupt", add_stat, c);
        }

        if (estimatedWarmupCount ==  std::numeric_limits<size_t>::max()) {
            addStat("estimated_value_count", "unknown", add_stat, c);
        } else {
            addStat("estimated_value_count", estimatedWarmupCount, add_stat, c);
        }
   } else {
        addStat(NULL, "disabled", add_stat, c);
    }
}

LoadStorageKVPairCallback *Warmup::createLKVPCB(const std::map<uint16_t, vbucket_state> &st,
                                                bool maybeEnable, int warmupState)
{
    LoadStorageKVPairCallback *load_cb;
    load_cb = new LoadStorageKVPairCallback(store, maybeEnable, warmupState);
    std::map<uint16_t, vbucket_state>::const_iterator it;
    for (it = st.begin(); it != st.end(); ++it) {
        uint16_t vbid = it->first;
        vbucket_state vbs = it->second;
        vbs.checkpointId++;
        load_cb->initVBucket(vbid, vbs);
    }

    return load_cb;
}
