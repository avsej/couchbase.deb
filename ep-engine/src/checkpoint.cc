/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "config.h"
#include "vbucket.hh"
#include "checkpoint.hh"
#include "ep_engine.h"

/**
 * A listener class to update checkpoint related configs at runtime.
 */
class CheckpointConfigChangeListener : public ValueChangedListener {
public:
    CheckpointConfigChangeListener(CheckpointConfig &c) : config(c) { }
    virtual ~CheckpointConfigChangeListener() { }

    virtual void sizeValueChanged(const std::string &key, size_t value) {
        if (key.compare("chk_period") == 0) {
            config.setCheckpointPeriod(value);
        } else if (key.compare("chk_max_items") == 0) {
            config.setCheckpointMaxItems(value);
        } else if (key.compare("max_checkpoints") == 0) {
            config.setMaxCheckpoints(value);
        }
    }

    virtual void booleanValueChanged(const std::string &key, bool value) {
        if (key.compare("inconsistent_slave_chk") == 0) {
            config.allowInconsistentSlaveCheckpoint(value);
        } else if (key.compare("item_num_based_new_chk") == 0) {
            config.allowItemNumBasedNewCheckpoint(value);
        } else if (key.compare("keep_closed_chks") == 0) {
            config.allowKeepClosedCheckpoints(value);
        }
    }

private:
    CheckpointConfig &config;
};

#define STATWRITER_NAMESPACE checkpoint
#include "statwriter.hh"
#undef STATWRITER_NAMESPACE

Checkpoint::~Checkpoint() {
    getLogger()->log(EXTENSION_LOG_INFO, NULL,
                     "Checkpoint %llu for vbucket %d is purged from memory.\n",
                     checkpointId, vbucketId);
    stats.memOverhead.decr(memorySize());
    assert(stats.memOverhead.get() < GIGANTOR);
}

void Checkpoint::setState(checkpoint_state state) {
    checkpointState = state;
}

void Checkpoint::popBackCheckpointEndItem() {
    if (toWrite.size() > 0 && toWrite.back()->getOperation() == queue_op_checkpoint_end) {
        keyIndex.erase(toWrite.back()->getKey());
        toWrite.pop_back();
    }
}

bool Checkpoint::keyExists(const std::string &key) {
    return keyIndex.find(key) != keyIndex.end();
}

queue_dirty_t Checkpoint::queueDirty(const queued_item &qi, CheckpointManager *checkpointManager) {
    assert (checkpointState == CHECKPOINT_OPEN);

    uint64_t newMutationId = checkpointManager->nextMutationId();
    queue_dirty_t rv;

    checkpoint_index::iterator it = keyIndex.find(qi->getKey());
    // Check if this checkpoint already had an item for the same key.
    if (it != keyIndex.end()) {
        std::list<queued_item>::iterator currPos = it->second.position;
        uint64_t currMutationId = it->second.mutation_id;
        CheckpointCursor &pcursor = checkpointManager->persistenceCursor;

        if (*(pcursor.currentCheckpoint) == this) {
            // If the existing item is in the left-hand side of the item pointed by the
            // persistence cursor, decrease the persistence cursor's offset by 1.
            const std::string &key = (*(pcursor.currentPos))->getKey();
            checkpoint_index::iterator ita = keyIndex.find(key);
            if (ita != keyIndex.end()) {
                uint64_t mutationId = ita->second.mutation_id;
                if (currMutationId <= mutationId) {
                    checkpointManager->decrCursorOffset_UNLOCKED(pcursor, 1);
                }
            }
            // If the persistence cursor points to the existing item for the same key,
            // shift the cursor left by 1.
            if (pcursor.currentPos == currPos) {
                checkpointManager->decrCursorPos_UNLOCKED(pcursor);
            }
        }

        std::map<const std::string, CheckpointCursor>::iterator map_it;
        for (map_it = checkpointManager->tapCursors.begin();
             map_it != checkpointManager->tapCursors.end(); map_it++) {

            if (*(map_it->second.currentCheckpoint) == this) {
                const std::string &key = (*(map_it->second.currentPos))->getKey();
                checkpoint_index::iterator ita = keyIndex.find(key);
                if (ita != keyIndex.end()) {
                    uint64_t mutationId = ita->second.mutation_id;
                    if (currMutationId <= mutationId) {
                        checkpointManager->decrCursorOffset_UNLOCKED(map_it->second, 1);
                    }
                }
                // If an TAP cursor points to the existing item for the same key, shift it left by 1
                if (map_it->second.currentPos == currPos) {
                    checkpointManager->decrCursorPos_UNLOCKED(map_it->second);
                }
            }
        }

        queued_item &existing_itm = *currPos;
        existing_itm->setOperation(qi->getOperation());
        existing_itm->setQueuedTime(qi->getQueuedTime());
        toWrite.push_back(existing_itm);
        // Remove the existing item for the same key from the list.
        toWrite.erase(currPos);
        rv = EXISTING_ITEM;
    } else {
        if (qi->getOperation() == queue_op_set || qi->getOperation() == queue_op_del) {
            ++numItems;
        }
        rv = NEW_ITEM;
        // Push the new item into the list
        toWrite.push_back(qi);
    }

    if (qi->getKey().size() > 0) {
        std::list<queued_item>::iterator last = toWrite.end();
        // --last is okay as the list is not empty now.
        index_entry entry = {--last, newMutationId};
        // Set the index of the key to the new item that is pushed back into the list.
        keyIndex[qi->getKey()] = entry;
        if (rv == NEW_ITEM) {
            size_t newEntrySize = qi->getKey().size() + sizeof(index_entry) + sizeof(queued_item);
            memOverhead += newEntrySize;
            stats.memOverhead.incr(newEntrySize);
            assert(stats.memOverhead.get() < GIGANTOR);
        }
    }
    return rv;
}

size_t Checkpoint::mergePrevCheckpoint(Checkpoint *pPrevCheckpoint) {
    size_t numNewItems = 0;
    size_t newEntryMemOverhead = 0;
    std::list<queued_item>::reverse_iterator rit = pPrevCheckpoint->rbegin();

    getLogger()->log(EXTENSION_LOG_INFO, NULL,
                     "Collapse the checkpoint %llu into the checkpoint %llu for vbucket %d.\n",
                     pPrevCheckpoint->getId(), checkpointId, vbucketId);

    for (; rit != pPrevCheckpoint->rend(); ++rit) {
        const std::string &key = (*rit)->getKey();
        if ((*rit)->getOperation() != queue_op_del &&
            (*rit)->getOperation() != queue_op_set) {
            continue;
        }
        checkpoint_index::iterator it = keyIndex.find(key);
        if (it == keyIndex.end()) {
            std::list<queued_item>::iterator pos = toWrite.begin();
            // Skip the first two meta items
            ++pos; ++pos;
            toWrite.insert(pos, *rit);
            index_entry entry = {--pos, pPrevCheckpoint->getMutationIdForKey(key)};
            keyIndex[key] = entry;
            newEntryMemOverhead += key.size() + sizeof(index_entry);
            ++numItems;
            ++numNewItems;
        }
    }
    memOverhead += newEntryMemOverhead;
    stats.memOverhead.incr(newEntryMemOverhead);
    assert(stats.memOverhead.get() < GIGANTOR);
    return numNewItems;
}

