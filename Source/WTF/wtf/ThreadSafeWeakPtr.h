/*
 * Copyright (C) 2022 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <wtf/Lock.h>
#include <wtf/MainThread.h>
#include <wtf/RefPtr.h>
#include <wtf/TaggedPtr.h>

namespace WTF {

template<typename T, typename = NoTaggingTraits<T>> class ThreadSafeWeakPtr;
template<typename> class ThreadSafeWeakHashSet;
template<typename, DestructionThread> class ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr;

class ThreadSafeWeakPtrControlBlock {
    WTF_MAKE_NONCOPYABLE(ThreadSafeWeakPtrControlBlock);
    WTF_MAKE_FAST_ALLOCATED;
public:
    ThreadSafeWeakPtrControlBlock* weakRef()
    {
        Locker locker { m_lock };
        if (m_object) {
            ++m_weakReferenceCount;
            return this;
        }
        return nullptr;
    }

    void weakDeref()
    {
        bool shouldDeleteControlBlock { false };
        {
            Locker locker { m_lock };
            ASSERT_WITH_SECURITY_IMPLICATION(m_weakReferenceCount);
            if (!--m_weakReferenceCount && !m_strongReferenceCount)
                shouldDeleteControlBlock = true;
        }
        if (shouldDeleteControlBlock)
            delete this;
    }

    size_t weakReferenceCount() const
    {
        Locker locker { m_lock };
        return m_weakReferenceCount;
    }

    size_t refCount() const
    {
        Locker locker { m_lock };
        return m_strongReferenceCount;
    }

    bool hasOneRef() const
    {
        Locker locker { m_lock };
        return m_strongReferenceCount == 1;
    }

    void strongRef() const
    {
        Locker locker { m_lock };
        ASSERT_WITH_SECURITY_IMPLICATION(m_object);
        ++m_strongReferenceCount;
    }

    template<typename T, DestructionThread destructionThread>
    void strongDeref() const
    {
        bool shouldDeleteControlBlock { false };
        T* object;

        {
            Locker locker { m_lock };
            ASSERT_WITH_SECURITY_IMPLICATION(m_object);
            if (LIKELY(--m_strongReferenceCount))
                return;
            object = static_cast<T*>(std::exchange(m_object, nullptr));
            if (!m_weakReferenceCount)
                shouldDeleteControlBlock = true;
        }

        auto deleteObject = [this, object, shouldDeleteControlBlock] {
            delete static_cast<const T*>(object);
            if (shouldDeleteControlBlock)
                delete this;
        };
        switch (destructionThread) {
        case DestructionThread::Any:
            deleteObject();
            break;
        case DestructionThread::Main:
            ensureOnMainThread(WTFMove(deleteObject));
            break;
        case DestructionThread::MainRunLoop:
            ensureOnMainRunLoop(WTFMove(deleteObject));
            break;
        }
    }

    template<typename T>
    RefPtr<T> makeStrongReferenceIfPossible(const T* objectOfCorrectType) const
    {
        Locker locker { m_lock };
        if (m_object) {
            // Calling the RefPtr constructor would call strongRef() and deadlock.
            ++m_strongReferenceCount;
            return adoptRef(const_cast<T*>(objectOfCorrectType));
        }
        return nullptr;
    }

    bool objectHasStartedDeletion() const
    {
        Locker locker { m_lock };
        return !m_object;
    }

private:
    template<typename, DestructionThread> friend class ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr;
    template<typename T>
    explicit ThreadSafeWeakPtrControlBlock(T* object)
        : m_object(object) { }

    mutable Lock m_lock;
    mutable size_t m_strongReferenceCount WTF_GUARDED_BY_LOCK(m_lock) { 1 };
    mutable size_t m_weakReferenceCount WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    mutable void* m_object WTF_GUARDED_BY_LOCK(m_lock) { nullptr };
};

struct ThreadSafeWeakPtrControlBlockRefDerefTraits {
    static ALWAYS_INLINE ThreadSafeWeakPtrControlBlock* refIfNotNull(ThreadSafeWeakPtrControlBlock* ptr)
    {
        if (LIKELY(ptr))
            return ptr->weakRef();
        return nullptr;
    }

    static ALWAYS_INLINE void derefIfNotNull(ThreadSafeWeakPtrControlBlock* ptr)
    {
        if (LIKELY(ptr))
            ptr->weakDeref();
    }
};
using ControlBlockRefPtr = RefPtr<ThreadSafeWeakPtrControlBlock, RawPtrTraits<ThreadSafeWeakPtrControlBlock>, ThreadSafeWeakPtrControlBlockRefDerefTraits>;

template<typename T, DestructionThread destructionThread = DestructionThread::Any>
class ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr {
    WTF_MAKE_NONCOPYABLE(ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr);
    WTF_MAKE_FAST_ALLOCATED;
public:
    void ref() const { m_controlBlock.strongRef(); }
    void deref() const { m_controlBlock.template strongDeref<T, destructionThread>(); }
    size_t refCount() const { return m_controlBlock.refCount(); }
    bool hasOneRef() const { return m_controlBlock.hasOneRef(); }
protected:
    ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr() = default;
    ThreadSafeWeakPtrControlBlock& controlBlock() const { return m_controlBlock; }
private:
    template<typename, typename> friend class ThreadSafeWeakPtr;
    template<typename> friend class ThreadSafeWeakHashSet;
    ThreadSafeWeakPtrControlBlock& m_controlBlock { *new ThreadSafeWeakPtrControlBlock(static_cast<T*>(this)) };
};

template<typename T>
inline void retainThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr(T* obj)
{
    RELEASE_ASSERT(obj != nullptr);
    static_assert(std::derived_from<T, ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<T>>);
    static_cast<ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<T>*>(obj)->ref();
}

template<typename T>
inline void releaseThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr(T* obj)
{
    RELEASE_ASSERT(obj != nullptr);
    static_assert(std::derived_from<T, ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<T>>);
    static_cast<ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<T>*>(obj)->deref();
}


template<typename T, typename TaggingTraits /* = NoTaggingTraits<T> */>
class ThreadSafeWeakPtr {
public:
    using TagType = typename TaggingTraits::TagType;
    ThreadSafeWeakPtr() = default;

