#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Scintilla::Internal {

class hierarchical_profiler
{
public:
    hierarchical_profiler()
    {
        m_root.name = "[root]";
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_root.children.clear();
        m_root.call_count = 0;
        m_root.total_ns = 0;
        m_root.min_ns = std::numeric_limits<uint64_t>::max();
        m_root.max_ns = 0;
        m_thread_contexts.clear();
    }

    void begin_scope(const char *name)
    {
        const char *scope_name = name ? name : "";
        const auto start_time = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(m_mutex);
        thread_context &context = thread_context_locked();
        context.start_times.push_back(start_time);

        auto &children = context.current->children;
        auto it = children.find(scope_name);
        if (it == children.end()) {
            auto child = std::make_unique<scope_stats>();
            child->name = scope_name;
            child->parent = context.current;
            it = children.emplace(scope_name, std::move(child)).first;
        }
        context.current = it->second.get();
    }

    void end_scope()
    {
        const auto end_time = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(m_mutex);
        thread_context &context = thread_context_locked();
        if (!context.current || context.start_times.empty()) {
            return;
        }

        const auto start_time = context.start_times.back();
        context.start_times.pop_back();
        const uint64_t elapsed_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());

        context.current->call_count += 1;
        context.current->total_ns += elapsed_ns;
        context.current->min_ns = std::min(context.current->min_ns, elapsed_ns);
        context.current->max_ns = std::max(context.current->max_ns, elapsed_ns);

        if (context.current->parent) {
            context.current = context.current->parent;
        }
    }

    QJsonObject to_json() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return node_to_json(m_root, true);
    }

private:
    struct scope_stats
    {
        std::string name;
        uint64_t call_count = 0;
        uint64_t total_ns = 0;
        uint64_t min_ns = std::numeric_limits<uint64_t>::max();
        uint64_t max_ns = 0;
        std::map<std::string, std::unique_ptr<scope_stats>> children;
        scope_stats *parent = nullptr;
    };

    struct thread_context
    {
        scope_stats *current = nullptr;
        std::vector<std::chrono::steady_clock::time_point> start_times;
    };

    thread_context &thread_context_locked()
    {
        thread_context &context = m_thread_contexts[std::this_thread::get_id()];
        if (!context.current) {
            context.current = &m_root;
        }
        return context;
    }

    static QJsonObject node_to_json(const scope_stats &node, bool include_name)
    {
        QJsonObject object;
        if (include_name) {
            object.insert("name", QString::fromStdString(node.name));
        }

        object.insert("call_count", static_cast<qint64>(node.call_count));
        object.insert("total_ms", static_cast<double>(node.total_ns) / 1'000'000.0);
        object.insert(
            "average_ms",
            node.call_count > 0
                ? (static_cast<double>(node.total_ns) / static_cast<double>(node.call_count)) / 1'000'000.0
                : 0.0);
        object.insert(
            "min_ms",
            node.call_count > 0
                ? static_cast<double>(node.min_ns) / 1'000'000.0
                : 0.0);
        object.insert("max_ms", static_cast<double>(node.max_ns) / 1'000'000.0);

        QJsonArray children;
        for (const auto &[name, child] : node.children) {
            Q_UNUSED(name);
            children.append(node_to_json(*child, true));
        }
        object.insert("children", children);
        return object;
    }

    scope_stats m_root;
    mutable std::mutex m_mutex;
    std::map<std::thread::id, thread_context> m_thread_contexts;
};

inline thread_local hierarchical_profiler *g_active_hierarchical_profiler = nullptr;

class active_hierarchical_profiler_binding
{
public:
    explicit active_hierarchical_profiler_binding(hierarchical_profiler *profiler)
    :
        m_previous(g_active_hierarchical_profiler)
    {
        g_active_hierarchical_profiler = profiler;
    }

    ~active_hierarchical_profiler_binding()
    {
        g_active_hierarchical_profiler = m_previous;
    }

    active_hierarchical_profiler_binding(const active_hierarchical_profiler_binding &) = delete;
    active_hierarchical_profiler_binding &operator=(const active_hierarchical_profiler_binding &) = delete;

private:
    hierarchical_profiler *m_previous = nullptr;
};

class hierarchical_profile_scope
{
public:
    hierarchical_profile_scope(hierarchical_profiler *profiler, const char *name)
    :
        m_profiler(profiler)
    {
        if (m_profiler) {
            m_profiler->begin_scope(name);
        }
    }

    ~hierarchical_profile_scope()
    {
        if (m_profiler) {
            m_profiler->end_scope();
        }
    }

    hierarchical_profile_scope(const hierarchical_profile_scope &) = delete;
    hierarchical_profile_scope &operator=(const hierarchical_profile_scope &) = delete;

private:
    hierarchical_profiler *m_profiler = nullptr;
};

inline hierarchical_profiler *active_hierarchical_profiler()
{
    return g_active_hierarchical_profiler;
}

#define SCINTILLAQUICK_CONCAT_IMPL(a, b) a##b
#define SCINTILLAQUICK_CONCAT(a, b) SCINTILLAQUICK_CONCAT_IMPL(a, b)

#define SCINTILLAQUICK_PROFILE_SCOPE(profiler, name) \
    ::Scintilla::Internal::hierarchical_profile_scope \
        SCINTILLAQUICK_CONCAT(scintillaquick_profile_scope_, __LINE__)((profiler), (name))

#define SCINTILLAQUICK_PROFILE_ACTIVE_SCOPE(name) \
    SCINTILLAQUICK_PROFILE_SCOPE(::Scintilla::Internal::active_hierarchical_profiler(), (name))

} // namespace Scintilla::Internal