uint64_t Checkpoint::getMutationIdForKey(const std::string &key) {
    uint64_t mid = 0;
    checkpoint_index::iterator it = keyIndex.find(key);
    if (it != keyIndex.end()) {
        mid = it->second.mutation_id;
    }
    return mid;
}

CheckpointManager::~CheckpointManager() {
    LockHolder lh(queueLock);
    std::list<Checkpoint*>::iterator it = checkpointList.begin();
    while(it != checkpointList.end()) {
        delete *it;
        ++it;
    }
}

uint64_t CheckpointManager::getOpenCheckpointId_UNLOCKED() {
    if (checkpointList.size() == 0) {
        return 0;
    }

    uint64_t id = checkpointList.back()->getId();
    return checkpointList.back()->getState() == CHECKPOINT_OPEN ? id : id + 1;
}

uint64_t CheckpointManager::getOpenCheckpointId() {
    LockHolder lh(queueLock);
    return getOpenCheckpointId_UNLOCKED();
}

uint64_t CheckpointManager::getLastClosedCheckpointId_UNLOCKED() {
    if (!isCollapsedCheckpoint) {
        uint64_t id = getOpenCheckpointId_UNLOCKED();
        lastClosedCheckpointId = id > 0 ? (id - 1) : 0;
    }
    return lastClosedCheckpointId;
}

uint64_t CheckpointManager::getLastClosedCheckpointId() {
    LockHolder lh(queueLock);
    return getLastClosedCheckpointId_UNLOCKED();
}

void CheckpointManager::setOpenCheckpointId_UNLOCKED(uint64_t id) {
    if (checkpointList.size() > 0) {
        getLogger()->log(EXTENSION_LOG_INFO, NULL,
                         "Set the current open checkpoint id to %llu for vbucket %d.\n",
                         id, vbucketId);
        checkpointList.back()->setId(id);
        // Update the checkpoint_start item with the new Id.
        queued_item qi = createCheckpointItem(id, vbucketId, queue_op_checkpoint_start);
        std::list<queued_item>::iterator it = ++(checkpointList.back()->begin());
        *it = qi;
    }
}

bool CheckpointManager::addNewCheckpoint_UNLOCKED(uint64_t id) {
    // This is just for making sure that the current checkpoint should be closed.
    if (checkpointList.size() > 0 &&
        checkpointList.back()->getState() == CHECKPOINT_OPEN) {
        closeOpenCheckpoint_UNLOCKED(checkpointList.back()->getId());
    }

    getLogger()->log(EXTENSION_LOG_INFO, NULL,
                     "Create a new open checkpoint %llu for vbucket %d.\n",
                     id, vbucketId);

    bool empty = checkpointList.empty() ? true : false;
    Checkpoint *checkpoint = new Checkpoint(stats, id, vbucketId, CHECKPOINT_OPEN);
    // Add a dummy item into the new checkpoint, so that any cursor referring to the actual first
    // item in this new checkpoint can be safely shifted left by 1 if the first item is removed
    // and pushed into the tail.
    queued_item dummyItem(new QueuedItem("dummy_key", 0xffff, queue_op_empty));
    checkpoint->queueDirty(dummyItem, this);

    // This item represents the start of the new checkpoint and is also sent to the slave node.
    queued_item qi = createCheckpointItem(id, vbucketId, queue_op_checkpoint_start);
    checkpoint->queueDirty(qi, this);
    ++numItems;
    checkpointList.push_back(checkpoint);

    if (empty) {
        return true;
    }
    // Move the persistence cursor to the next checkpoint if it already reached to
    // the end of its current checkpoint.
    ++(persistenceCursor.currentPos);
    if (persistenceCursor.currentPos != (*(persistenceCursor.currentCheckpoint))->end()) {
        if ((*(persistenceCursor.currentPos))->getOperation() == queue_op_checkpoint_end) {
            // Skip checkpoint_end meta item that is only used by TAP replication cursors.
            ++(persistenceCursor.offset);
            ++(persistenceCursor.currentPos); // cursor now reaches to the checkpoint end.
        }
    }
    if (persistenceCursor.currentPos == (*(persistenceCursor.currentCheckpoint))->end()) {
        if ((*(persistenceCursor.currentCheckpoint))->getState() == CHECKPOINT_CLOSED) {
            uint64_t chkid = (*(persistenceCursor.currentCheckpoint))->getId();
            if (moveCursorToNextCheckpoint(persistenceCursor)) {
                pCursorPreCheckpointId = chkid;
            } else {
                --(persistenceCursor.currentPos);
            }
        } else {
            // The persistence cursor is already reached to the end of the open checkpoint.
            --(persistenceCursor.currentPos);
        }
    } else {
        --(persistenceCursor.currentPos);
    }

    return true;
}

bool CheckpointManager::addNewCheckpoint(uint64_t id) {
    LockHolder lh(queueLock);
    return addNewCheckpoint_UNLOCKED(id);
}

bool CheckpointManager::closeOpenCheckpoint_UNLOCKED(uint64_t id) {
    if (checkpointList.size() == 0) {
        return false;
    }
    if (id != checkpointList.back()->getId() ||
        checkpointList.back()->getState() == CHECKPOINT_CLOSED) {
        return true;
    }

    getLogger()->log(EXTENSION_LOG_INFO, NULL,
                     "Close the open checkpoint %llu for vbucket %d\n", id, vbucketId);

    // This item represents the end of the current open checkpoint and is sent to the slave node.
    queued_item qi = createCheckpointItem(id, vbucketId, queue_op_checkpoint_end);
    checkpointList.back()->queueDirty(qi, this);
    ++numItems;
    checkpointList.back()->setState(CHECKPOINT_CLOSED);
    return true;
}

bool CheckpointManager::closeOpenCheckpoint(uint64_t id) {
    LockHolder lh(queueLock);
    return closeOpenCheckpoint_UNLOCKED(id);
}

void CheckpointManager::registerPersistenceCursor() {
    LockHolder lh(queueLock);
    assert(checkpointList.size() > 0);
    persistenceCursor.currentCheckpoint = checkpointList.begin();
    persistenceCursor.currentPos = checkpointList.front()->begin();
    checkpointList.front()->registerCursorName(persistenceCursor.name);
}

bool CheckpointManager::registerTAPCursor(const std::string &name, uint64_t checkpointId,
                                          bool closedCheckpointOnly, bool alwaysFromBeginning) {
    LockHolder lh(queueLock);
    return registerTAPCursor_UNLOCKED(name,
                                      checkpointId,
                                      closedCheckpointOnly,
                                      alwaysFromBeginning);
}

