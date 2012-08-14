/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "config.h"
#include <iostream>

#include "ep_engine.h"
#include "access_scanner.hh"

class ItemAccessVisitor : public VBucketVisitor {
public:
    ItemAccessVisitor(EventuallyPersistentStore &_store, EPStats &_stats) :
        store(_store), stats(_stats), startTime(ep_real_time())
    {
        Configuration &conf = store.getEPEngine().getConfiguration();
        name = conf.getAlogPath();
        prev = name + ".old";
        next = name + ".next";

        log = new MutationLog(next, conf.getAlogBlockSize());
        assert(log != NULL);
        log->open();
        if (!log->isOpen()) {
            getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                             "FATAL: Failed to open access log: %s",
                             next.c_str());
            delete log;
            log = NULL;
        }
    }

    void visit(StoredValue *v) {
        if (log != NULL && v->isReferenced(true, &currentBucket->ht)) {
            if (v->isExpired(startTime) || v->isDeleted()) {
                getLogger()->log(EXTENSION_LOG_INFO, NULL,
                                 "INFO: Skipping expired/deleted item: %s",
                                 v->getKey().c_str());
            } else {
                log->newItem(currentBucket->getId(), v->getKey(), v->getId());
            }
        }
    }

    bool visitBucket(RCPtr<VBucket> &vb) {
        if (log == NULL) {
            return false;
        }

        return VBucketVisitor::visitBucket(vb);
    }

    virtual void complete() {
        if (log != NULL) {
            size_t num_items = log->itemsLogged[ML_NEW];
            log->commit1();
            log->commit2();
            delete log;
            log = NULL;
            ++stats.alogRuns;

            if (num_items == 0) {
                getLogger()->log(EXTENSION_LOG_INFO, NULL,
                                 "The new access log is empty. "
                                 "Delete it without replacing the current access log...\n");
                remove(next.c_str());
                return;
            }

            if (access(prev.c_str(), F_OK) == 0 && remove(prev.c_str()) == -1) {
                getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                 "FATAL: Failed to remove '%s': %s",
                                 prev.c_str(), strerror(errno));
                remove(next.c_str());
            } else if (access(name.c_str(), F_OK) == 0 && rename(name.c_str(), prev.c_str()) == -1) {
                getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                 "FATAL: Failed to rename '%s' to '%s': %s",
                                 name.c_str(), prev.c_str(), strerror(errno));
                remove(next.c_str());
            } else if (rename(next.c_str(), name.c_str()) == -1) {
                getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                 "FATAL: Failed to rename '%s' to '%s': %s",
                                 next.c_str(), name.c_str(), strerror(errno));
                remove(next.c_str());
            }
        }
    }

private:
    EventuallyPersistentStore &store;
    EPStats &stats;
    rel_time_t startTime;
    std::string prev;
    std::string next;
    std::string name;

    MutationLog *log;
};

AccessScanner::AccessScanner(EventuallyPersistentStore &_store, EPStats &st,
                             size_t sleeptime) :
        store(_store), stats(st), sleepTime(sleeptime)
{}

bool AccessScanner::callback(Dispatcher &d, TaskId t) {
    // @todo we should be able to suspend this task to ensure that we're not
    //       running multiple in parallel
    shared_ptr<ItemAccessVisitor> pv(new ItemAccessVisitor(store, stats));
    store.resetAccessScannerTasktime();
    store.visit(pv, "Item access scanner", &d, Priority::ItemPagerPriority);
    d.snooze(t, sleepTime);
    stats.alogTime.set(t->getWaketime().tv_sec);
    return true;
}

std::string AccessScanner::description() {
    return std::string("Generating access log");
}

size_t AccessScanner::startTime() {
    Configuration &cfg = store.getEPEngine().getConfiguration();
    return cfg.getAlogTaskTime();
}
