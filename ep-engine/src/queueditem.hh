/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#ifndef QUEUEDITEM_HH
#define QUEUEDITEM_HH 1

#include "common.hh"
#include "item.hh"
#include "stats.hh"

enum queue_operation {
    queue_op_set,
    queue_op_get,
    queue_op_get_meta,
    queue_op_del,
    queue_op_flush,
    queue_op_empty,
    queue_op_commit,
    queue_op_checkpoint_start,
    queue_op_checkpoint_end
};

typedef enum {
    vbucket_del_success,
    vbucket_del_fail,
    vbucket_del_invalid
} vbucket_del_result;

/**
 * Representation of an item queued for persistence or tap.
 */
class QueuedItem : public RCValue {
public:
    QueuedItem(const std::string &k, const uint16_t vb,
               enum queue_operation o, const uint64_t seqno = 1)
        : key(k), seqNum(seqno), queued(ep_current_time()),
          op(static_cast<uint16_t>(o)), vbucket(vb)
    {
        ObjectRegistry::onCreateQueuedItem(this);
    }

    ~QueuedItem() {
        ObjectRegistry::onDeleteQueuedItem(this);
    }

    const std::string &getKey(void) const { return key; }
    uint16_t getVBucketId(void) const { return vbucket; }
    uint32_t getQueuedTime(void) const { return queued; }
    enum queue_operation getOperation(void) const {
        return static_cast<enum queue_operation>(op);
    }

    uint64_t getSeqno() const { return seqNum; }

    void setQueuedTime(uint32_t queued_time) {
        queued = queued_time;
    }

    void setOperation(enum queue_operation o) {
        op = static_cast<uint16_t>(o);
    }

    bool operator <(const QueuedItem &other) const {
        return getVBucketId() == other.getVBucketId() ?
            getKey() < other.getKey() : getVBucketId() < other.getVBucketId();
    }

    size_t size() {
        return sizeof(QueuedItem) + getKey().size();
    }

private:
    std::string key;
    uint64_t seqNum;
    uint32_t queued;
    uint16_t op;
    uint16_t vbucket;

    DISALLOW_COPY_AND_ASSIGN(QueuedItem);
};

typedef SingleThreadedRCPtr<QueuedItem> queued_item;

/**
 * Order QueuedItem objects pointed by shared_ptr by their keys.
 */
class CompareQueuedItemsByKey {
public:
    CompareQueuedItemsByKey() {}
    bool operator()(const queued_item &i1, const queued_item &i2) {
        return i1->getKey() < i2->getKey();
    }
};

/**
 * Order QueuedItem objects by their vbucket ids and keys.
 */
class CompareQueuedItemsByVBAndKey {
public:
    CompareQueuedItemsByVBAndKey() {}
    bool operator()(const queued_item &i1, const queued_item &i2) {
        return i1->getVBucketId() == i2->getVBucketId()
            ? i1->getKey() < i2->getKey()
            : i1->getVBucketId() < i2->getVBucketId();
    }
};

#endif /* QUEUEDITEM_HH */
