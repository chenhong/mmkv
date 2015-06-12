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
#include "lock_guard.hpp"
#include "mmkv_impl.hpp"
#include "containers.hpp"
#include <limits.h>
#include <float.h>

#define OP_DIFF 1
#define OP_UNION 2
#define OP_INTER 3

#define AGGREGATE_SUM 1
#define AGGREGATE_MIN 2
#define AGGREGATE_MAX 3
namespace mmkv
{
    void MMKVImpl::AssignScoreValue(ScoreValue& sv, double score, const Data& value)
    {
        AssignObjectContent(sv.value, value, false);
        sv.score = score;
        return;
    }
    static inline int update_zset_score(ZSet& zset, const Object& value, double score, double new_score)
    {
        ScoreValue tmp;
        tmp.value = value;
        tmp.score = score;
        SortedSet::iterator found = zset.set.find(tmp);
        if (found == zset.set.end())
        {
            return -1;
        }
        ScoreValue old_value = *found;
        zset.set.erase(found);
        old_value.score = new_score;
        zset.set.insert(old_value);
        return 0;
    }

    int MMKVImpl::ZAdd(DBID db, const Data& key, const ScoreDataArray& vals, bool nx, bool xx, bool ch, bool incr)
    {
        if (m_readonly)
        {
            return ERR_PERMISSION_DENIED;
        }
        int err = 0;
        Allocator<char> allocator = m_segment.ValueAllocator<char>();
        RWLockGuard<MemorySegmentManager, WRITE_LOCK> keylock_guard(m_segment);
        ZSet* zset = GetObject<ZSet>(db, key, V_TYPE_ZSET, xx ? false : true, err)(allocator);
        if (IS_NOT_EXISTS(err))
        {
            return 0;
        }
        if (0 != err)
        {
            return err;
        }
        int modified = 0;
        for (size_t i = 0; i < vals.size(); i++)
        {
            Object tmpk(vals[i].value);
            std::pair<StringDoubleTable::iterator, bool> ret = zset->scores.insert(
                    StringDoubleTable::value_type(tmpk, vals[i].score));
            if (ret.second)
            {
                if (xx)
                {
                    zset->scores.erase(ret.first);
                    continue;
                }
                ScoreValue fv;
                AssignScoreValue(fv, vals[i].score, vals[i].value);
                const_cast<Object&>(ret.first.key()) = fv.value;
                zset->set.insert(fv);
                modified++;
            }
            else
            {
                if (nx)
                {
                    continue;
                }
                double new_score = vals[i].score;
                if (incr)
                {
                    new_score += ret.first.value();
                }
                else
                {
                    if (ret.first.value() == vals[i].score)
                    {
                        continue;
                    }
                }

                if (ch)
                {
                    modified++;
                }
                if (0 == update_zset_score(*zset, vals[i].value, ret.first.value(), new_score))
                {
                    ret.first.value(new_score);
                }
                else
                {
                    ABORT("Can not replace old zset element score.");
                }
            }
        }
        return modified;
    }
    int MMKVImpl::ZCard(DBID db, const Data& key)
    {
        int err = 0;
        RWLockGuard<MemorySegmentManager, READ_LOCK> keylock_guard(m_segment);
        SortedSet* zset = GetObject<SortedSet>(db, key, V_TYPE_ZSET, false, err)();
        if (NULL == zset || 0 != err)
        {
            return err;
        }
        return zset->size();
    }
    /* Struct to hold a inclusive/exclusive range spec by score comparison. */
    typedef struct
    {
            double min, max;
            int minex, maxex; /* are min or max exclusive? */
    } zrangespec;
    /* Populate the rangespec according to the objects min and max. */
    static int zslParseRange(const std::string& min, const std::string& max, zrangespec *spec)
    {
        char *eptr;
        spec->minex = spec->maxex = 0;

        /* Parse the min-max interval. If one of the values is prefixed
         * by the "(" character, it's considered "open". For instance
         * ZRANGEBYSCORE zset (1.5 (2.5 will match min < x < max
         * ZRANGEBYSCORE zset 1.5 2.5 will instead match min <= x <= max */
        if (min[0] == '(')
        {
            spec->min = strtod(min.data() + 1, &eptr);
            if (eptr[0] != '\0' || isnan(spec->min))
                return ERR_INVALID_MIN_MAX;
            spec->minex = 1;
        }
        else
        {
            if (strcasecmp(min.data(), "-inf") == 0)
            {
                spec->min = -DBL_MAX;
            }
            else
            {
                spec->min = strtod(min.data(), &eptr);
                if (eptr[0] != '\0' || isnan(spec->min))
                    return ERR_INVALID_MIN_MAX;
            }

        }
        if (max[0] == '(')
        {
            spec->max = strtod(max.data() + 1, &eptr);
            if (eptr[0] != '\0' || isnan(spec->max))
                return ERR_INVALID_MIN_MAX;
            spec->maxex = 1;
        }
        else
        {
            if (strcasecmp(max.data(), "+inf") == 0)
            {
                spec->max = DBL_MAX;
            }
            else
            {
                spec->max = strtod(max.data(), &eptr);
                if (eptr[0] != '\0' || isnan(spec->max))
                    return ERR_INVALID_MIN_MAX;
            }
        }
        if (spec->min > spec->max)
        {
            return ERR_INVALID_MIN_MAX;
        }
        return 0;
    }
    static inline size_t btree_rank(SortedSet& set, SortedSet::iterator& it)
    {
        if (it == set.end())
        {
            return set.size() - 1;
        }
        if (it == set.begin())
        {
            return 0;
        }
        ScoreValue& match_value = *it;
        size_t rank = 0;
        if (set.size() <= 32)
        {
            SortedSet::iterator bit = set.begin();
            while (bit != it)
            {
                bit++;
                rank++;
            }
        }
        else
        {
            size_t low = 0;
            size_t high = set.size() - 1;
            SortedSet::iterator last_it = set.end();
            while (low <= high)
            {
                size_t mid = low + (high - low) / 2;
                SortedSet::iterator cit = set.begin();
                cit.increment_by(mid);
                ScoreValue& tmp = *cit;
                int cmpret = tmp.Compare(match_value);
                if (cmpret == 0)
                    return mid;
                else if (cmpret > 0)
                    high = mid - 1;
                else
                    low = mid + 1;
            }
        }
        return rank;
    }
    int MMKVImpl::ZCount(DBID db, const Data& key, const std::string& min, const std::string& max)
    {
        zrangespec spec;
        int err = zslParseRange(min, max, &spec);
        if (0 != err)
        {
            return err;
        }
        RWLockGuard<MemorySegmentManager, READ_LOCK> keylock_guard(m_segment);
        ZSet* zset = GetObject<ZSet>(db, key, V_TYPE_ZSET, false, err)();
        if (IS_NOT_EXISTS(err))
        {
            return 0;
        }
        if (0 != err)
        {
            return err;
        }
        ScoreValue min_sv, max_sv;
        min_sv.score = spec.min;
        max_sv.score = spec.max;
        SortedSet::iterator min_it = zset->set.lower_bound(min_sv);
        SortedSet::iterator max_it = zset->set.lower_bound(max_sv);
        while (spec.minex && min_it != zset->set.end())
        {
            if (min_it->score == spec.min)
            {
                min_it++;
            }
            else
            {
                break;
            }
        }
        while (spec.maxex && max_it != zset->set.end())
        {
            if (max_it->score == spec.max)
            {
                max_it--;
            }
            else
            {
                break;
            }
        }
        int min_rank = btree_rank(zset->set, min_it);
        int max_rank = btree_rank(zset->set, max_it);
        return max_rank - min_rank + 1;
    }
    int MMKVImpl::ZIncrBy(DBID db, const Data& key, double increment, const Data& member, double& new_score)
    {
        if (m_readonly)
        {
            return ERR_PERMISSION_DENIED;
        }
        int err = 0;
        new_score = 0;
        Allocator<char> allocator = m_segment.ValueAllocator<char>();
        RWLockGuard<MemorySegmentManager, WRITE_LOCK> keylock_guard(m_segment);
        ZSet* zset = GetObject<ZSet>(db, key, V_TYPE_ZSET, true, err)(allocator);
        if (0 != err)
        {
            return err;
        }
        Object tmpk(member);
        std::pair<StringDoubleTable::iterator, bool> ret = zset->scores.insert(
                StringDoubleTable::value_type(tmpk, increment));
        if (ret.second)
        {
            ScoreValue fv;
            AssignScoreValue(fv, increment, member);
            const_cast<Object&>(ret.first.key()) = fv.value;
            zset->set.insert(fv);
            new_score = increment;
        }
        else
        {
            update_zset_score(*zset, member, ret.first.value(), ret.first.value() + increment);
            ret.first.value(ret.first.value() + increment);
            new_score = ret.first.value();
        }
        return 0;
    }
    /* Struct to hold an inclusive/exclusive range spec by lexicographic comparison. */
    typedef struct
    {
            std::string min, max; /* May be set to shared.(minstring|maxstring) */
            int minex, maxex; /* are min or max exclusive? */
            int min_empty_type;
            int max_empty_type;
    } zlexrangespec;
    /* ------------------------ Lexicographic ranges ---------------------------- */