bool CheckpointManager::registerTAPCursor_UNLOCKED(const std::string &name,
                                                   uint64_t checkpointId,
                                                   bool closedCheckpointOnly,
                                                   bool alwaysFromBeginning) {
    assert(checkpointList.size() > 0);

    bool found = false;
    std::list<Checkpoint*>::iterator it = checkpointList.begin();
    for (; it != checkpointList.end(); it++) {
        if (checkpointId == (*it)->getId()) {
            found = true;
            break;
        }
    }

    getLogger()->log(EXTENSION_LOG_INFO, NULL,
                     "Register the tap cursor with the name \"%s\" for vbucket %d.\n",
                     name.c_str(), vbucketId);

    // Get the current open_checkpoint_id. The cursor that grabs items from closed checkpoints
    // only walks the checkpoint datastructure until it reaches to the beginning of the
    // checkpoint with open_checkpoint_id. One of the typical use cases is the cursor for the
    // incremental backup client.
    uint64_t open_chk_id = getOpenCheckpointId_UNLOCKED();

    // If the tap cursor exists, remove its name from the checkpoint that is
    // currently referenced by the tap cursor.
    std::map<const std::string, CheckpointCursor>::iterator map_it = tapCursors.find(name);
    if (map_it != tapCursors.end()) {
        (*(map_it->second.currentCheckpoint))->removeCursorName(name);
    }

    if (!found) {
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                         "Checkpoint %llu for vbucket %d doesn't exist in memory. "
                         "Set the cursor with the name \"%s\" to the open checkpoint.\n",
                         checkpointId, vbucketId, name.c_str());
        it = --(checkpointList.end());
        CheckpointCursor cursor(name, it, (*it)->begin(),
                            numItems - ((*it)->getNumItems() + 1), // 1 is for checkpoint start item
                            closedCheckpointOnly, open_chk_id);
        tapCursors[name] = cursor;
        (*it)->registerCursorName(name);
    } else {
        size_t offset = 0;
        std::list<queued_item>::iterator curr;

        getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                         "Checkpoint %llu for vbucket %d exists in memory. "
                         "Set the cursor with the name \"%s\" to the checkpoint %llu\n",
                         checkpointId, vbucketId, name.c_str(), checkpointId);

        if (!alwaysFromBeginning &&
            map_it != tapCursors.end() &&
            (*(map_it->second.currentCheckpoint))->getId() == (*it)->getId()) {
            // If the cursor is currently in the checkpoint to start with, simply start from
            // its current position.
            curr = map_it->second.currentPos;
            offset = map_it->second.offset;
        } else {
            // Set the cursor's position to the begining of the checkpoint to start with
            curr = (*it)->begin();
            std::list<Checkpoint*>::iterator pos = checkpointList.begin();
            for (; pos != it; ++pos) {
                offset += (*pos)->getNumItems() + 2; // 2 is for checkpoint start and end items.
            }
        }

        CheckpointCursor cursor(name, it, curr, offset, closedCheckpointOnly, open_chk_id);
        tapCursors[name] = cursor;
        // Register the tap cursor's name to the checkpoint.
        (*it)->registerCursorName(name);
    }

    return found;
}

bool CheckpointManager::removeTAPCursor(const std::string &name) {
    LockHolder lh(queueLock);

    getLogger()->log(EXTENSION_LOG_INFO, NULL,
                     "Remove the checkpoint cursor with the name \"%s\" from vbucket %d.\n",
                     name.c_str(), vbucketId);

    std::map<const std::string, CheckpointCursor>::iterator it = tapCursors.find(name);
    if (it == tapCursors.end()) {
        return false;
    }

    // We can simply remove the cursor's name from the checkpoint to which it currently belongs,
    // by calling
    // (*(it->second.currentCheckpoint))->removeCursorName(name);
    // However, we just want to do more sanity checks by looking at each checkpoint. This won't
    // cause much overhead because the max number of checkpoints allowed per vbucket is small.
    std::list<Checkpoint*>::iterator cit = checkpointList.begin();
    for (; cit != checkpointList.end(); cit++) {
        (*cit)->removeCursorName(name);
    }

    tapCursors.erase(it);
    return true;
}

uint64_t CheckpointManager::getCheckpointIdForTAPCursor(const std::string &name) {
    LockHolder lh(queueLock);
    std::map<const std::string, CheckpointCursor>::iterator it = tapCursors.find(name);
    if (it == tapCursors.end()) {
        return 0;
    }

    return (*(it->second.currentCheckpoint))->getId();
}

size_t CheckpointManager::getNumOfTAPCursors() {
    LockHolder lh(queueLock);
    return tapCursors.size();
}

size_t CheckpointManager::getNumCheckpoints() {
    LockHolder lh(queueLock);
    return checkpointList.size();
}

std::list<std::string> CheckpointManager::getTAPCursorNames() {
    LockHolder lh(queueLock);
    std::list<std::string> cursor_names;
    std::map<const std::string, CheckpointCursor>::iterator tap_it = tapCursors.begin();
        for (; tap_it != tapCursors.end(); ++tap_it) {
        cursor_names.push_back((tap_it->first));
    }
    return cursor_names;
}

bool CheckpointManager::tapCursorExists(const std::string &name) {
    return tapCursors.find(name) != tapCursors.end();
}

bool CheckpointManager::isCheckpointCreationForHighMemUsage(const RCPtr<VBucket> &vbucket) {
    bool forceCreation = false;
    double memoryUsed = static_cast<double>(stats.getTotalMemoryUsed());
    // pesistence and tap cursors are all currently in the open checkpoint?
    bool allCursorsInOpenCheckpoint =
        (tapCursors.size() + 1) == checkpointList.back()->getNumberOfCursors();

    if (memoryUsed > stats.mem_high_wat &&
        allCursorsInOpenCheckpoint &&
        (checkpointList.back()->getNumItems() >= MIN_CHECKPOINT_ITEMS ||
         checkpointList.back()->getNumItems() == vbucket->ht.getNumItems())) {
        forceCreation = true;
    }
    return forceCreation;
}