    ThreadSafeWeakPtr(std::nullptr_t) { }

    ThreadSafeWeakPtr(const ThreadSafeWeakPtr& other)
        : m_objectOfCorrectType(other.m_objectOfCorrectType)
        , m_controlBlock(other.m_controlBlock)
    { }

    ThreadSafeWeakPtr(ThreadSafeWeakPtr&& other)
        : m_objectOfCorrectType(std::exchange(other.m_objectOfCorrectType, nullptr))
        , m_controlBlock(std::exchange(other.m_controlBlock, nullptr))
    { }

    template<typename U, std::enable_if_t<!std::is_pointer_v<U>>* = nullptr>
    ThreadSafeWeakPtr(const U& retainedReference)
        : m_objectOfCorrectType(static_cast<const T*>(&retainedReference))
        , m_controlBlock(controlBlock(retainedReference))
    { }

    template<typename U>
    ThreadSafeWeakPtr(const U* retainedPointer)
        : m_objectOfCorrectType(static_cast<const T*>(retainedPointer))
        , m_controlBlock(retainedPointer ? controlBlock(*retainedPointer) : nullptr)
    { }

    template<typename U>
    ThreadSafeWeakPtr(const Ref<U>& strongReference)
        : m_objectOfCorrectType(static_cast<const T*>(strongReference.ptr()))
        , m_controlBlock(controlBlock(strongReference.get()))
    { }

    template<typename U>
    ThreadSafeWeakPtr(const RefPtr<U>& strongReference)
        : m_objectOfCorrectType(static_cast<const T*>(strongReference.get()))
        , m_controlBlock(strongReference ? controlBlock(*strongReference) : nullptr)
    { }

    ThreadSafeWeakPtr(ThreadSafeWeakPtrControlBlock& controlBlock, const T& objectOfCorrectType)
        : m_objectOfCorrectType(&objectOfCorrectType)
        , m_controlBlock(&controlBlock)
    { }