    /* Parse max or min argument of ZRANGEBYLEX.
     * (foo means foo (open interval)
     * [foo means foo (closed interval)
     * - means the min string possible
     * + means the max string possible
     *
     * If the string is valid the *dest pointer is set to the redis object
     * that will be used for the comparision, and ex will be set to 0 or 1
     * respectively if the item is exclusive or inclusive. REDIS_OK will be
     * returned.
     *
     * If the string is not a valid range REDIS_ERR is returned, and the value
     * of *dest and *ex is undefined. */
    int zslParseLexRangeItem(const std::string& item, std::string& dest, int *ex, int* empty_type)
    {
        const char *c = item.c_str();
        *empty_type = 0;
        switch (c[0])
        {
            case '+':
            {
                if (c[1] != '\0')
                    return ERR_INVALID_MIN_MAX;
                *ex = 0;
                *empty_type = 1;
                return 0;
            }
            case '-':
            {
                if (c[1] != '\0')
                    return ERR_INVALID_MIN_MAX;
                *ex = 0;
                *empty_type = -1;
                return 0;
            }
            case '(':
            {
                *ex = 1;
                dest = c + 1;
                return 0;
            }
            case '[':
            {
                *ex = 0;
                dest = c + 1;
                return 0;
            }
            default:
            {
                return ERR_INVALID_MIN_MAX;
            }
        }
    }
    static int zslParseLexRange(const std::string& min, const std::string& max, zlexrangespec *spec)
    {
        int err = zslParseLexRangeItem(min, spec->min, &spec->minex, &spec->min_empty_type);
        if (0 != err)
        {
            return err;
        }
        err = zslParseLexRangeItem(max, spec->max, &spec->maxex, &spec->max_empty_type);
        if (0 != err)
        {
            return err;
        }
        if (spec->min > spec->max)
        {
            return ERR_INVALID_MIN_MAX;
        }
        if (spec->min_empty_type == 1 || spec->max_empty_type == -1)
        {
            return ERR_INVALID_MIN_MAX;
        }
        return 0;
    }

