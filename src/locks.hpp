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
#ifndef LOCKS_HPP_
#define LOCKS_HPP_
#include "lock_mode.hpp"
#include "atomic.hpp"
#include <string>
#include <stdint.h>
namespace mmkv
{
    class FileLock
    {
        private:
            int m_fd;
            std::string m_error;
        public:
            FileLock();
            bool Init(const std::string& path);
            bool Lock(LockMode mode = WRITE_LOCK);
            bool Unlock(LockMode mode = WRITE_LOCK);
            const std::string& LastError()const;
            ~FileLock();
    };

    class SpinMutexLock
    {
        public:
            volatile uint32_t m_lock;
        public:
            SpinMutexLock() :
                    m_lock(0)
            {
            }
            bool Lock()
            {
                while (!cmpchg(&m_lock, 0, 1))
                    sched_yield();
                return true;
            }
            bool Unlock()
            {
                m_lock = 0;
                return true;
            }
            ~SpinMutexLock()
            {
            }
    };

    class SpinRWLock
    {
        public:
            volatile uint32_t m_lock;
        public:
            SpinRWLock() :
                    m_lock(0)
            {
            }
            bool Lock(LockMode mode)
            {
                switch (mode)
                {
                    case READ_LOCK:
                    {
                        for (;;)
                        {
                            // Wait for active writer to release the lock
                            while (m_lock & 0xfff00000)
                                sched_yield();

                            if ((0xfff00000 & atomic_add(&m_lock, 1)) == 0)
                                return true;

                            atomic_add(&m_lock, -1);
                        }
                        return true;
                    }
                    case WRITE_LOCK:
                    {
                        for (;;)
                        {
                            // Wait for active writer to release the lock
                            while (m_lock & 0xfff00000)
                                sched_yield();

                            if ((0xfff00000 & atomic_add(&m_lock, 0x100000)) == 0x100000)
                            {
                                // Wait until there's no more readers
                                while (m_lock & 0x000fffff)
                                    sched_yield();

                                return true;
                            }
                            atomic_add(&m_lock, -0x100000);
                        }
                        return true;
                    }
                    default:
                    {
                        return false;
                    }
                }
            }
            bool Unlock(LockMode mode)
            {
                switch (mode)
                {
                    case READ_LOCK:
                    {
                        atomic_add(&m_lock, -1);
                        return true;
                    }
                    case WRITE_LOCK:
                    {
                        atomic_add(&m_lock, -0x100000);
                        return true;
                    }
                    default:
                    {
                        return false;
                    }
                }
            }
            ~SpinRWLock()
            {
            }
    };

    /*
     * port from  http://locklessinc.com/articles/sleeping_rwlocks/
     */
    struct fair_rwlock
    {
            union
            {
                    volatile uint64_t l;
                    volatile uint32_t waiters;
                    volatile uint8_t locked;
            } lock;

            union
            {
                    volatile uint32_t seq;
                    volatile uint8_t contend;
            } read_wait;
    };
    /*
     * WARNING: this lock can NOT be used recursive
     */
    class SleepingRWLock
    {
        private:
            fair_rwlock m_lock;
            bool LockWrite();
            bool LockRead();
            bool UnlockWrite();
            bool UnlockRead();
        public:
            SleepingRWLock();
            bool Lock(LockMode mode);
            bool Unlock(LockMode mode = WRITE_LOCK);
    };



}

#endif /* LOCKS_HPP_ */