    ThreadSafeWeakPtr& operator=(ThreadSafeWeakPtr&& other)
    {
        m_controlBlock = std::exchange(other.m_controlBlock, nullptr);
        m_objectOfCorrectType = std::exchange(other.m_objectOfCorrectType, nullptr);
        return *this;
    }

    ThreadSafeWeakPtr& operator=(const ThreadSafeWeakPtr& other)
    {
        m_controlBlock = other.m_controlBlock;
        m_objectOfCorrectType = other.m_objectOfCorrectType;
        return *this;
    }

    template<typename U, std::enable_if_t<!std::is_pointer_v<U>>* = nullptr>
    ThreadSafeWeakPtr& operator=(const U& retainedReference)
    {
        m_controlBlock = controlBlock(retainedReference);
        m_objectOfCorrectType = static_cast<const T*>(static_cast<const U*>(&retainedReference));
        return *this;
    }

    template<typename U>
    ThreadSafeWeakPtr& operator=(const U* retainedPointer)
    {
        m_controlBlock = retainedPointer ? controlBlock(*retainedPointer) : nullptr;
        m_objectOfCorrectType = static_cast<const T*>(retainedPointer);
        return *this;
    }

    ThreadSafeWeakPtr& operator=(std::nullptr_t)
    {
        m_controlBlock = nullptr;
        m_objectOfCorrectType = nullptr;
        return *this;
    }

    template<typename U>
    ThreadSafeWeakPtr& operator=(const Ref<U>& strongReference)
    {
        m_controlBlock = controlBlock(strongReference);
        m_objectOfCorrectType = static_cast<const T*>(strongReference.ptr());
        return *this;
    }

    template<typename U>
    ThreadSafeWeakPtr& operator=(const RefPtr<U>& strongReference)
    {
        m_controlBlock = strongReference ? controlBlock(*strongReference) : nullptr;
        m_objectOfCorrectType = static_cast<const T*>(strongReference.get());
        return *this;
    }

    RefPtr<T> get() const { return m_controlBlock ? m_controlBlock->template makeStrongReferenceIfPossible<T>(m_objectOfCorrectType.ptr()) : nullptr; }

    void setTag(TagType tag) { m_objectOfCorrectType.setTag(tag); }
    TagType tag() const { return m_objectOfCorrectType.tag(); }

private:
    template<typename U, std::enable_if_t<std::is_convertible_v<U*, T*>>* = nullptr>
    ThreadSafeWeakPtrControlBlock* controlBlock(const U& classOrChildClass)
    {
        return &classOrChildClass.controlBlock();
    }

    template<typename, DestructionThread> friend class ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr;
    template<typename> friend class ThreadSafeWeakHashSet;
    template<typename> friend class ThreadSafeWeakOrStrongPtr;

    TaggedPtr<T, TaggingTraits> m_objectOfCorrectType;
    // FIXME: Either remove ThreadSafeWeakPtrControlBlock::m_object as redundant information,
    // or use CompactRefPtrTuple to reduce sizeof(ThreadSafeWeakPtr) by storing just an offset
    // from ThreadSafeWeakPtrControlBlock::m_object and don't support structs larger than 65535.
    ControlBlockRefPtr m_controlBlock;
};

template<class T> ThreadSafeWeakPtr(const T&) -> ThreadSafeWeakPtr<T>;
template<class T> ThreadSafeWeakPtr(const T*) -> ThreadSafeWeakPtr<T>;

template<typename T>
class ThreadSafeWeakOrStrongPtr {
public:
    enum class Status {
        Strong = 0,
        Weak = 1
    };

    Status status() const { return m_weak.tag(); }
    bool isWeak() const { return status() == Status::Weak; }
    // This says nullptr is strong, which makes sense because you can always have a strong reference to nullptr but could be a little non-intuitive.
    bool isStrong() const { return !isWeak(); }

    RefPtr<T> get() const { return isWeak() ? m_weak.get() : m_strong; }
    T* ptr() const { ASSERT(isStrong()); return m_strong.get(); }