    int MMKVImpl::ZLexCount(DBID db, const Data& key, const std::string& min, const std::string& max)
    {
        zlexrangespec range;
        int count = 0;
        int err = 0;
        /* Parse the range arguments */
        if ((err = zslParseLexRange(min, max, &range)) != 0)
        {
            return err;
        }
        RWLockGuard<MemorySegmentManager, READ_LOCK> keylock_guard(m_segment);
        ZSet* zset = GetObject<ZSet>(db, key, V_TYPE_ZSET, false, err)();
        if (NULL == zset || 0 != err)
        {
            return err;
        }
        if (zset->set.empty())
        {
            return 0;
        }
        ScoreValue sv;
        sv.score = zset->set.begin()->score;
        sv.value = range.min;

        Object min_cstr(range.min);
        Object max_cstr(range.min);

        SortedSet::iterator min_it = range.min_empty_type == -1 ? zset->set.begin() : zset->set.lower_bound(sv);
        while (min_it != zset->set.end())
        {
            ScoreValue& tmp = *min_it;
            if (range.minex && min_cstr == tmp.value)
            {
                min_it++;
            }
            else
            {
                break;
            }
        }
        sv.value = range.max;
        SortedSet::iterator max_it = range.max_empty_type == 1 ? zset->set.end() : zset->set.lower_bound(sv);
        if (max_it == zset->set.end())
        {
            max_it--;
        }
        while (max_it != zset->set.begin())
        {
            ScoreValue& tmp = *max_it;
            if (range.maxex && max_cstr == tmp.value)
            {
                max_it--;
            }
            else
            {
                break;
            }
        }
        int min_rank = btree_rank(zset->set, min_it);
        int max_rank = btree_rank(zset->set, max_it);
        return max_rank - min_rank + 1;
    }
    int MMKVImpl::ZRange(DBID db, const Data& key, int start, int end, bool with_scores, StringArray& vals)
    {
        int err;
        vals.clear();
        RWLockGuard<MemorySegmentManager, READ_LOCK> keylock_guard(m_segment);
        ZSet* zset = GetObject<ZSet>(db, key, V_TYPE_ZSET, false, err)();
        if (IS_NOT_EXISTS(err))
        {
            return 0;
        }
        if (0 != err)
        {
            return err;
        }
        int llen = zset->set.size();
        if (start < 0)
            start = llen + start;
        if (end < 0)
            end = llen + end;
        if (start < 0)
            start = 0;
        if (end < 0)
        {
            return ERR_OFFSET_OUTRANGE;
        }
        /* Invariant: start >= 0, so this test will be true when end < 0.
         * The range is empty when start > end or start >= length. */
        if (start > end || start >= llen)
        {
            return 0;
        }
        if (end >= llen)
            end = llen - 1;
        SortedSet::iterator it = zset->set.begin();
        it.increment_by(start);
        for (int i = start; i <= end; i++, it++)
        {
            ScoreValue& sv = *it;
            std::string tmp;
            sv.value.ToString(tmp);
            vals.push_back(tmp);
            if (with_scores)
            {
                char buf[256];
                snprintf(buf, sizeof(buf), "%.17g", sv.score);
                vals.push_back(buf);
            }
        }
        return 0;
    }
    int MMKVImpl::ZRangeByLex(DBID db, const Data& key, const std::string& min, const std::string& max,
            int limit_offset, int limit_count, StringArray& vals)
    {
        zlexrangespec range;
        int count = 0;
        int err = 0;
        /* Parse the range arguments */
        if ((err = zslParseLexRange(min, max, &range)) != 0)
        {
            return err;
        }
        vals.clear();
        RWLockGuard<MemorySegmentManager, READ_LOCK> keylock_guard(m_segment);
        ZSet* zset = GetObject<ZSet>(db, key, V_TYPE_ZSET, false, err)();
        if (IS_NOT_EXISTS(err))
        {
            return 0;
        }
        if (0 != err)
        {
            return err;
        }
        if (zset->set.empty())
        {
            return 0;
        }
        ScoreValue sv;
        sv.score = zset->set.begin()->score;
        sv.value = range.min;
        Object min_cstr(range.min);
        Object max_cstr(range.max);
        SortedSet::iterator min_it = range.min_empty_type == -1 ? zset->set.begin() : zset->set.lower_bound(sv);
        if (limit_offset > 0)
        {
            min_it.increment_by(limit_offset);
        }
        while (min_it != zset->set.end())
        {
            if (limit_count > 0 && vals.size() >= limit_count)
            {
                break;
            }
            ScoreValue& tmp = *min_it;
            int cmpret = tmp.value.Compare(max_cstr);
            if (range.minex && min_cstr == tmp.value)
            {
                min_it++;
            }
            else if (cmpret > 0 || (range.maxex && cmpret == 0))
            {
                break;
            }
            else
            {
                std::string tmpstr;
                tmp.value.ToString(tmpstr);
                vals.push_back(tmpstr);
                min_it++;
            }
        }
        return 0;
    }
    int MMKVImpl::ZRangeByScore(DBID db, const Data& key, const std::string& min, const std::string& max,
            bool with_scores, int limit_offset, int limit_count, StringArray& vals)
    {
        zrangespec spec;
        int err = zslParseRange(min, max, &spec);
        if (0 != err)
        {
            return err;
        }
        vals.clear();
        RWLockGuard<MemorySegmentManager, READ_LOCK> keylock_guard(m_segment);
        ZSet* zset = GetObject<ZSet>(db, key, V_TYPE_ZSET, false, err)();
        if (IS_NOT_EXISTS(err))
        {
            return 0;
        }
        if (0 != err)
        {
            return err;
        }
        ScoreValue min_sv;
        min_sv.score = spec.min;
        SortedSet::iterator min_it = zset->set.lower_bound(min_sv);
        while (spec.minex && min_it != zset->set.end())
        {
            if (min_it->score == spec.min)
            {
                min_it++;
            }
            else
            {
                break;
            }
        }
        if (limit_offset > 0 && min_it != zset->set.end())
        {
            min_it.increment_by(limit_offset);
        }
        int value_count = 0;
        while (min_it != zset->set.end())
        {
            if (limit_count > 0 && value_count >= limit_count)
            {
                break;
            }
            ScoreValue& sv = *min_it;
            if (sv.score > spec.max || (spec.maxex && sv.score == spec.max))
            {
                break;
            }
            value_count++;
            std::string tmp;
            sv.value.ToString(tmp);
            vals.push_back(tmp);
            if (with_scores)
            {
                char buf[256];
                snprintf(buf, sizeof(buf), "%.17g", sv.score);
                vals.push_back(buf);
            }
            min_it++;
        }
        return 0;
    }

