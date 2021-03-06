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

#ifndef OPTIONS_HPP_
#define OPTIONS_HPP_

#include "mmkv_logger.hpp"
#include <map>
#include <vector>
#define GEO_SEARCH_WITH_DISTANCES "WITHDISTANCES"
#define GEO_SEARCH_WITH_COORDINATES "WITHCOORDINATES"
namespace mmkv
{
    typedef uint32_t DBID;
    struct CreateOptions
    {
            int64_t size;
            bool autoexpand;
            CreateOptions() :
                    size(1024 * 1024 * 1024), autoexpand(false)
            {
            }
    };

    /*
     * return -1 means break remove operations
     */
    typedef int ExpireCallback(DBID db, const std::string& key);
    typedef int RoutineCallback();
    struct OpenOptions
    {
            std::string dir;
            bool readonly;
            bool verify;
            bool reserve_space;
            bool use_lock;
            bool create_if_notexist;
            bool open_ignore_error;
            uint32_t hll_sparse_max_bytes;
            LogLevel log_level;
            LoggerFunc* log_func;
            ExpireCallback* expire_cb;
            RoutineCallback* routine_cb;
            CreateOptions create_options;

            OpenOptions() :
                    dir("./mmkv"), readonly(false), verify(true), reserve_space(false), use_lock(false), create_if_notexist(false), open_ignore_error(false), hll_sparse_max_bytes(
                            3000), log_level(INFO_LOG_LEVEL), log_func(
                    NULL), expire_cb(NULL), routine_cb(NULL)
            {
            }
    };

    struct GeoSearchOptions
    {
            std::string coord_type;
            bool asc;   //sort by asc
            uint32_t radius;  //range meters
            int32_t offset;
            int32_t limit;

            long double by_x;
            long double by_y;
            std::string by_member;

            typedef std::map<std::string, std::string> PatternMap;
            typedef std::vector<std::string> PatternArray;
            PatternMap includes;
            PatternMap excludes;
            PatternArray get_patterns;
            GeoSearchOptions() :
                    coord_type("mercator"), asc(true), radius(0), offset(0), limit(0), by_x(0), by_y(0)
            {
            }
    };
}

#endif /* OPTIONS_HPP_ */