    // NB. This function is not atomic so it's not safe to call get() while this transition is happening.
    RefPtr<T> convertToWeak()
    {
        ASSERT(isStrong());
        RefPtr<T> strong = WTFMove(m_strong);
        m_weak = strong;
        m_weak.setTag(Status::Weak);
        ASSERT(isWeak());
        return strong;
    }

    T* tryConvertToStrong()
    {
        ASSERT(isWeak());
        RefPtr<T> strong = m_weak.get();
        m_weak.setTag(Status::Strong);
        m_weak = nullptr;
        m_strong = WTFMove(strong);
        return ptr();
    }

    ThreadSafeWeakOrStrongPtr& operator=(const ThreadSafeWeakOrStrongPtr& other)
    {
        *this = nullptr;
        if (other.isWeak())
            m_weak = other.m_weak;
        else
            m_strong = other.m_strong;
        ASSERT(status() == other.status());
        return *this;
    }

    ThreadSafeWeakOrStrongPtr& operator=(ThreadSafeWeakOrStrongPtr&& other)
    {
        *this = nullptr;
        Status status = other.status();
        if (status == Status::Weak)
            m_weak = std::exchange(other.m_weak, nullptr);
        else
            m_strong = std::exchange(other.m_strong, nullptr);
        ASSERT(status == this->status());
        ASSERT(!other.ptr());
        return *this;
    }

    ThreadSafeWeakOrStrongPtr& operator=(std::nullptr_t)
    {
        if (isWeak())
            m_weak = nullptr;
        else
            m_strong = nullptr;
        return *this;
    }

    template<typename U>
    ThreadSafeWeakOrStrongPtr& operator=(const RefPtr<U>& strongReference)
    {
        if (isWeak())
            m_weak = nullptr;
        m_strong = strongReference;
        ASSERT(isStrong());
        return *this;
    }

    template<typename U>
    ThreadSafeWeakOrStrongPtr& operator=(RefPtr<U>&& strongReference)
    {
        if (isWeak())
            m_weak = nullptr;
        m_strong = WTFMove(strongReference);
        ASSERT(isStrong());
        return *this;
    }

    template<typename U>
    ThreadSafeWeakOrStrongPtr& operator=(const Ref<U>& strongReference)
    {
        if (isWeak())
            m_weak = nullptr;
        m_strong = strongReference;
        ASSERT(isStrong());
        return *this;
    }

    template<typename U>
    ThreadSafeWeakOrStrongPtr& operator=(Ref<U>&& strongReference)
    {
        if (isWeak())
            m_weak = nullptr;
        m_strong = WTFMove(strongReference);
        ASSERT(isStrong());
        return *this;
    }

    ThreadSafeWeakOrStrongPtr()
        : m_weak(nullptr)
    {
        ASSERT(isStrong());
    }

    template<typename U>
    ThreadSafeWeakOrStrongPtr(const Ref<U>& strongReference) { *this = strongReference; }

    template<typename U>
    ThreadSafeWeakOrStrongPtr(const RefPtr<U>& strongReference) { *this = strongReference; }

    template<typename U>
    ThreadSafeWeakOrStrongPtr(Ref<U>&& strongReference) { *this = WTFMove(strongReference); }

    template<typename U>
    ThreadSafeWeakOrStrongPtr(RefPtr<U>&& strongReference) { *this = WTFMove(strongReference); }

    ~ThreadSafeWeakOrStrongPtr()
    {
        if (isStrong())
            m_strong.~RefPtr<T>();
        else
            m_weak.~ThreadSafeWeakPtr<T, EnumTaggingTraits<T, Status>>();
    }

private:
    union {
        ThreadSafeWeakPtr<T, EnumTaggingTraits<T, Status>> m_weak;
        RefPtr<T> m_strong;
    };
};

}

using WTF::ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr;
using WTF::ThreadSafeWeakPtr;
using WTF::ThreadSafeWeakPtrControlBlock;
using WTF::ThreadSafeWeakOrStrongPtr;