    int MMKVImpl::ZRank(DBID db, const Data& key, const Data& member)
    {
        int err = 0;
        RWLockGuard<MemorySegmentManager, READ_LOCK> keylock_guard(m_segment);
        ZSet* zset = GetObject<ZSet>(db, key, V_TYPE_ZSET, false, err)();
        if (0 != err)
        {
            return err;
        }
        StringDoubleTable::iterator found = zset->scores.find(member);
        if (found == zset->scores.end())
        {
            return ERR_ENTRY_NOT_EXIST;
        }
        ScoreValue sv;
        sv.value = member;
        sv.score = found.value();
        SortedSet::iterator fit = zset->set.find(sv);
        if (fit == zset->set.end())
        {
            ABORT("Find no entry in sorted set.");
        }
        return btree_rank(zset->set, fit);
    }
    int MMKVImpl::ZRem(DBID db, const Data& key, const DataArray& members)
    {
        if (m_readonly)
        {
            return ERR_PERMISSION_DENIED;
        }
        int err = 0;
        RWLockGuard<MemorySegmentManager, WRITE_LOCK> keylock_guard(m_segment);
        ZSet* zset = GetObject<ZSet>(db, key, V_TYPE_ZSET, false, err)();
        if (IS_NOT_EXISTS(err))
        {
            return 0;
        }
        if (0 != err)
        {
            return err;
        }
        size_t removed = 0;
        for (size_t i = 0; i < members.size(); i++)
        {
            StringDoubleTable::iterator found = zset->scores.find(members[i]);
            if (found == zset->scores.end())
            {
                continue;
            }
            ScoreValue tmp;
            tmp.score = found.value();
            tmp.value = members[i];
            SortedSet::iterator fit = zset->set.find(tmp);
            if (fit == zset->set.end())
            {
                ABORT("No zset element found.");
            }
            ScoreValue sv = *fit;
            zset->set.erase(fit);
            zset->scores.erase(found);
            DestroyObjectContent(sv.value);
            removed++;
        }
        return removed;
    }
    int MMKVImpl::ZRemRangeByLex(DBID db, const Data& key, const std::string& min, const std::string& max)
    {
        if (m_readonly)
        {
            return ERR_PERMISSION_DENIED;
        }
        zlexrangespec range;
        int count = 0;
        int err = 0;
        /* Parse the range arguments */
        if ((err = zslParseLexRange(min, max, &range)) != 0)
        {
            return err;
        }
        RWLockGuard<MemorySegmentManager, READ_LOCK> keylock_guard(m_segment);
        ZSet* zset = GetObject<ZSet>(db, key, V_TYPE_ZSET, false, err)();
        if (IS_NOT_EXISTS(err))
        {
            return 0;
        }
        if (0 != err)
        {
            return err;
        }
        if (zset->set.empty())
        {
            return 0;
        }
        ScoreValue sv;
        sv.score = zset->set.begin()->score;
        sv.value = range.min;
        Object min_cstr(range.min);
        Object max_cstr(range.max);
        SortedSet::iterator min_it = range.min_empty_type == -1 ? zset->set.begin() : zset->set.lower_bound(sv);
        size_t removed = 0;
        while (min_it != zset->set.end())
        {
            ScoreValue& tmp = *min_it;
            if (range.minex && min_cstr == tmp.value)
            {
                min_it++;
            }
            else if (range.maxex && max_cstr == tmp.value)
            {
                break;
            }
            else
            {
                zset->scores.erase(tmp.value);
                ScoreValue to_remove = tmp;
                min_it = zset->set.erase(min_it);
                DestroyObjectContent(to_remove.value);
                removed++;
            }
        }
        return removed;
    }
    int MMKVImpl::ZRemRangeByRank(DBID db, const Data& key, int start, int end)
    {
        int err;
        RWLockGuard<MemorySegmentManager, WRITE_LOCK> keylock_guard(m_segment);
        ZSet* zset = GetObject<ZSet>(db, key, V_TYPE_ZSET, false, err)();
        if (IS_NOT_EXISTS(err))
        {
            return 0;
        }
        if (0 != err)
        {
            return err;
        }
        int llen = zset->set.size();
        if (start < 0)
            start = llen + start;
        if (end < 0)
            end = llen + end;
        if (start < 0)
            start = 0;
        if (end < 0)
        {
            return ERR_OFFSET_OUTRANGE;
        }
        /* Invariant: start >= 0, so this test will be true when end < 0.
         * The range is empty when start > end or start >= length. */
        if (start > end || start >= llen)
        {
            return 0;
        }
        if (end >= llen)
            end = llen - 1;
        SortedSet::iterator it = zset->set.begin();
        it.increment_by(start);
        for (int i = start; i <= end; i++)
        {
            ScoreValue sv = *it;
            zset->scores.erase(sv.value);
            it = zset->set.erase(it);
            DestroyObjectContent(sv.value);
        }
        return end - start + 1;
    }
    int MMKVImpl::ZRemRangeByScore(DBID db, const Data& key, const std::string& min, const std::string& max)
    {
        if (m_readonly)
        {
            return ERR_PERMISSION_DENIED;
        }
        zrangespec spec;
        int err = zslParseRange(min, max, &spec);
        if (0 != err)
        {
            return err;
        }
        RWLockGuard<MemorySegmentManager, READ_LOCK> keylock_guard(m_segment);
        ZSet* zset = GetObject<ZSet>(db, key, V_TYPE_ZSET, false, err)();
        if (IS_NOT_EXISTS(err))
        {
            return 0;
        }
        if (0 != err)
        {
            return err;
        }
        ScoreValue min_sv;
        min_sv.score = spec.min;
        SortedSet::iterator min_it = zset->set.lower_bound(min_sv);

        while (spec.minex && min_it != zset->set.end())
        {
            if (min_it->score == spec.min)
            {
                min_it++;
            }
            else
            {
                break;
            }
        }
        int remove_count = 0;
        while (min_it != zset->set.end())
        {
            ScoreValue sv = *min_it;
            if (sv.score > spec.max || (spec.maxex && sv.score == spec.max))
            {
                break;
            }
            zset->scores.erase(sv.value);
            min_it = zset->set.erase(min_it);
            DestroyObjectContent(sv.value);
            remove_count++;
        }
        return remove_count;
    }
    int MMKVImpl::ZRevRange(DBID db, const Data& key, int start, int end, bool with_scores, StringArray& vals)
    {
        int err;
        vals.clear();
        RWLockGuard<MemorySegmentManager, READ_LOCK> keylock_guard(m_segment);
        ZSet* zset = GetObject<ZSet>(db, key, V_TYPE_ZSET, false, err)();
        if (IS_NOT_EXISTS(err))
        {
            return 0;
        }
        if (0 != err)
        {
            return err;
        }
        int llen = zset->set.size();
        if (0 == llen)
        {
            return 0;
        }
        if (start < 0)
            start = llen + start;
        if (end < 0)
            end = llen + end;
        if (start < 0)
            start = 0;
        if (end < 0)
        {
            return ERR_OFFSET_OUTRANGE;
        }
        /* Invariant: start >= 0, so this test will be true when end < 0.
         * The range is empty when start > end or start >= length. */
        if (start > end || start >= llen)
        {
            return 0;
        }
        if (end >= llen)
            end = llen - 1;
        SortedSet::iterator it = zset->set.begin();
        it.increment_by(llen - 1 - start);
        for (int i = end; i >= start; i--, it--)
        {
            ScoreValue& sv = *it;
            std::string tmp;
            sv.value.ToString(tmp);
            vals.push_back(tmp);
            if (with_scores)
            {
                char buf[256];
                snprintf(buf, sizeof(buf), "%.17g", sv.score);
                vals.push_back(buf);
            }
        }
        return 0;
    }
    int MMKVImpl::ZRevRangeByLex(DBID db, const Data& key, const std::string& max, const std::string& min,
            int limit_offset, int limit_count, StringArray& vals)
    {
        zlexrangespec range;
        int count = 0;
        int err = 0;
        /* Parse the range arguments */
        if ((err = zslParseLexRange(min, max, &range)) != 0)
        {
            return err;
        }
        RWLockGuard<MemorySegmentManager, READ_LOCK> keylock_guard(m_segment);
        ZSet* zset = GetObject<ZSet>(db, key, V_TYPE_ZSET, false, err)();
        if (IS_NOT_EXISTS(err))
        {
            return 0;
        }
        if (0 != err)
        {
            return err;
        }
        if (zset->set.empty())
        {
            return 0;
        }
        ScoreValue sv;
        sv.score = zset->set.begin()->score;
        sv.value = range.max;
        Object min_cstr(range.min);
        Object max_cstr(range.max);

        SortedSet::iterator max_it = range.max_empty_type == 1 ? zset->set.end() : zset->set.lower_bound(sv);
        if (max_it == zset->set.end())
        {
            max_it--;
        }
        while (range.maxex)
        {
            if (max_it->value == range.max && max_it != zset->set.begin())
            {
                max_it--;
            }
            else
            {
                break;
            }
        }

        SortedSet::iterator sit;
        if (limit_offset > 0)
        {
            size_t max_rank = btree_rank(zset->set, max_it);
            if (max_rank < limit_offset)
            {
                return 0;
            }
            sit = zset->set.begin();
            sit.increment_by(max_rank - limit_offset);
        }
        else
        {
            sit = max_it;
        }
        int value_count = 0;
        while (true)
        {
            if (limit_count > 0 && value_count >= limit_count)
            {
                break;
            }
            ScoreValue& sv = *max_it;
            if (sv.value < min_cstr || (range.minex && sv.value == min_cstr))
            {
                break;
            }
            value_count++;
            std::string tmp;
            sv.value.ToString(tmp);
            vals.push_back(tmp);

            if (sit == zset->set.begin())
            {
                break;
            }
            max_it--;
        }
        return 0;
    }
    int MMKVImpl::ZRevRangeByScore(DBID db, const Data& key, const std::string& max, const std::string& min,
            bool with_scores, int limit_offset, int limit_count, StringArray& vals)
    {
        zrangespec spec;
        int err = zslParseRange(min, max, &spec);
        if (0 != err)
        {
            return err;
        }
        vals.clear();
        RWLockGuard<MemorySegmentManager, READ_LOCK> keylock_guard(m_segment);
        ZSet* zset = GetObject<ZSet>(db, key, V_TYPE_ZSET, false, err)();
        if (IS_NOT_EXISTS(err))
        {
            return 0;
        }
        if (0 != err)
        {
            return err;
        }
        if (zset->scores.size() == 0)
        {
            return 0;
        }
        ScoreValue max_sv;
        max_sv.score = spec.max;
        SortedSet::iterator max_it = zset->set.lower_bound(max_sv);
        if (max_it == zset->set.end())
        {
            max_it--;
        }
        while (spec.maxex)
        {
            if (max_it->score == spec.max && max_it != zset->set.begin())
            {
                max_it--;
            }
            else
            {
                break;
            }
        }
        SortedSet::iterator sit;
        if (limit_offset > 0)
        {
            size_t max_rank = btree_rank(zset->set, max_it);
            if (max_rank < limit_offset)
            {
                return 0;
            }
            sit = zset->set.begin();
            sit.increment_by(max_rank - limit_offset);
        }
        else
        {
            sit = max_it;
        }
        int value_count = 0;
        bool last_element = false;
        while (true)
        {
            if (limit_count > 0 && value_count >= limit_count)
            {
                break;
            }
            ScoreValue& sv = *sit;
            if (sv.score < spec.min || (spec.minex && sv.score == spec.min))
            {
                break;
            }

            value_count++;
            std::string tmp;
            sv.value.ToString(tmp);
            vals.push_back(tmp);
            if (with_scores)
            {
                char buf[256];
                snprintf(buf, sizeof(buf), "%.17g", sv.score);
                vals.push_back(buf);
            }
            if (sit == zset->set.begin())
            {
                break;
            }
            sit--;
        }
        return 0;
    }
    int MMKVImpl::ZRevRank(DBID db, const Data& key, const Data& member)
    {
        int err = 0;
        RWLockGuard<MemorySegmentManager, READ_LOCK> keylock_guard(m_segment);
        ZSet* zset = GetObject<ZSet>(db, key, V_TYPE_ZSET, false, err)();
        if (0 != err)
        {
            return err;
        }
        StringDoubleTable::iterator found = zset->scores.find(member);
        if (found == zset->scores.end())
        {
            return ERR_ENTRY_NOT_EXIST;
        }
        ScoreValue sv;
        sv.value = member;
        sv.score = found.value();
        SortedSet::iterator fit = zset->set.find(sv);
        if (fit == zset->set.end())
        {
            ABORT("Find no entry in sorted set.");
        }
        return zset->set.size() - btree_rank(zset->set, fit) - 1;
    }
    int MMKVImpl::ZScore(DBID db, const Data& key, const Data& member, double& score)
    {
        int err = 0;
        RWLockGuard<MemorySegmentManager, WRITE_LOCK> keylock_guard(m_segment);
        ZSet* zset = GetObject<ZSet>(db, key, V_TYPE_ZSET, false, err)();
        if (0 != err)
        {
            return err;
        }
        StringDoubleTable::iterator found = zset->scores.find(member);
        if (found == zset->scores.end())
        {
            return ERR_ENTRY_NOT_EXIST;
        }
        score = found.value();
        return 0;
    }
    int MMKVImpl::ZScan(DBID db, const Data& key, int cursor, const std::string& pattern, int32_t limit_count)
    {
        return -1;
    }

