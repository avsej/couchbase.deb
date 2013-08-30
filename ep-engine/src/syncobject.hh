/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#ifndef SYNCOBJECT_HH
#define SYNCOBJECT_HH 1

#include <stdexcept>
#include <iostream>
#include <sstream>
#include <pthread.h>
#include <sys/time.h>

#include "common.hh"

/**
 * Abstraction built on top of pthread mutexes
 */
class SyncObject : public Mutex {
public:
    SyncObject() : Mutex() {
#ifdef VALGRIND
        // valgrind complains about an uninitialzed memory read
        // if we just initialize the cond with pthread_cond_init.
        memset(&cond, 0, sizeof(cond));
#endif
        if (pthread_cond_init(&cond, NULL) != 0) {
            throw std::runtime_error("MUTEX ERROR: Failed to initialize cond.");
        }
    }

    ~SyncObject() {
        int e;
        if ((e = pthread_cond_destroy(&cond)) != 0) {
            if (e == EINVAL) {
                std::string err = std::strerror(e);
                // cond. object might have already destroyed, just log
                // error and continue.  TODO: platform specific error
                // handling for the case of EINVAL, especially on WIN32
                getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                                 "Warning: Failed to destroy cond. object: %s\n",
                                 err.c_str());
            } else {
                throw std::runtime_error("MUTEX ERROR: Failed to destroy cond.");
            }
        }
    }

    void wait() {
        if (pthread_cond_wait(&cond, &mutex) != 0) {
            throw std::runtime_error("Failed to wait for condition.");
        }
        setHolder(true);
    }

    bool wait(const struct timeval &tv) {
        struct timespec ts;
        ts.tv_sec = tv.tv_sec + 0;
        ts.tv_nsec = tv.tv_usec * 1000;

        switch (pthread_cond_timedwait(&cond, &mutex, &ts)) {
        case 0:
            setHolder(true);
            return true;
        case ETIMEDOUT:
            setHolder(true);
            return false;
        default:
            throw std::runtime_error("Failed timed_wait for condition.");
        }
    }

    bool wait(const double secs) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        advance_tv(tv, secs);
        return wait(tv);
    }

    void notify() {
        if(pthread_cond_broadcast(&cond) != 0) {
            throw std::runtime_error("Failed to broadcast change.");
        }
    }

private:
    pthread_cond_t cond;

    DISALLOW_COPY_AND_ASSIGN(SyncObject);
};

#endif