size_t CheckpointManager::removeClosedUnrefCheckpoints(const RCPtr<VBucket> &vbucket,
                                                       bool &newOpenCheckpointCreated) {

    // This function is executed periodically by the non-IO dispatcher.
    LockHolder lh(queueLock);
    assert(vbucket);
    uint64_t oldCheckpointId = 0;
    bool canCreateNewCheckpoint = false;
    if (checkpointList.size() < checkpointConfig.getMaxCheckpoints() ||
        (checkpointList.size() == checkpointConfig.getMaxCheckpoints() &&
         checkpointList.front()->getNumberOfCursors() == 0)) {
        canCreateNewCheckpoint = true;
    }
    if (vbucket->getState() == vbucket_state_active &&
        !checkpointConfig.isInconsistentSlaveCheckpoint() &&
        canCreateNewCheckpoint) {

        bool forceCreation = isCheckpointCreationForHighMemUsage(vbucket);
        // Check if this master active vbucket needs to create a new open checkpoint.
        oldCheckpointId = checkOpenCheckpoint_UNLOCKED(forceCreation, true);
    }
    newOpenCheckpointCreated = oldCheckpointId > 0;
    if (oldCheckpointId > 0) {
        // If the persistence cursor reached to the end of the old open checkpoint, move it to
        // the new open checkpoint.
        if ((*(persistenceCursor.currentCheckpoint))->getId() == oldCheckpointId) {
            if (++(persistenceCursor.currentPos) ==
                (*(persistenceCursor.currentCheckpoint))->end()) {
                moveCursorToNextCheckpoint(persistenceCursor);
            } else {
                decrCursorPos_UNLOCKED(persistenceCursor);
            }
        }
        // If any of TAP cursors reached to the end of the old open checkpoint, move them to
        // the new open checkpoint.
        std::map<const std::string, CheckpointCursor>::iterator tap_it = tapCursors.begin();
        for (; tap_it != tapCursors.end(); ++tap_it) {
            CheckpointCursor &cursor = tap_it->second;
            if ((*(cursor.currentCheckpoint))->getId() == oldCheckpointId) {
                if (++(cursor.currentPos) == (*(cursor.currentCheckpoint))->end()) {
                    moveCursorToNextCheckpoint(cursor);
                } else {
                    decrCursorPos_UNLOCKED(cursor);
                }
            }
        }
    }

    if (checkpointConfig.canKeepClosedCheckpoints()) {
        double memoryUsed = static_cast<double>(stats.getTotalMemoryUsed());
        if (memoryUsed < stats.mem_high_wat &&
            checkpointList.size() <= checkpointConfig.getMaxCheckpoints()) {
            return 0;
        }
    }

    size_t numUnrefItems = 0;
    size_t numCheckpointsRemoved = 0;
    std::list<Checkpoint*> unrefCheckpointList;
    std::list<Checkpoint*>::iterator it = checkpointList.begin();
    for (; it != checkpointList.end(); it++) {
        removeInvalidCursorsOnCheckpoint(*it);
        if ((*it)->getNumberOfCursors() > 0) {
            break;
        } else {
            numUnrefItems += (*it)->getNumItems() + 2; // 2 is for checkpoint start and end items.
            ++numCheckpointsRemoved;
            if (checkpointConfig.canKeepClosedCheckpoints() &&
                (checkpointList.size() - numCheckpointsRemoved) <=
                 checkpointConfig.getMaxCheckpoints()) {
                // Collect unreferenced closed checkpoints until the number of checkpoints is
                // equal to the number of max checkpoints allowed.
                ++it;
                break;
            }
        }
    }
    if (numUnrefItems > 0) {
        numItems -= numUnrefItems;
        decrCursorOffset_UNLOCKED(persistenceCursor, numUnrefItems);
        std::map<const std::string, CheckpointCursor>::iterator map_it = tapCursors.begin();
        for (; map_it != tapCursors.end(); ++map_it) {
            decrCursorOffset_UNLOCKED(map_it->second, numUnrefItems);
        }
    }
    unrefCheckpointList.splice(unrefCheckpointList.begin(), checkpointList,
                               checkpointList.begin(), it);
    // If any cursor on a replica vbucket or downstream active vbucket receiving checkpoints from
    // the upstream master is very slow and causes more closed checkpoints in memory,
    // collapse those closed checkpoints into a single one to reduce the memory overhead.
    if (!checkpointConfig.canKeepClosedCheckpoints() &&
        (vbucket->getState() == vbucket_state_replica ||
         (vbucket->getState() == vbucket_state_active &&
          checkpointConfig.isInconsistentSlaveCheckpoint()))) {
        collapseClosedCheckpoints(unrefCheckpointList);
    }
    lh.unlock();

    std::list<Checkpoint*>::iterator chkpoint_it = unrefCheckpointList.begin();
    for (; chkpoint_it != unrefCheckpointList.end(); chkpoint_it++) {
        delete *chkpoint_it;
    }

    return numUnrefItems;
}

void CheckpointManager::removeInvalidCursorsOnCheckpoint(Checkpoint *pCheckpoint) {
    std::list<std::string> invalidCursorNames;
    const std::set<std::string> &cursors = pCheckpoint->getCursorNameList();
    std::set<std::string>::const_iterator cit = cursors.begin();
    for (; cit != cursors.end(); ++cit) {
        // Check it with persistence cursor
        if ((*cit).compare(persistenceCursor.name) == 0) {
            if (pCheckpoint != *(persistenceCursor.currentCheckpoint)) {
                invalidCursorNames.push_back(*cit);
            }
        } else { // Check it with tap cursors
            std::map<const std::string, CheckpointCursor>::iterator mit = tapCursors.find(*cit);
            if (mit == tapCursors.end() || pCheckpoint != *(mit->second.currentCheckpoint)) {
                invalidCursorNames.push_back(*cit);
            }
        }
    }

    std::list<std::string>::iterator it = invalidCursorNames.begin();
    for (; it != invalidCursorNames.end(); ++it) {
        pCheckpoint->removeCursorName(*it);
    }
}

void CheckpointManager::collapseClosedCheckpoints(std::list<Checkpoint*> &collapsedChks) {
    // If there are one open checkpoint and more than one closed checkpoint, collapse those
    // closed checkpoints into one checkpoint to reduce the memory overhead.
    if (checkpointList.size() > 2) {
        std::set<std::string> slowCursors;
        std::set<std::string> fastCursors;
        std::list<Checkpoint*>::iterator lastClosedChk = checkpointList.end();
        --lastClosedChk; --lastClosedChk; // Move to the lastest closed checkpoint.
        fastCursors.insert((*lastClosedChk)->getCursorNameList().begin(),
                           (*lastClosedChk)->getCursorNameList().end());
        std::list<Checkpoint*>::reverse_iterator rit = checkpointList.rbegin();
        ++rit; ++rit;// Move to the second lastest closed checkpoint.
        size_t numDuplicatedItems = 0, numMetaItems = 0;
        for (; rit != checkpointList.rend(); ++rit) {
            size_t numAddedItems = (*lastClosedChk)->mergePrevCheckpoint(*rit);
            numDuplicatedItems += ((*rit)->getNumItems() - numAddedItems);
            numMetaItems += 2; // checkpoint start and end meta items
            slowCursors.insert((*rit)->getCursorNameList().begin(),
                              (*rit)->getCursorNameList().end());
        }
        // Reposition the slow cursors to the beginning of the last closed checkpoint.
        std::set<std::string>::iterator sit = slowCursors.begin();
        for (; sit != slowCursors.end(); ++sit) {
            CheckpointCursor *cursor = NULL;
            if ((*sit).compare(persistenceCursor.name) == 0) { // Reposition persistence cursor
                cursor = &persistenceCursor;
            } else { // Reposition tap cursors
                std::map<const std::string, CheckpointCursor>::iterator mit = tapCursors.find(*sit);
                if (mit != tapCursors.end()) {
                    cursor = &(mit->second);
                }
            }
            if (cursor) {
                cursor->currentCheckpoint = lastClosedChk;
                cursor->currentPos =  (*lastClosedChk)->begin();
                cursor->offset = 0;
                (*lastClosedChk)->registerCursorName(cursor->name);
            }
        }

        numItems -= (numDuplicatedItems + numMetaItems);
        Checkpoint *pOpenCheckpoint = checkpointList.back();
        const std::set<std::string> &openCheckpointCursors = pOpenCheckpoint->getCursorNameList();
        fastCursors.insert(openCheckpointCursors.begin(), openCheckpointCursors.end());
        std::set<std::string>::const_iterator cit = fastCursors.begin();
        // Update the offset of each fast cursor.
        for (; cit != fastCursors.end(); ++cit) {
            if ((*cit).compare(persistenceCursor.name) == 0) {
                decrCursorOffset_UNLOCKED(persistenceCursor, numDuplicatedItems + numMetaItems);
            } else {
                std::map<const std::string, CheckpointCursor>::iterator mit = tapCursors.find(*cit);
                if (mit != tapCursors.end()) {
                    decrCursorOffset_UNLOCKED(mit->second, numDuplicatedItems + numMetaItems);
                }
            }
        }
        collapsedChks.splice(collapsedChks.end(), checkpointList,
                             checkpointList.begin(),  lastClosedChk);
    }
}