    static inline size_t SetLen(Object* set)
    {
        if (NULL == set)
        {
            return 0;
        }
        if (set->type == V_TYPE_ZSET)
        {
            SortedSet* ss = (SortedSet*) set->RawValue();
            return ss->size();
        }
        StringSet* ss = (StringSet*) set->RawValue();
        return ss->size();
    }

    static void aggregate_score(int aggregate_type, double current_score, double& score)
    {
        switch (aggregate_type)
        {
            case AGGREGATE_SUM:
            {
                score += current_score;
                break;
            }
            case AGGREGATE_MIN:
            {
                if (current_score < score)
                {
                    score = current_score;
                }
                break;
            }
            case AGGREGATE_MAX:
            {
                if (current_score > score)
                {
                    score = current_score;
                }
                break;
            }
            default:
            {
                break;
            }
        }
    }

    int MMKVImpl::GenericZSetInterfUnion(DBID db, int op, const Data& destination, const DataArray& keys,
            const WeightArray& weights, const std::string& aggregate)
    {
        if (!weights.empty() && weights.size() != keys.size())
        {
            return ERR_SIZE_MISMATCH;
        }
        MMKVTable* kv = GetMMKVTable(db, false);
        if (NULL == kv)
        {
            return ERR_ENTRY_NOT_EXIST;
        }
        std::vector<Object*> sets;
        sets.resize(keys.size());
        int err;
        Allocator<char> allocator = m_segment.ValueAllocator<char>();
        RWLockGuard<MemorySegmentManager, WRITE_LOCK> keylock_guard(m_segment);
        ZSet* destset = GetObject<ZSet>(db, destination, V_TYPE_ZSET, true, err)(allocator);
        if (0 != err)
        {
            return err;
        }

        for (size_t i = 0; i < keys.size(); i++)
        {
            const Object* o = FindMMValue(kv, keys[i]);
            if (NULL != o && o->type != V_TYPE_ZSET && o->type != V_TYPE_SET)
            {
                return ERR_INVALID_TYPE;
            }
            if (0 != err)
            {
                return err;
            }
            sets[i] = const_cast<Object*>(o);
        }
        StdObjectScoreTable cache_result;
        bool fill_cache_result = false;
        int aggregate_type = AGGREGATE_SUM;
        if (!aggregate.empty())
        {
            if (!strcasecmp(aggregate.c_str(), "min"))
            {
                aggregate_type = AGGREGATE_MIN;
            }
            else if (!strcasecmp(aggregate.c_str(), "max"))
            {
                aggregate_type = AGGREGATE_MAX;
            }
            else if (!strcasecmp(aggregate.c_str(), "sum"))
            {
                aggregate_type = AGGREGATE_SUM;
            }
            else
            {
                return ERR_INVALID_ARGS;
            }
        }

        for (size_t i = 0; i < keys.size(); i++)
        {
            if (SetLen(sets[i]) == 0 && op == OP_INTER)
            {
                if (op == OP_INTER)
                {
                    GenericDelValue(V_TYPE_ZSET, destset);
                    return 0;
                }
                else
                {
                    continue;
                }
            }

            if (sets[i]->type == V_TYPE_SET)
            {
                StringSet* ss = (StringSet*) sets[i]->RawValue();
                if (!fill_cache_result || op == OP_UNION)
                {
                    StringSet::iterator it = ss->begin();
                    while (it != ss->end())
                    {
                        double current_score = weights.empty() ? 1.0 : weights[i] * 1.0;
                        std::pair<StdObjectScoreTable::iterator, bool> ret = cache_result.insert(
                                StdObjectScoreTable::value_type(*it, current_score));
                        if (ret.second)
                        {
                            aggregate_score(aggregate_type, current_score, ret.first->second);
                        }

                        it++;
                    }
                    fill_cache_result = true;
                }
                else
                {
                    StdObjectScoreTable::iterator sit = cache_result.begin();
                    while (sit != cache_result.end())
                    {
                        StringSet::iterator ssit = ss->find(sit->first);
                        if (ssit == ss->end())
                        {
                            cache_result.erase(sit++);
                        }
                        else
                        {
                            double current_score = weights.empty() ? 1.0 : weights[i] * 1.0;
                            aggregate_score(aggregate_type, current_score, sit->second);
                            sit++;
                        }
                    }
                }
            }
            else
            {
                ZSet* ss = (ZSet*) sets[i]->RawValue();
                if (!fill_cache_result || op == OP_UNION)
                {
                    SortedSet::iterator it = ss->set.begin();
                    while (it != ss->set.end())
                    {
                        double current_score = weights.empty() ? it->score : weights[i] * it->score;
                        std::pair<StdObjectScoreTable::iterator, bool> ret = cache_result.insert(
                                StdObjectScoreTable::value_type(it->value, current_score));
                        if (ret.second)
                        {
                            aggregate_score(aggregate_type, current_score, ret.first->second);
                        }
                        it++;
                    }
                    fill_cache_result = true;
                }
                else
                {
                    StdObjectScoreTable::iterator sit = cache_result.begin();
                    while (sit != cache_result.end())
                    {
                        ScoreValue sv;
                        sv.score = sit->second;
                        sv.value = sit->first;
                        StringDoubleTable::iterator ssit = ss->scores.find(sit->first);
                        if (ssit == ss->scores.end())
                        {
                            cache_result.erase(sit++);
                        }
                        else
                        {
                            double current_score = weights.empty() ? ssit.value() : weights[i] * ssit.value();
                            aggregate_score(aggregate_type, current_score, sit->second);
                            sit++;
                        }
                    }
                }
            }
        }
        //remove elements not in dest set
        StringDoubleTable::iterator it = destset->scores.begin();
        while (it != destset->scores.end())
        {
            Object element = it.key();
            StdObjectScoreTable::iterator cit = cache_result.find(element);
            if (cit == cache_result.end()) //remove elements from results which already in dest set
            {
                ScoreValue sv;
                sv.value = element;
                sv.score = it.value();
                destset->scores.erase(it);
                destset->set.erase(sv);
                DestroyObjectContent(element);
            }
            it++;
        }
        destset->set.clear();

        //insert rest elements
        StdObjectScoreTable::iterator cit = cache_result.begin();
        while (cit != cache_result.end())
        {

            std::pair<StringDoubleTable::iterator, bool> ret = destset->scores.insert(
                    StringDoubleTable::value_type(cit->first, cit->second));
            if (!ret.second)
            {
                ret.first.value(cit->second);
            }
            ScoreValue sv;
            sv.value = CloneStrObject(cit->first, false);
            sv.score = cit->second;
            const_cast<Object&>(ret.first.key()) = sv.value;
            destset->set.insert(sv);
            cit++;
        }
        return 0;
    }

    int MMKVImpl::ZInterStore(DBID db, const Data& destination, const DataArray& keys, const WeightArray& weights,
            const std::string& aggregate)
    {
        if (m_readonly)
        {
            return ERR_PERMISSION_DENIED;
        }
        return GenericZSetInterfUnion(db, OP_INTER, destination, keys, weights, aggregate);
    }
    int MMKVImpl::ZUnionStore(DBID db, const Data& destination, const DataArray& keys, const WeightArray& weights,
            const std::string& aggregate)
    {
        if (m_readonly)
        {
            return ERR_PERMISSION_DENIED;
        }
        return GenericZSetInterfUnion(db, OP_UNION, destination, keys, weights, aggregate);
    }
}
