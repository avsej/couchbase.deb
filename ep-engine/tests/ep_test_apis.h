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

#ifndef EP_TEST_APIS_H
#define EP_TEST_APIS_H 1

#include <map>
#include <string>

#include <memcached/engine.h>
#include <memcached/engine_testapp.h>

#include "ep-engine/command_ids.h"
#include "item.hh"

#ifdef __cplusplus
extern "C" {
#endif

void decayingSleep(useconds_t *sleepTime);

bool add_response(const void *key, uint16_t keylen, const void *ext,
                  uint8_t extlen, const void *body, uint32_t bodylen,
                  uint8_t datatype, uint16_t status, uint64_t cas,
                  const void *cookie);

void add_stats(const char *key, const uint16_t klen, const char *val,
               const uint32_t vlen, const void *cookie);

protocol_binary_request_header* createPacket(uint8_t opcode,
                                             uint16_t vbid = 0,
                                             uint64_t cas = 0,
                                             const char *ext = NULL,
                                             uint32_t extlen = 0,
                                             const char *key = NULL,
                                             uint32_t keylen = 0,
                                             const char *val = NULL,
                                             uint32_t vallen = 0);



// Basic Operations
ENGINE_ERROR_CODE del(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1, const char *key,
                      uint64_t cas, uint16_t vbucket, const void* cookie = NULL);
void evict_key(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1, const char *key,
               uint16_t vbucketId = 0, const char *msg = NULL,
               bool expectError = false);
void gat(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1, const char* key,
         uint16_t vb, uint32_t exp, bool quiet = false);
bool get_item_info(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1, item_info *info,
                   const char* key, uint16_t vb = 0);
bool get_key(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1, item *i,
             std::string &key);
void getl(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1, const char* key, uint16_t vb,
          uint32_t lock_timeout);
void observe(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1,
             std::map<std::string, uint16_t> obskeys);
protocol_binary_request_header* prepare_get_replica(ENGINE_HANDLE *h,
                                                    ENGINE_HANDLE_V1 *h1,
                                                    vbucket_state_t state,
                                                    bool makeinvalidkey = false);
bool set_param(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1, engine_param_t paramtype,
               const char *param, const char *val);
bool set_vbucket_state(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1,
                       uint16_t vb, vbucket_state_t state);
void start_persistence(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1);
void stop_persistence(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1);
ENGINE_ERROR_CODE store(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1,
                        const void *cookie, ENGINE_STORE_OPERATION op,
                        const char *key, const char *value, item **outitem,
                        uint64_t casIn = 0, uint16_t vb = 0);
ENGINE_ERROR_CODE storeCasVb11(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1,
                               const void *cookie, ENGINE_STORE_OPERATION op,
                               const char *key, const char *value, size_t vlen,
                               uint32_t flags, item **outitem, uint64_t casIn,
                               uint16_t vb);
void touch(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1, const char* key,
           uint16_t vb, uint32_t exp);
void unl(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1, const char* key,
         uint16_t vb, uint64_t cas = 0);
ENGINE_ERROR_CODE verify_key(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1,
                             const char* key, uint16_t vbucket = 0);
bool verify_vbucket_missing(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1,
                            uint16_t vb);
bool verify_vbucket_state(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1, uint16_t vb,
                          vbucket_state_t expected, bool mute = false);

// Checkpoint Operations
void createCheckpoint(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1);
void extendCheckpoint(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1,
                      uint32_t checkpoint_num);
ENGINE_ERROR_CODE checkpointPersistence(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1,
                                        uint64_t checkpoint_id);

// Stats Operations
int get_int_stat(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1, const char *statname,
                 const char *statkey = NULL);
std::string get_str_stat(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1,
                         const char *statname, const char *statkey = NULL);
void verify_curr_items(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1, int exp,
                       const char *msg);
void wait_for_stat_change(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1,
                          const char *stat, int initial);
void wait_for_stat_to_be(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1, const char *stat,
                         int final, const char* stat_key = NULL);
bool wait_for_warmup_complete(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1);
void wait_for_flusher_to_settle(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1);
void wait_for_persisted_value(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1,
                              const char *key, const char *val,
                              uint16_t vbucketId = 0);

// Tap Operations
void changeVBFilter(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1, std::string name,
                    std::map<uint16_t, uint64_t> filtermap);

// VBucket operations
void vbucketDelete(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1, uint16_t vb,
                   const char* args = NULL);

// XDCR Operations
void add_with_meta(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1, const char *key,
                   const size_t keylen, const char *val, const size_t vallen,
                   const uint32_t vb, ItemMetaData *itemMeta);
bool get_meta(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1, const char* key);
void del_with_meta(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1, const char *key,
                   const size_t keylen, const uint32_t vb,
                   ItemMetaData *itemMeta, uint64_t cas_for_delete = 0);
void set_with_meta(ENGINE_HANDLE *h, ENGINE_HANDLE_V1 *h1, const char *key,
                   const size_t keylen, const char *val, const size_t vallen,
                   const uint32_t vb, ItemMetaData *itemMeta,
                   uint64_t cas_for_set);

#ifdef __cplusplus
}
#endif

#endif