bool CheckpointManager::queueDirty(const queued_item &qi, const RCPtr<VBucket> &vbucket) {
    LockHolder lh(queueLock);
    if (vbucket->getState() != vbucket_state_active &&
        checkpointList.back()->getState() == CHECKPOINT_CLOSED) {
        // Replica vbucket might receive items from the master even if the current open checkpoint
        // has been already closed, because some items from the backfill with an invalid token
        // are on the wire even after that backfill thread is closed. Simply ignore those items.
        return false;
    }

    assert(vbucket);
    bool canCreateNewCheckpoint = false;
    if (checkpointList.size() < checkpointConfig.getMaxCheckpoints() ||
        (checkpointList.size() == checkpointConfig.getMaxCheckpoints() &&
         checkpointList.front()->getNumberOfCursors() == 0)) {
        canCreateNewCheckpoint = true;
    }
    if (vbucket->getState() == vbucket_state_active &&
        !checkpointConfig.isInconsistentSlaveCheckpoint() &&
        canCreateNewCheckpoint) {
        // Only the master active vbucket can create a next open checkpoint.
        checkOpenCheckpoint_UNLOCKED(false, true);
    }
    // Note that the creation of a new checkpoint on the replica vbucket will be controlled by TAP
    // mutation messages from the active vbucket, which contain the checkpoint Ids.

    assert(checkpointList.back()->getState() == CHECKPOINT_OPEN);
    size_t numItemsBefore = getNumItemsForPersistence_UNLOCKED();
    if (checkpointList.back()->queueDirty(qi, this) == NEW_ITEM) {
        ++numItems;
    }
    size_t numItemsAfter = getNumItemsForPersistence_UNLOCKED();

    return (numItemsAfter - numItemsBefore) > 0;
}

void CheckpointManager::getAllItemsFromCurrentPosition(CheckpointCursor &cursor,
                                                       uint64_t barrier,
                                                       std::vector<queued_item> &items) {
    while (true) {
        if ( barrier > 0 )  {
            if ((*(cursor.currentCheckpoint))->getId() >= barrier) {
                break;
            }
        }
        while (++(cursor.currentPos) != (*(cursor.currentCheckpoint))->end()) {
            items.push_back(*(cursor.currentPos));
        }
        if ((*(cursor.currentCheckpoint))->getState() == CHECKPOINT_CLOSED) {
            if (!moveCursorToNextCheckpoint(cursor)) {
                --(cursor.currentPos);
                break;
            }
        } else { // The cursor is currently in the open checkpoint and reached to
                 // the end() of the open checkpoint.
            --(cursor.currentPos);
            break;
        }
    }
}

void CheckpointManager::getAllItemsForPersistence(std::vector<queued_item> &items) {
    LockHolder lh(queueLock);
    // Get all the items up to the end of the current open checkpoint.
    getAllItemsFromCurrentPosition(persistenceCursor, 0, items);
    persistenceCursor.offset = numItems;
    pCursorPreCheckpointId = getLastClosedCheckpointId_UNLOCKED();

    getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                     "Grab %ld items through the persistence cursor from vbucket %d.\n",
                     items.size(), vbucketId);
}

void CheckpointManager::getAllItemsForTAPConnection(const std::string &name,
                                                    std::vector<queued_item> &items) {
    LockHolder lh(queueLock);
    std::map<const std::string, CheckpointCursor>::iterator it = tapCursors.find(name);
    if (it == tapCursors.end()) {
        getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                         "The cursor for TAP connection \"%s\" is not found in the checkpoint.\n",
                         name.c_str());
        return;
    }
    getAllItemsFromCurrentPosition(it->second, 0, items);
    it->second.offset = numItems;

    getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                     "Grab %ld items through the tap cursor with name \"%s\" from vbucket %d.\n",
                     items.size(), name.c_str(), vbucketId);
}

queued_item CheckpointManager::nextItem(const std::string &name, bool &isLastMutationItem) {
    LockHolder lh(queueLock);
    isLastMutationItem = false;
    std::map<const std::string, CheckpointCursor>::iterator it = tapCursors.find(name);
    if (it == tapCursors.end()) {
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "The cursor with name \"%s\" is not found in "
                         "the checkpoint of vbucket %d.\n",
                         name.c_str(), vbucketId);
        queued_item qi(new QueuedItem("", 0xffff, queue_op_empty));
        return qi;
    }
    if (checkpointList.back()->getId() == 0) {
        getLogger()->log(EXTENSION_LOG_INFO, NULL,
                         "VBucket %d is still in backfill phase that doesn't allow "
                         " the tap cursor to fetch an item from it's current checkpoint.\n",
                         vbucketId);
        queued_item qi(new QueuedItem("", 0xffff, queue_op_empty));
        return qi;
    }

    CheckpointCursor &cursor = it->second;
    if ((*(it->second.currentCheckpoint))->getState() == CHECKPOINT_CLOSED) {
        return nextItemFromClosedCheckpoint(cursor, isLastMutationItem);
    } else {
        return nextItemFromOpenCheckpoint(cursor, isLastMutationItem);
    }
}

