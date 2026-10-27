/*
 * Copyright (C) 2024-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <cassert>
#include <type_traits>
#include <utility>

namespace mfb
{

/**
 * Runs a function when it goes out of scope, unless it was dismissed or already committed.
 */
template<typename F>
class ScopeGuard
{
public:
    [[nodiscard]] explicit ScopeGuard(F &&f) noexcept
        : m_func(std::move(f))
    {
    }

    [[nodiscard]] explicit ScopeGuard(const F &f) noexcept
        : m_func(f)
    {
    }

    [[nodiscard]] ScopeGuard(ScopeGuard &&other) noexcept
        : m_func(std::move(other.m_func)),
          m_invoke(std::exchange(other.m_invoke, false))
    {
    }

    ~ScopeGuard() noexcept
    {
        if (m_invoke)
            m_func();
    }

    /**
     * Never run the function.
     */
    void dismiss() noexcept
    {
        m_invoke = false;
    }

    /**
     * Run the function right now instead of at the end of the scope.
     */
    void commit() noexcept(std::is_nothrow_invocable_v<F>)
    {
        assert(m_invoke);
        m_invoke = false; // do it before we may throw from calling m_func()
        m_func();
    }

    ScopeGuard(const ScopeGuard &) = delete;
    ScopeGuard &operator=(const ScopeGuard &) = delete;
    ScopeGuard &operator=(ScopeGuard &&) = delete;

private:
    F m_func;
    bool m_invoke = true;
};

template<typename F>
ScopeGuard(F (&)()) -> ScopeGuard<F (*)()>;

template<typename F>
[[nodiscard]] ScopeGuard<typename std::decay<F>::type> scopeGuard(F &&f)
{
    return ScopeGuard<typename std::decay<F>::type>(std::forward<F>(f));
}

} // namespace mfb
