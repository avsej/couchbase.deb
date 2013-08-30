/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2011 Couchbase, Inc.
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
/**
 * This file contains empty macros for all probes to allow compilation
 * on platforms who doesn't support DTrace. Please note that you
 * could always implement this as functions and have them log somewhere
 * else if you like..
 */
#ifndef DUMMY_PROBES_H
#define DUMMY_PROBES_H

#define EP_MUTEX_ACQUIRED(arg0)
#define EP_MUTEX_ACQUIRED_ENABLED() (0)
#define EP_MUTEX_CREATED(arg0)
#define EP_MUTEX_CREATED_ENABLED() (0)
#define EP_MUTEX_DESTROYED(arg0)
#define EP_MUTEX_DESTROYED_ENABLED() (0)
#define EP_MUTEX_RELEASED(arg0)
#define EP_MUTEX_RELEASED_ENABLED() (0)
#define EP_SPINLOCK_ACQUIRED(arg0, arg1)
#define EP_SPINLOCK_ACQUIRED_ENABLED() (0)
#define EP_SPINLOCK_CREATED(arg0)
#define EP_SPINLOCK_CREATED_ENABLED() (0)
#define EP_SPINLOCK_DESTROYED(arg0)
#define EP_SPINLOCK_DESTROYED_ENABLED() (0)
#define EP_SPINLOCK_RELEASED(arg0)
#define EP_SPINLOCK_RELEASED_ENABLED() (0)


#endif /* DUMMY_PROBES_H */