queued_item CheckpointManager::nextItemFromClosedCheckpoint(CheckpointCursor &cursor,
                                                            bool &isLastMutationItem) {
    // The cursor already reached to the beginning of the checkpoint that had "open" state
    // when registered. Simply return an empty item so that the corresponding TAP client
    // can close the connection.
    if (cursor.closedCheckpointOnly &&
        cursor.openChkIdAtRegistration <= (*(cursor.currentCheckpoint))->getId()) {
        queued_item qi(new QueuedItem("", vbucketId, queue_op_empty));
        return qi;
    }

    ++(cursor.currentPos);
    if (cursor.currentPos != (*(cursor.currentCheckpoint))->end()) {
        ++(cursor.offset);
        isLastMutationItem = isLastMutationItemInCheckpoint(cursor);
        return *(cursor.currentPos);
    } else {
        if (!moveCursorToNextCheckpoint(cursor)) {
            --(cursor.currentPos);
            queued_item qi(new QueuedItem("", 0xffff, queue_op_empty));
            return qi;
        }
        if ((*(cursor.currentCheckpoint))->getState() == CHECKPOINT_CLOSED) {
            ++(cursor.currentPos); // Move the cursor to point to the actual first item.
            ++(cursor.offset);
            isLastMutationItem = isLastMutationItemInCheckpoint(cursor);
            return *(cursor.currentPos);
        } else { // the open checkpoint.
            return nextItemFromOpenCheckpoint(cursor, isLastMutationItem);
        }
    }
}

queued_item CheckpointManager::nextItemFromOpenCheckpoint(CheckpointCursor &cursor,
                                                          bool &isLastMutationItem) {
    if (cursor.closedCheckpointOnly) {
        queued_item qi(new QueuedItem("", vbucketId, queue_op_empty));
        return qi;
    }

    ++(cursor.currentPos);
    if (cursor.currentPos != (*(cursor.currentCheckpoint))->end()) {
        ++(cursor.offset);
        isLastMutationItem = isLastMutationItemInCheckpoint(cursor);
        return *(cursor.currentPos);
    } else {
        --(cursor.currentPos);
        queued_item qi(new QueuedItem("", 0xffff, queue_op_empty));
        return qi;
    }
}

void CheckpointManager::clear(vbucket_state_t vbState) {
    LockHolder lh(queueLock);
    std::list<Checkpoint*>::iterator it = checkpointList.begin();
    // Remove all the checkpoints.
    while(it != checkpointList.end()) {
        delete *it;
        ++it;
    }
    checkpointList.clear();
    numItems = 0;
    mutationCounter = 0;

    uint64_t checkpointId = vbState == vbucket_state_active ? 1 : 0;
    // Add a new open checkpoint.
    addNewCheckpoint_UNLOCKED(checkpointId);
    resetCursors();
}

void CheckpointManager::resetCursors() {
    // Reset the persistence cursor.
    persistenceCursor.currentCheckpoint = checkpointList.begin();
    persistenceCursor.currentPos = checkpointList.front()->begin();
    persistenceCursor.offset = 0;
    checkpointList.front()->registerCursorName(persistenceCursor.name);

    // Reset all the TAP cursors.
    std::map<const std::string, CheckpointCursor>::iterator cit = tapCursors.begin();
    for (; cit != tapCursors.end(); ++cit) {
        cit->second.currentCheckpoint = checkpointList.begin();
        cit->second.currentPos = checkpointList.front()->begin();
        cit->second.offset = 0;
        checkpointList.front()->registerCursorName(cit->second.name);
    }
}

void CheckpointManager::resetTAPCursors(const std::list<std::string> &cursors) {
    LockHolder lh(queueLock);
    std::list<std::string>::const_iterator it = cursors.begin();
    for (; it != cursors.end(); it++) {
        registerTAPCursor_UNLOCKED(*it, getOpenCheckpointId_UNLOCKED(), false, true);
    }
}

bool CheckpointManager::moveCursorToNextCheckpoint(CheckpointCursor &cursor) {
    if ((*(cursor.currentCheckpoint))->getState() == CHECKPOINT_OPEN) {
        return false;
    } else if ((*(cursor.currentCheckpoint))->getState() == CHECKPOINT_CLOSED) {
        std::list<Checkpoint*>::iterator currCheckpoint = cursor.currentCheckpoint;
        if (++currCheckpoint == checkpointList.end()) {
            return false;
        }
    }

    // Remove the cursor's name from its current checkpoint.
    (*(cursor.currentCheckpoint))->removeCursorName(cursor.name);
    // Move the cursor to the next checkpoint.
    ++(cursor.currentCheckpoint);
    cursor.currentPos = (*(cursor.currentCheckpoint))->begin();
    // Register the cursor's name to its new current checkpoint.
    (*(cursor.currentCheckpoint))->registerCursorName(cursor.name);
    return true;
}

uint64_t CheckpointManager::checkOpenCheckpoint_UNLOCKED(bool forceCreation, bool timeBound) {
    int checkpoint_id = 0;

    if (checkpointExtension) {
        return checkpoint_id;
    }

    timeBound = timeBound &&
                (ep_real_time() - checkpointList.back()->getCreationTime()) >=
                checkpointConfig.getCheckpointPeriod();
    // Create the new open checkpoint if any of the following conditions is satisfied:
    // (1) force creation due to online update or high memory usage
    // (2) current checkpoint is reached to the max number of items allowed.
    // (3) time elapsed since the creation of the current checkpoint is greater than the threshold
    if (forceCreation ||
        (checkpointConfig.isItemNumBasedNewCheckpoint() &&
         checkpointList.back()->getNumItems() >= checkpointConfig.getCheckpointMaxItems()) ||
        (checkpointList.back()->getNumItems() > 0 && timeBound)) {

        checkpoint_id = checkpointList.back()->getId();
        closeOpenCheckpoint_UNLOCKED(checkpoint_id);
        addNewCheckpoint_UNLOCKED(checkpoint_id + 1);
    }
    return checkpoint_id;
}

bool CheckpointManager::eligibleForEviction(const std::string &key) {
    LockHolder lh(queueLock);
    uint64_t smallest_mid = 0;

    // Get the mutation id of the item pointed by the slowest cursor.
    // This won't cause much overhead as the number of cursors per vbucket is
    // usually bounded to 3 (persistence cursor + 2 replicas).
    const std::string &pkey = (*(persistenceCursor.currentPos))->getKey();
    smallest_mid = (*(persistenceCursor.currentCheckpoint))->getMutationIdForKey(pkey);
    std::map<const std::string, CheckpointCursor>::iterator mit = tapCursors.begin();
    for (; mit != tapCursors.end(); ++mit) {
        const std::string &tkey = (*(mit->second.currentPos))->getKey();
        uint64_t mid = (*(mit->second.currentCheckpoint))->getMutationIdForKey(tkey);
        if (mid < smallest_mid) {
            smallest_mid = mid;
        }
    }

    bool can_evict = true;
    std::list<Checkpoint*>::reverse_iterator it = checkpointList.rbegin();
    for (; it != checkpointList.rend(); ++it) {
        uint64_t mid = (*it)->getMutationIdForKey(key);
        if (mid == 0) { // key doesn't exist in a checkpoint.
            continue;
        }
        if (smallest_mid < mid) { // The slowest cursor is still sitting behind a given key.
            can_evict = false;
            break;
        }
    }

    return can_evict;
}

