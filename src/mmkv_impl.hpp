/*
 *Copyright (c) 2015-2015, yinqiwen <yinqiwen@gmail.com>
 *All rights reserved.
 *
 *Redistribution and use in source and binary forms, with or without
 *modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Redis nor the names of its contributors may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 *THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 *BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 *THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef MMKV_IMPL_HPP_
#define MMKV_IMPL_HPP_

#include <stdio.h>
#include <stdint.h>
#include <vector>
#include "logger_macros.hpp"
#include "memory.hpp"
#include "types.hpp"
#include "mmkv_logger.hpp"
#include "mmkv.hpp"
#include "mmkv_options.hpp"

#define DENSE_TABLE_DELETED_KEY "\t\t\t\t"
namespace mmkv
{
    /* Struct to hold a inclusive/exclusive range spec by score comparison. */
    typedef struct
    {
            long double min, max;
            int minex, maxex; /* are min or max exclusive? */
    } zrangespec;
    typedef std::vector<zrangespec> ZRangeSpecArray;
    struct IteratorCursor;
    class MMKVImpl: public MMKV
    {
        private:
            MemorySegmentManager m_segment;
            bool m_readonly;
            typedef std::vector<MMKVTable*> MMKVTableArray;
            MMKVTableArray m_kvs;
            ExpireInfoSetArray* m_expires;
            DBIDSet* m_dbid_set;
            SpinMutexLock m_kv_table_lock;
            Logger m_logger;

            typedef std::map<uint32_t, PODestructor*> PODestructorTable;
            PODestructorTable m_destructors;

            OpenOptions m_options;

            friend class IteratorCursor;
            friend class Iterator;

            void* Malloc(size_t size);
            void Free(void* p);
            void Lock(bool readonly);
            void Unlock(bool readonly);
            bool IsLocked(bool readonly);
            int GetPOD(DBID db, const Data& key, bool created_if_notexist, uint32_t expected_type, Data& v);
            int RegisterPODDestructor(uint32_t expected_type, PODestructor* des);
            PODestructor* GetPODDestructor(uint32_t expected_type);
            Allocator<char> GetCharAllocator();

            bool IsExpired(DBID db, const Data& key, const Object& obj);
            void DestroyObjectContent(const Object& obj);
            Object CloneStrObject(const Object& obi);

            void CreateHLLObject(Object& obj);
            int HLLSparseToDense(Object& o);
            int HLLSparseAdd(Object& o, unsigned char *ele, size_t elesize);
            int HLLAdd(Object& o, unsigned char *ele, size_t elesize);

            void AssignScoreValue(ScoreValue& sv, long double score, const Data& value);
            MMKVTable* GetMMKVTable(DBID db, bool create_if_notexist);
            int DeleteMMKVTable(DBID db);
            ExpireInfoSet* GetDBExpireInfo(DBID db, bool create_ifnotexist);
            void ClearTTL(DBID db, const Object& key, Object& value);
            void SetTTL(DBID db, const Object& key, Object& value, uint64_t ttl);
            uint64_t GetTTL(DBID db, const Object& key, const Object& value);

            const Object* FindMMValue(MMKVTable* table, const Data& key) const;
            Object& FindOrCreateStringValue(MMKVTable* table, const Data& key, const Data& value, bool& created);
            int GenericSet(MMKVTable* table, DBID db, const Data& key, const Data& value, int32_t ex, int64_t px,
                    int8_t nx_xx, bool replace = false);
            int GenericGet(MMKVTable* table, DBID db, const Data& key, std::string& value);
            int GenericDelValue(const Object& v);
            int GenericDelValue(uint32_t type, void* p);
            int GenericDel(MMKVTable* table, DBID db, const Object& key);
            int GenericInsertValue(MMKVTable* table, const Data& key, Object& v, bool replace);
            int GenericMoveKey(DBID src_db, const Data& src_key, DBID dest_db, const Data& dest_key, bool nx);

            int GenericSInterDiffUnion(DBID db, int op, const DataArray& keys, const Data* dest,
                    const StringArrayResult* results);

            int GenericZSetInterUnion(DBID db, int op, const Data& destination, const DataArray& keys,
                    const WeightArray& weights, const std::string& aggregate);
            int ReOpen(bool lock);
            int EnsureWritableValueSpace(size_t space_size = 0);
            int GetValueByPattern(MMKVTable* table, const std::string& pattern, const Object& subst, Object& value);
            bool MatchValueByPattern(MMKVTable* table, const std::string& pattern, const std::string& value_pattern,
                    Object& subst);
            int UpdateZSetScore(ZSet& zset, const Object& value, long double score, long double new_score);
            int GeoSearchWithMinLimit(DBID db, const Data& key, const GeoSearchOptions& options, int coord_type,
                    long double x, long double y, int min_limit, const StringArrayResult& results);

            int IncrementalRehash();
            int RemoveExpiredKeys();
        public:
            MMKVImpl();
            MemorySegmentManager& GetMemoryManager()
            {
                return m_segment;
            }
            int Open(const OpenOptions& open_options);

            size_t MSpaceUsed();

            template<typename T>
            ConstructorProxy<T> GetObject(DBID db, const Data& key, uint32_t expected_type, bool create_if_notexist,
                    int& err, bool* created = NULL)
            {
                if (NULL != created)
                {
                    *created = false;
                }
                err = 0;
                ConstructorProxy<T> proxy;
                MMKVTable* kv = GetMMKVTable(db, create_if_notexist);
                if (NULL == kv)
                {
                    err = ERR_DB_NOT_EXIST;
                    return proxy;
                }
                Object tmpkey(key, false);
                if (create_if_notexist)
                {
                    std::pair<MMKVTable::iterator, bool> ret = kv->insert(MMKVTable::value_type(tmpkey, Object()));
                    //Object& value_data = const_cast<Object&>(ret.first.value());
                    Object& value_data = ret.first->second;
                    if (ret.second)
                    {
                        m_segment.AssignObjectValue(const_cast<Object&>(ret.first->first), key, false);
//                        if (ret.second)
//                        {
//                            m_segment.AssignObjectValue(const_cast<Object&>(ret.first->first), key, false);
//                        }
//                        else
//                        {
//                            GenericDelValue(value_data);
//                        }
                        m_segment.ObjectMakeRoom(value_data, sizeof(T));
                        value_data.type = expected_type;
                        proxy.invoke_constructor = true;
                        proxy.ptr = (T*) value_data.RawValue();
                        if (NULL != created)
                        {
                            *created = true;
                        }
                    }
                    else
                    {
                        if (value_data.type != expected_type)
                        {
                            err = ERR_INVALID_TYPE;
                            return proxy;
                        }
                        proxy.ptr = (T*) (ret.first->second.RawValue());
                        proxy.invoke_constructor = false;
                    }
                }
                else
                {
                    MMKVTable::iterator found = kv->find(tmpkey);
                    if (found != kv->end())
                    {
                        Object& value_data = found->second;
                        if (IsExpired(db, key, value_data))
                        {
                            err = ERR_ENTRY_NOT_EXIST;
                            return proxy;
                        }
                        if (value_data.type == expected_type)
                        {
                            proxy.invoke_constructor = false;
                            proxy.ptr = (T*) (value_data.RawValue());
                        }
                        else
                        {
                            err = ERR_INVALID_TYPE;
                        }
                    }
                    else
                    {
                        err = ERR_ENTRY_NOT_EXIST;
                    }
                }
                return proxy;
            }

            /*
             * keys' operations
             */
            int Del(DBID db, const DataArray& keys);
            int Exists(DBID db, const Data& key);
            int Expire(DBID db, const Data& key, uint32_t secs);
            int Keys(DBID db, const std::string& pattern, const StringArrayResult& keys);
            int Move(DBID db, const Data& key, DBID destdb);
            int PExpire(DBID db, const Data& key, uint64_t milliseconds);
            int PExpireat(DBID db, const Data& key, uint64_t milliseconds_timestamp);
            int64_t PTTL(DBID db, const Data& key);
            int RandomKey(DBID db, std::string& key);
            int Rename(DBID db, const Data& key, const Data& new_key);
            int RenameNX(DBID db, const Data& key, const Data& new_key);
            int64_t Scan(DBID db, int64_t cursor, const std::string& pattern, int32_t limit_count,
                    const StringArrayResult& result);
            int64_t TTL(DBID db, const Data& key);
            int Persist(DBID db, const Data& key);
            int Type(DBID db, const Data& key);
            int Sort(DBID db, const Data& key, const std::string& by, int limit_offset, int limit_count,
                    const StringArray& get_patterns, bool desc, bool alpha_sort, const Data& destination_key,
                    const StringArrayResult& results);

            /*
             * string's operations
             */
            int Append(DBID db, const Data& key, const Data& value);
            int BitCount(DBID db, const Data& key, int start = 0, int end = -1);
            int BitOP(DBID db, const std::string& op, const Data& dest_key, const DataArray& keys);
            int BitPos(DBID db, const Data& key, uint8_t bit, int start = 0, int end = -1);
            int Decr(DBID db, const Data& key, int64_t& new_val);
            int DecrBy(DBID db, const Data& key, int64_t decrement, int64_t& new_val);
            int Get(DBID db, const Data& key, std::string& value);
            int GetBit(DBID db, const Data& key, int offset);
            int GetRange(DBID db, const Data& key, int start, int end, std::string& value);
            int GetSet(DBID db, const Data& key, const Data& value, std::string& old_value);
            int Incr(DBID db, const Data& key, int64_t& new_val);
            int IncrBy(DBID db, const Data& key, int64_t increment, int64_t& new_val);
            int IncrByFloat(DBID db, const Data& key, long double increment, long double& new_val);
            int MGet(DBID db, const DataArray& keys, const StringArrayResult& vals, BooleanArray* get_flags);
            int MSet(DBID db, const DataPairArray& key_vals);
            int MSetNX(DBID db, const DataPairArray& key_vals);
            int PSetNX(DBID db, const Data& key, int64_t milliseconds, const Data& value);
            /*
             * EX seconds -- Set the specified expire time, in seconds.
             * PX milliseconds -- Set the specified expire time, in milliseconds.
             * NX(nx_xx = 0) -- Only set the key if it does not already exist.
             * XX(nx_xx = 1) -- Only set the key if it already exist.
             */
            int Set(DBID db, const Data& key, const Data& value, int32_t ex = -1, int64_t px = -1, int8_t nx_xx = -1);
            int SetBit(DBID db, const Data& key, int offset, uint8_t value);
            int SetEX(DBID db, const Data& key, int32_t secs, const Data& value);
            int SetNX(DBID db, const Data& key, const Data& value);
            int SetRange(DBID db, const Data& key, int offset, const Data& value);
            int Strlen(DBID db, const Data& key);

            /*
             * hash's operations
             */
            int HDel(DBID db, const Data& key, const DataArray& fields);
            int HExists(DBID db, const Data& key, const Data& field);
            int HGet(DBID db, const Data& key, const Data& field, std::string& val);
            int HGetAll(DBID db, const Data& key, const StringArrayResult& vals);
            int HIncrBy(DBID db, const Data& key, const Data& field, int64_t increment, int64_t& new_val);
            int HIncrByFloat(DBID db, const Data& key, const Data& field, long double increment, long double& new_val);
            int HKeys(DBID db, const Data& key, const StringArrayResult& fields);
            int HLen(DBID db, const Data& key);
            int HMGet(DBID db, const Data& key, const DataArray& fields, const StringArrayResult& vals, BooleanArray* get_flag);
            int HMSet(DBID db, const Data& key, const DataPairArray& field_vals);
            int64_t HScan(DBID db, const Data& key, int64_t cursor, const std::string& pattern, int32_t limit_count,
                    const StringArrayResult& results);
            int HSet(DBID db, const Data& key, const Data& field, const Data& val, bool nx = false);
            int HStrlen(DBID db, const Data& key, const Data& field);
            int HVals(DBID db, const Data& key, const StringArrayResult& vals);

            /*
             * hyperloglog's operations
             */
            int PFAdd(DBID db, const Data& key, const DataArray& elements);
            int PFCount(DBID db, const DataArray& keys);
            int PFMerge(DBID db, const Data& destkey, const DataArray& sourcekeys);

            /*
             * list's operations
             */
            int LIndex(DBID db, const Data& key, int index, std::string& val);
            int LInsert(DBID db, const Data& key, bool before_ot_after, const Data& pivot, const Data& val);
            int LLen(DBID db, const Data& key);
            int LPop(DBID db, const Data& key, std::string& val);
            int LPush(DBID db, const Data& key, const DataArray& vals, bool nx = false);
            int LRange(DBID db, const Data& key, int start, int stop, const StringArrayResult& vals);
            int LRem(DBID db, const Data& key, int count, const Data& val);
            int LSet(DBID db, const Data& key, int index, const Data& val);
            int LTrim(DBID db, const Data& key, int start, int stop);
            int RPop(DBID db, const Data& key, std::string& val);
            int RPopLPush(DBID db, const Data& source, const Data& destination, std::string& pop_value);
            int RPush(DBID db, const Data& key, const DataArray& vals, bool nx = false);

            /*
             * set's operations
             */
            int SAdd(DBID db, const Data& key, const DataArray& elements);
            int SCard(DBID db, const Data& key);
            int SDiff(DBID db, const DataArray& keys, const StringArrayResult& diffs);
            int SDiffStore(DBID db, const Data& destination, const DataArray& keys);
            int SInter(DBID db, const DataArray& keys, const StringArrayResult& inters);
            int SInterStore(DBID db, const Data& destination, const DataArray& keys);
            int SIsMember(DBID db, const Data& key, const Data& member);
            int SMembers(DBID db, const Data& key, const StringArrayResult& members);
            int SMove(DBID db, const Data& source, const Data& destination, const Data& member);
            int SPop(DBID db, const Data& key, const StringArrayResult& members, int count = 1);
            int SRandMember(DBID db, const Data& key, const StringArrayResult& members, int count = 1);
            int SRem(DBID db, const Data& key, const DataArray& members);
            int64_t SScan(DBID db, const Data& key, int64_t cursor, const std::string& pattern, int32_t limit_count,
                    const StringArrayResult& results);
            int SUnion(DBID db, const DataArray& keys, const StringArrayResult& unions);
            int SUnionStore(DBID db, const Data& destination, const DataArray& keys);

            /*
             * zset's operations
             */
            int ZAdd(DBID db, const Data& key, const ScoreDataArray& vals, bool nx = false, bool xx = false, bool ch =
                    false, bool incr = false);
            int ZCard(DBID db, const Data& key);
            int ZCount(DBID db, const Data& key, const std::string& min, const std::string& max);
            int ZIncrBy(DBID db, const Data& key, long double increment, const Data& member, long double& new_score);
            int ZLexCount(DBID db, const Data& key, const std::string& min, const std::string& max);
            int ZRange(DBID db, const Data& key, int start, int stop, bool with_scores, const StringArrayResult& vals);
            int ZRangeByLex(DBID db, const Data& key, const std::string& min, const std::string& max, int limit_offset,
                    int limit_count, const StringArrayResult& vals);
            int ZRangeByScore(DBID db, const Data& key, const std::string& min, const std::string& max,
                    bool with_scores, int limit_offset, int limit_count, const StringArrayResult& vals);
            int ZRank(DBID db, const Data& key, const Data& member);
            int ZRem(DBID db, const Data& key, const DataArray& members);
            int ZRemRangeByLex(DBID db, const Data& key, const std::string& min, const std::string& max);
            int ZRemRangeByRank(DBID db, const Data& key, int start, int stop);
            int ZRemRangeByScore(DBID db, const Data& key, const std::string& min, const std::string& max);
            int ZRevRange(DBID db, const Data& key, int start, int stop, bool with_scores,
                    const StringArrayResult& vals);
            int ZRevRangeByLex(DBID db, const Data& key, const std::string& min, const std::string& max,
                    int limit_offset, int limit_count, const StringArrayResult& vals);
            int ZRevRangeByScore(DBID db, const Data& key, const std::string& min, const std::string& max,
                    bool with_scores, int limit_offset, int limit_count, const StringArrayResult& vals);
            int ZRevRank(DBID db, const Data& key, const Data& member);
            int ZScore(DBID db, const Data& key, const Data& member, long double& score);
            int64_t ZScan(DBID db, const Data& key, int64_t cursor, const std::string& pattern, int32_t limit_count,
                    const StringArrayResult& results);
            int ZInterStore(DBID db, const Data& destination, const DataArray& keys, const WeightArray& weights,
                    const std::string& aggregate);
            int ZUnionStore(DBID db, const Data& destination, const DataArray& keys, const WeightArray& weights,
                    const std::string& aggregate);

            int GeoAdd(DBID db, const Data& key, const Data& coord_type, const GeoPointArray& points);
            int GeoSearch(DBID db, const Data& key, const GeoSearchOptions& options, const StringArrayResult& results);

            int64_t DBSize(DBID db);
            int FlushDB(DBID db);
            int FlushAll();
            int GetAllDBInfo(DBInfoArray& dbs);

            int Routine();

            int Backup(const std::string& path);
            int Restore(const std::string& from_file);
            //int Restore(const std::string& backup_dir, const std::string& to_dir);
            //bool CompareDataStore(const std::string& dir);
            int EnsureWritableSpace(size_t space_size);
            Iterator* NewIterator();
            ~MMKVImpl();
    };

}

#endif /* MMKV_HPP_ */