size_t CheckpointManager::getNumItemsForTAPConnection(const std::string &name) {
    LockHolder lh(queueLock);
    size_t remains = 0;
    std::map<const std::string, CheckpointCursor>::iterator it = tapCursors.find(name);
    if (it != tapCursors.end()) {
        remains = (numItems >= it->second.offset) ? numItems - it->second.offset : 0;
    }
    return remains;
}

void CheckpointManager::decrTapCursorFromCheckpointEnd(const std::string &name) {
    LockHolder lh(queueLock);
    std::map<const std::string, CheckpointCursor>::iterator it = tapCursors.find(name);
    if (it != tapCursors.end() &&
        (*(it->second.currentPos))->getOperation() == queue_op_checkpoint_end) {
        decrCursorOffset_UNLOCKED(it->second, 1);
        decrCursorPos_UNLOCKED(it->second);
    }
}

bool CheckpointManager::isLastMutationItemInCheckpoint(CheckpointCursor &cursor) {
    std::list<queued_item>::iterator it = cursor.currentPos;
    ++it;
    if (it == (*(cursor.currentCheckpoint))->end() ||
        (*it)->getOperation() == queue_op_checkpoint_end) {
        return true;
    }
    return false;
}

void CheckpointManager::checkAndAddNewCheckpoint(uint64_t id) {
    LockHolder lh(queueLock);

    // Ignore CHECKPOINT_START message with ID 0 as 0 is reserved for representing backfill.
    if (id == 0) {
        return;
    }
    // If the replica receives a checkpoint start message right after backfill completion,
    // simply set the current open checkpoint id to the one received from the active vbucket.
    if (checkpointList.back()->getId() == 0) {
        setOpenCheckpointId_UNLOCKED(id);
        resetCursors();
        return;
    }

    std::list<Checkpoint*>::iterator it = checkpointList.begin();
    // Check if a checkpoint exists with ID >= id.
    while (it != checkpointList.end()) {
        if (id <= (*it)->getId()) {
            break;
        }
        ++it;
    }

    if (it == checkpointList.end()) {
        if ((checkpointList.back()->getId() + 1) < id) {
            isCollapsedCheckpoint = true;
            uint64_t oid = getOpenCheckpointId_UNLOCKED();
            lastClosedCheckpointId = oid > 0 ? (oid - 1) : 0;
        } else if ((checkpointList.back()->getId() + 1) == id) {
            isCollapsedCheckpoint = false;
        }
        if (checkpointList.back()->getState() == CHECKPOINT_OPEN &&
            checkpointList.back()->getNumItems() == 0) {
            // If the current open checkpoint doesn't have any items, simply set its id to
            // the one from the master node.
            setOpenCheckpointId_UNLOCKED(id);
            // Reposition all the cursors in the open checkpoint to the begining position
            // so that a checkpoint_start message can be sent again with the correct id.
            const std::set<std::string> &cursors = checkpointList.back()->getCursorNameList();
            std::set<std::string>::const_iterator cit = cursors.begin();
            for (; cit != cursors.end(); ++cit) {
                if ((*cit).compare(persistenceCursor.name) == 0) { // Persistence cursor
                    persistenceCursor.currentPos = checkpointList.back()->begin();
                } else { // TAP cursors
                    std::map<const std::string, CheckpointCursor>::iterator mit =
                        tapCursors.find(*cit);
                    mit->second.currentPos = checkpointList.back()->begin();
                }
            }
        } else {
            closeOpenCheckpoint_UNLOCKED(checkpointList.back()->getId());
            addNewCheckpoint_UNLOCKED(id);
        }
    } else {
        assert(checkpointList.size() > 0);
        std::list<Checkpoint*>::reverse_iterator rit = checkpointList.rbegin();
        ++rit; // Move to the last closed checkpoint.
        size_t numDuplicatedItems = 0, numMetaItems = 0;
        // Collapse all checkpoints.
        for (; rit != checkpointList.rend(); ++rit) {
            size_t numAddedItems = checkpointList.back()->mergePrevCheckpoint(*rit);
            numDuplicatedItems += ((*rit)->getNumItems() - numAddedItems);
            numMetaItems += 2; // checkpoint start and end meta items
            delete *rit;
        }
        numItems -= (numDuplicatedItems + numMetaItems);

        if (checkpointList.size() > 1) {
            checkpointList.erase(checkpointList.begin(), --checkpointList.end());
        }
        assert(checkpointList.size() == 1);

        if (checkpointList.back()->getState() == CHECKPOINT_CLOSED) {
            checkpointList.back()->popBackCheckpointEndItem();
            --numItems;
            checkpointList.back()->setState(CHECKPOINT_OPEN);
        }
        setOpenCheckpointId_UNLOCKED(id);
        resetCursors();
    }
}

bool CheckpointManager::hasNext(const std::string &name) {
    LockHolder lh(queueLock);
    std::map<const std::string, CheckpointCursor>::iterator it = tapCursors.find(name);
    if (it == tapCursors.end() || getOpenCheckpointId_UNLOCKED() == 0) {
        return false;
    }

    bool hasMore = true;
    std::list<queued_item>::iterator curr = it->second.currentPos;
    ++curr;
    if (curr == (*(it->second.currentCheckpoint))->end() &&
        (*(it->second.currentCheckpoint)) == checkpointList.back()) {
        hasMore = false;
    }
    return hasMore;
}

queued_item CheckpointManager::createCheckpointItem(uint64_t id,
                                                    uint16_t vbid,
                                                    enum queue_operation checkpoint_op) {
    assert(checkpoint_op == queue_op_checkpoint_start || checkpoint_op == queue_op_checkpoint_end);
    std::stringstream key;
    if (checkpoint_op == queue_op_checkpoint_start) {
        key << "checkpoint_start";
    } else {
        key << "checkpoint_end";
    }
    queued_item qi(new QueuedItem(key.str(), vbid, checkpoint_op, id));
    return qi;
}

bool CheckpointManager::hasNextForPersistence() {
    LockHolder lh(queueLock);
    bool hasMore = true;
    std::list<queued_item>::iterator curr = persistenceCursor.currentPos;
    ++curr;
    if (curr == (*(persistenceCursor.currentCheckpoint))->end() &&
        (*(persistenceCursor.currentCheckpoint)) == checkpointList.back()) {
        hasMore = false;
    }
    return hasMore;
}

uint64_t CheckpointManager::createNewCheckpoint() {
    LockHolder lh(queueLock);
    if (checkpointList.back()->getNumItems() > 0) {
        uint64_t chk_id = checkpointList.back()->getId();
        closeOpenCheckpoint_UNLOCKED(chk_id);
        addNewCheckpoint_UNLOCKED(chk_id + 1);
    }
    return checkpointList.back()->getId();
}

void CheckpointManager::decrCursorOffset_UNLOCKED(CheckpointCursor &cursor, size_t decr) {
    if (cursor.offset >= decr) {
        cursor.offset -= decr;
    } else {
        cursor.offset = 0;
        getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                         "%s cursor offset is negative. Reset it to 0.",
                         cursor.name.c_str());
    }
}

void CheckpointManager::decrCursorPos_UNLOCKED(CheckpointCursor &cursor) {
    if (cursor.currentPos != (*(cursor.currentCheckpoint))->begin()) {
        --(cursor.currentPos);
    }
}

uint64_t CheckpointManager::getPersistenceCursorPreChkId() {
    LockHolder lh(queueLock);
    return pCursorPreCheckpointId;
}

void CheckpointConfig::addConfigChangeListener(EventuallyPersistentEngine &engine) {
    Configuration &configuration = engine.getConfiguration();
    configuration.addValueChangedListener("chk_period",
                              new CheckpointConfigChangeListener(engine.getCheckpointConfig()));
    configuration.addValueChangedListener("chk_max_items",
                              new CheckpointConfigChangeListener(engine.getCheckpointConfig()));
    configuration.addValueChangedListener("max_checkpoints",
                              new CheckpointConfigChangeListener(engine.getCheckpointConfig()));
    configuration.addValueChangedListener("inconsistent_slave_chk",
                              new CheckpointConfigChangeListener(engine.getCheckpointConfig()));
    configuration.addValueChangedListener("item_num_based_new_chk",
                              new CheckpointConfigChangeListener(engine.getCheckpointConfig()));
    configuration.addValueChangedListener("keep_closed_chks",
                              new CheckpointConfigChangeListener(engine.getCheckpointConfig()));
}

CheckpointConfig::CheckpointConfig(EventuallyPersistentEngine &e) {
    Configuration &config = e.getConfiguration();
    checkpointPeriod = config.getChkPeriod();
    checkpointMaxItems = config.getChkMaxItems();
    maxCheckpoints = config.getMaxCheckpoints();
    inconsistentSlaveCheckpoint = config.isInconsistentSlaveChk();
    itemNumBasedNewCheckpoint = config.isItemNumBasedNewChk();
    keepClosedCheckpoints = config.isKeepClosedChks();
}

bool CheckpointConfig::validateCheckpointMaxItemsParam(size_t checkpoint_max_items) {
    if (checkpoint_max_items < MIN_CHECKPOINT_ITEMS ||
        checkpoint_max_items > MAX_CHECKPOINT_ITEMS) {
        std::stringstream ss;
        ss << "New checkpoint_max_items param value " << checkpoint_max_items
           << " is not ranged between the min allowed value " << MIN_CHECKPOINT_ITEMS
           << " and max value " << MAX_CHECKPOINT_ITEMS;
        getLogger()->log(EXTENSION_LOG_WARNING, NULL, "%s\n", ss.str().c_str());
        return false;
    }
    return true;
}

bool CheckpointConfig::validateCheckpointPeriodParam(size_t checkpoint_period) {
    if (checkpoint_period < MIN_CHECKPOINT_PERIOD ||
        checkpoint_period > MAX_CHECKPOINT_PERIOD) {
        std::stringstream ss;
        ss << "New checkpoint_period param value " << checkpoint_period
           << " is not ranged between the min allowed value " << MIN_CHECKPOINT_PERIOD
           << " and max value " << MAX_CHECKPOINT_PERIOD;
        getLogger()->log(EXTENSION_LOG_WARNING, NULL, "%s\n", ss.str().c_str());
        return false;
    }
    return true;
}

bool CheckpointConfig::validateMaxCheckpointsParam(size_t max_checkpoints) {
    if (max_checkpoints < DEFAULT_MAX_CHECKPOINTS ||
        max_checkpoints > MAX_CHECKPOINTS_UPPER_BOUND) {
        std::stringstream ss;
        ss << "New max_checkpoints param value " << max_checkpoints
           << " is not ranged between the min allowed value " << DEFAULT_MAX_CHECKPOINTS
           << " and max value " << MAX_CHECKPOINTS_UPPER_BOUND;
        getLogger()->log(EXTENSION_LOG_WARNING, NULL, "%s\n", ss.str().c_str());
        return false;
    }
    return true;
}

void CheckpointConfig::setCheckpointPeriod(size_t value) {
    if (!validateCheckpointPeriodParam(value)) {
        value = DEFAULT_CHECKPOINT_PERIOD;
    }
    checkpointPeriod = static_cast<rel_time_t>(value);
}

void CheckpointConfig::setCheckpointMaxItems(size_t value) {
    if (!validateCheckpointMaxItemsParam(value)) {
        value = DEFAULT_CHECKPOINT_ITEMS;
    }
    checkpointMaxItems = value;
}

void CheckpointConfig::setMaxCheckpoints(size_t value) {
    if (!validateMaxCheckpointsParam(value)) {
        value = DEFAULT_MAX_CHECKPOINTS;
    }
    maxCheckpoints = value;
}

void CheckpointManager::addStats(ADD_STAT add_stat, const void *cookie) {
    LockHolder lh(queueLock);
    char buf[256];

    snprintf(buf, sizeof(buf), "vb_%d:open_checkpoint_id", vbucketId);
    add_casted_stat(buf, getOpenCheckpointId_UNLOCKED(), add_stat, cookie);
    snprintf(buf, sizeof(buf), "vb_%d:last_closed_checkpoint_id", vbucketId);
    add_casted_stat(buf, getLastClosedCheckpointId_UNLOCKED(), add_stat, cookie);
    snprintf(buf, sizeof(buf), "vb_%d:num_tap_cursors", vbucketId);
    add_casted_stat(buf, tapCursors.size(), add_stat, cookie);
    snprintf(buf, sizeof(buf), "vb_%d:num_checkpoint_items", vbucketId);
    add_casted_stat(buf, numItems, add_stat, cookie);
    snprintf(buf, sizeof(buf), "vb_%d:num_open_checkpoint_items", vbucketId);
    add_casted_stat(buf, checkpointList.empty() ? 0 : checkpointList.back()->getNumItems(),
                    add_stat, cookie);
    snprintf(buf, sizeof(buf), "vb_%d:num_checkpoints", vbucketId);
    add_casted_stat(buf, checkpointList.size(), add_stat, cookie);
    snprintf(buf, sizeof(buf), "vb_%d:num_items_for_persistence", vbucketId);
    add_casted_stat(buf, getNumItemsForPersistence_UNLOCKED(), add_stat, cookie);
    snprintf(buf, sizeof(buf), "vb_%d:checkpoint_extension", vbucketId);
    add_casted_stat(buf, isCheckpointExtension() ? "true" : "false",
                    add_stat, cookie);

    std::map<const std::string, CheckpointCursor>::iterator tap_it = tapCursors.begin();
    for (; tap_it != tapCursors.end(); ++tap_it) {
        snprintf(buf, sizeof(buf),
                 "vb_%d:%s:cursor_checkpoint_id", vbucketId, tap_it->first.c_str());
        add_casted_stat(buf, (*(tap_it->second.currentCheckpoint))->getId(),
                        add_stat, cookie);
    }
}
