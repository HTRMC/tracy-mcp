#include "tools.hpp"

#include "TracyFileRead.hpp"
#include "TracyWorker.hpp"
#include "../public/common/TracyVersion.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tracymcp {
namespace {

// Loaded captures, keyed by path. Keep FileRead alive alongside Worker.
// ponytail: cache never invalidates; re-run the server to pick up a recapture
// at the same path. Add mtime keying if that bites.
struct Capture {
    std::unique_ptr<tracy::FileRead> file;
    std::unique_ptr<tracy::Worker> worker;
};
std::map<std::string, Capture> g_cache;

std::string LinkedVersion() {
    return std::to_string(tracy::Version::Major) + "." + std::to_string(tracy::Version::Minor) +
           "." + std::to_string(tracy::Version::Patch);
}

tracy::Worker& Load(const std::string& path) {
    auto it = g_cache.find(path);
    if (it != g_cache.end()) return *it->second.worker;

    auto f = std::unique_ptr<tracy::FileRead>(tracy::FileRead::Open(path.c_str()));
    if (!f) throw std::runtime_error("could not open trace file: " + path);

    std::unique_ptr<tracy::Worker> w;
    try {
        w = std::make_unique<tracy::Worker>(*f);
    } catch (const tracy::UnsupportedVersion& e) {
        const int v = e.version;
        throw std::runtime_error("unsupported trace version " +
            std::to_string(v >> 16) + "." + std::to_string((v >> 8) & 0xff) + "." +
            std::to_string(v & 0xff) + " -- this build links Tracy " + LinkedVersion() +
            "; bump third_party/tracy to match");
    } catch (const tracy::LegacyVersion& e) {
        throw std::runtime_error("legacy trace version " + std::to_string(e.version) +
            " -- too old for Tracy 0.13.0; re-save it with a newer Tracy");
    } catch (const tracy::LoadFailure& e) {
        throw std::runtime_error("trace load failure: " + e.msg);
    }
    while (!w->AreSourceLocationZonesReady()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto [ins, _] = g_cache.emplace(path, Capture{std::move(f), std::move(w)});
    return *ins->second.worker;
}

// Symbol statistics and other sample data are built by background jobs after loading.
void WaitForBackground(tracy::Worker& w) {
    while (!w.IsBackgroundDone()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool ContainsText(std::string hay, std::string needle) {
    auto low = [](std::string& s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
    };
    low(hay);
    low(needle);
    return hay.find(needle) != std::string::npos;
}

std::string PathArg(const json& args, const char* key) {
    if (!args.contains(key) || !args[key].is_string())
        throw std::runtime_error(std::string("missing required string arg: ") + key);
    return args[key].get<std::string>();
}

const char* ZoneName(tracy::Worker& w, int16_t srcloc) {
    const auto& sl = w.GetSourceLocation(srcloc);
    return w.GetString(sl.name.active ? sl.name : sl.function);
}

// Matches tracy-csvexport's std-dev computation exactly.
// `z` is Worker::SourceLocationZones (a private nested type) -- take it as an
// abbreviated template so we never name it textually.
double StdDev(const auto& z, int64_t total, double avg) {
    const auto n = z.zones.size();
    if (n <= 1) return 0.0;
    const double ss = z.sumSq - 2.0 * total * avg + avg * avg * (double)n;
    return std::sqrt(ss / (n - 1));
}

json ZoneRow(tracy::Worker& w, int16_t srcloc,
             const auto& z, bool self, int64_t lastTime) {
    const int64_t time = self ? z.selfTotal : z.total;
    const size_t count = z.zones.size();
    const double avg = count ? (double)time / count : 0.0;
    const auto& sl = w.GetSourceLocation(srcloc);
    return {
        {"name", ZoneName(w, srcloc)},
        {"file", w.GetString(sl.file)},
        {"line", sl.line},
        {"count", count},
        {"total_ns", time},
        {"percent", lastTime ? 100.0 * time / lastTime : 0.0},
        {"mean_ns", avg},
        {"min_ns", self ? z.selfMin : z.min},
        {"max_ns", self ? z.selfMax : z.max},
        {"std_ns", StdDev(z, z.total, avg)},
    };
}

json TextResult(const json& payload) {
    return {
        {"content", json::array({{{"type", "text"}, {"text", payload.dump(2)}}})},
        {"isError", false},
    };
}

// --- tools -------------------------------------------------------------

json CaptureInfo(const json& args) {
    auto& w = Load(PathArg(args, "path"));
    const auto* base = w.GetFramesBase();
    return TextResult({
        {"program", w.GetCaptureProgram()},
        {"capture_name", w.GetCaptureName()},
        {"duration_ns", w.GetLastTime() - w.GetFirstTime()},
        {"zone_count", w.GetZoneCount()},
        {"zone_types", w.GetSourceLocationZones().size()},
        {"thread_count", w.GetThreadData().size()},
        {"frame_count", base ? w.GetFrameCount(*base) : 0},
        {"frame_image_count", w.GetFrameImageCount()},
        {"callstack_samples", w.GetCallstackSampleCount()},
    });
}

json Hotspots(const json& args) {
    auto& w = Load(PathArg(args, "path"));
    const int top_n = args.value("top_n", 15);
    const bool self = args.value("self_time", false);
    const int64_t last = w.GetLastTime();

    const auto& slz = w.GetSourceLocationZones();
    using ZPtr = decltype(&slz.begin()->second);  // pointer to the private type, via deduction
    std::vector<std::pair<int16_t, ZPtr>> rows;
    for (const auto& [id, z] : slz) {
        if (z.total != 0) rows.emplace_back(id, &z);
    }
    std::sort(rows.begin(), rows.end(), [self](auto& a, auto& b) {
        return (self ? a.second->selfTotal : a.second->total) >
               (self ? b.second->selfTotal : b.second->total);
    });
    if ((int)rows.size() > top_n) rows.resize(top_n);

    json arr = json::array();
    for (auto& [id, z] : rows) arr.push_back(ZoneRow(w, id, *z, self, last));
    return TextResult({{"sort", self ? "self_time" : "total_time"}, {"zones", arr}});
}

json Zone(const json& args) {
    auto& w = Load(PathArg(args, "path"));
    const std::string needle = PathArg(args, "name");
    const bool self = args.value("self_time", false);
    const bool cs = args.value("case_sensitive", false);
    const int64_t last = w.GetLastTime();

    auto contains = [&](const char* hay) {
        std::string h = hay, n = needle;
        if (!cs) {
            auto low = [](std::string& s) {
                std::transform(s.begin(), s.end(), s.begin(),
                               [](unsigned char c) { return std::tolower(c); });
            };
            low(h); low(n);
        }
        return h.find(n) != std::string::npos;
    };

    json arr = json::array();
    for (const auto& [id, z] : w.GetSourceLocationZones()) {
        if (z.total != 0 && contains(ZoneName(w, id)))
            arr.push_back(ZoneRow(w, id, z, self, last));
    }
    std::sort(arr.begin(), arr.end(), [](const json& a, const json& b) {
        return a["total_ns"].get<int64_t>() > b["total_ns"].get<int64_t>();
    });
    return TextResult({{"query", needle}, {"matches", arr.size()}, {"zones", arr}});
}

json CompareCaptures(const json& args) {
    auto& a = Load(PathArg(args, "path_a"));
    auto& b = Load(PathArg(args, "path_b"));
    const int top_n = args.value("top_n", 15);
    const bool self = args.value("self_time", false);

    auto totals = [self](tracy::Worker& w) {
        std::map<std::string, int64_t> m;
        for (const auto& [id, z] : w.GetSourceLocationZones())
            m[ZoneName(w, id)] += self ? z.selfTotal : z.total;
        return m;
    };
    auto ma = totals(a), mb = totals(b);

    struct Diff { std::string name; int64_t a, b, delta; };
    std::vector<Diff> diffs;
    std::map<std::string, bool> keys;
    for (auto& [k, _] : ma) keys[k] = true;
    for (auto& [k, _] : mb) keys[k] = true;
    for (auto& [k, _] : keys) {
        int64_t va = ma.count(k) ? ma[k] : 0;
        int64_t vb = mb.count(k) ? mb[k] : 0;
        diffs.push_back({k, va, vb, vb - va});
    }
    std::sort(diffs.begin(), diffs.end(), [](const Diff& x, const Diff& y) {
        return std::llabs(x.delta) > std::llabs(y.delta);
    });
    if ((int)diffs.size() > top_n) diffs.resize(top_n);

    json arr = json::array();
    for (auto& d : diffs) {
        arr.push_back({
            {"name", d.name},
            {"a_ns", d.a},
            {"b_ns", d.b},
            {"delta_ns", d.delta},
            {"delta_pct", d.a ? 100.0 * d.delta / d.a : (d.b ? 100.0 : 0.0)},
        });
    }
    return TextResult({{"sort", self ? "self_time" : "total_time"},
                       {"note", "delta = b - a (positive means slower in b)"},
                       {"zones", arr}});
}

// Same labels as the GUI: the main set, a monitor's vsync, or the set's own name.
std::string FrameSetName(tracy::Worker& w, const tracy::FrameData& fd) {
    if (fd.name == 0) return "Frames";
    if (fd.name >> 63) return "[" + std::to_string(uint32_t(fd.name)) + "] Vsync";
    return w.GetString(fd.name);
}

// Calls `f` for each sample of a thread except context-switch samples, which the statistics
// skip. Same matching as the Worker's background job that builds the symbol statistics.
template <typename F>
void ForEachCpuSample(const tracy::ThreadData& td, F&& f) {
    auto ctx = td.ctxSwitchSamples.begin();
    for (const auto& sample : td.samples) {
        const auto time = sample.time.Val();
        ctx = std::lower_bound(ctx, td.ctxSwitchSamples.end(), time,
                               [](const auto& l, int64_t r) { return l.time.Val() < r; });
        if (ctx != td.ctxSwitchSamples.end() && ctx->time.Val() == time) continue;
        f(sample);
    }
}

uint64_t ThreadSamples(const tracy::ThreadData& td) {
    uint64_t n = 0;
    ForEachCpuSample(td, [&](const auto&) { n++; });
    return n;
}

json BusiestThreads(tracy::Worker& w, size_t n) {
    std::vector<const tracy::ThreadData*> threads;
    for (const auto* td : w.GetThreadData())
        if (ThreadSamples(*td)) threads.push_back(td);
    std::sort(threads.begin(), threads.end(), [](auto* a, auto* b) {
        return ThreadSamples(*a) > ThreadSamples(*b);
    });
    if (threads.size() > n) threads.resize(n);
    json arr = json::array();
    for (const auto* td : threads)
        arr.push_back({{"id", td->id}, {"name", w.GetThreadName(td->id)}, {"samples", ThreadSamples(*td)}});
    return arr;
}

// `wanted` is a thread id or a case-insensitive substring of the thread's name.
const tracy::ThreadData& FindThread(tracy::Worker& w, const std::string& wanted) {
    std::vector<const tracy::ThreadData*> hits;
    for (const auto* td : w.GetThreadData()) {
        if (std::to_string(td->id) == wanted) return *td;
        if (ThreadSamples(*td) && ContainsText(w.GetThreadName(td->id), wanted)) hits.push_back(td);
    }
    if (hits.size() == 1) return *hits.front();
    json names = json::array();
    for (const auto* td : hits) names.push_back({{"id", td->id}, {"name", w.GetThreadName(td->id)}});
    throw std::runtime_error(
        (hits.empty() ? "no sampled thread matches '" : "several threads match '") + wanted +
        "'; pass a thread id instead. " + (hits.empty() ? BusiestThreads(w, 15) : names).dump());
}

struct SampleCounts { uint32_t incl = 0, excl = 0; };

// The Worker's per-symbol counting (UpdateSampleStatisticsImpl), for one thread: self goes to
// the innermost symbol of the top frame, inclusive to every distinct symbol on the stack.
std::unordered_map<uint64_t, SampleCounts> CountThreadSamples(tracy::Worker& w,
                                                             const tracy::ThreadData& td,
                                                             uint64_t& total) {
    std::unordered_map<uint64_t, SampleCounts> counts;
    std::unordered_set<uint64_t> seen;
    total = 0;
    ForEachCpuSample(td, [&](const tracy::SampleData& sample) {
        total++;
        const auto& stack = w.GetCallstack(sample.callstack.Val());
        seen.clear();
        for (size_t c = 0; c < stack.size(); c++) {
            const auto* frames = w.GetCallstackFrame(stack[c]);
            if (!frames) {
                if (c == 0) counts[0].excl++;  // top frame never resolved: "[unknown]"
                continue;
            }
            for (uint8_t f = 0; f < frames->size; f++) {
                const uint64_t sym = frames->data[f].symAddr;
                if (c == 0 && f == 0) counts[sym].excl++;
                if (seen.insert(sym).second) counts[sym].incl++;
            }
        }
    });
    return counts;
}

// Mirrors the GUI's Statistics -> Sampling view with its default settings (whole capture,
// all threads, inlined functions folded into the function they were inlined into).
json Samples(const json& args) {
    auto& w = Load(PathArg(args, "path"));
    WaitForBackground(w);
    const int top_n = args.value("top_n", 20);
    const bool self = args.value("self_time", true);
    const bool separate = args.value("separate_inlines", false);
    const std::string filter = args.value("name", std::string());
    const std::string threadArg = args.value("thread", std::string());

    std::unordered_map<uint64_t, SampleCounts> counts;
    uint64_t total = 0;
    json scope = "all threads";
    if (threadArg.empty()) {
        // Context-switch samples do not count, as in the GUI.
        const uint64_t all = w.GetCallstackSampleCount();
        const uint64_t ctx = w.GetContextSwitchSampleCount();
        total = all > ctx ? all - ctx : 0;
        for (const auto& [addr, stat] : w.GetSymbolStats()) counts[addr] = {stat.incl, stat.excl};
    } else {
        const auto& td = FindThread(w, threadArg);
        counts = CountThreadSamples(w, td, total);
        scope = {{"id", td.id}, {"name", w.GetThreadName(td.id)}};
    }
    const int64_t period = w.GetSamplingPeriod();
    if (total == 0) {
        return TextResult({{"total_samples", 0},
                           {"note", "no call-stack samples in this capture (sampling off, or "
                                    "the profiled program lacked the privileges for it)"}});
    }

    struct Row { uint64_t addr; uint32_t incl, excl, inlines; };
    std::unordered_map<uint64_t, Row> rows;
    for (const auto& [addr, stat] : counts) {
        const auto* sym = w.GetSymbolData(addr);
        uint64_t base = addr;
        if (!separate && sym && sym->isInline) {
            const uint64_t parent = w.GetSymbolForAddress(addr);
            if (parent != 0) base = parent;
        }
        // A base symbol is on the stack whenever one of its inlines is, so its own inclusive
        // count is already the folded one; self counts add up.
        const bool isBase = base == addr;
        auto& row = rows.try_emplace(base, Row{base, 0, 0, 0}).first->second;
        if (isBase) row.incl = stat.incl;
        else row.inlines++;
        row.excl += stat.excl;
    }

    std::vector<Row> sorted;
    for (const auto& [_, r] : rows) {
        const auto* sym = w.GetSymbolData(r.addr);
        const char* name = sym ? w.GetString(sym->name) : "[unknown]";
        if (!filter.empty() && !ContainsText(name, filter)) continue;
        if ((self ? r.excl : r.incl) != 0) sorted.push_back(r);
    }
    std::sort(sorted.begin(), sorted.end(), [self](const Row& a, const Row& b) {
        const auto x = self ? a.excl : a.incl;
        const auto y = self ? b.excl : b.incl;
        return x != y ? x > y : a.addr < b.addr;
    });
    if ((int)sorted.size() > top_n) sorted.resize(top_n);

    json arr = json::array();
    for (const auto& r : sorted) {
        const auto* sym = w.GetSymbolData(r.addr);
        json row = {
            {"name", sym ? w.GetString(sym->name) : "[unknown]"},
            {"image", sym ? w.GetString(sym->imageName) : ""},
            {"file", sym ? w.GetString(sym->file) : ""},
            {"line", sym ? sym->line : 0},
            {"self_samples", r.excl},
            {"self_percent", 100.0 * r.excl / total},
            {"self_ns", (int64_t)r.excl * period},
            {"incl_samples", r.incl},
            {"incl_percent", 100.0 * r.incl / total},
            {"incl_ns", (int64_t)r.incl * period},
        };
        if (r.addr >> 63) row["kernel"] = true;
        if (r.inlines) row["inlined_functions_folded"] = r.inlines;
        arr.push_back(row);
    }
    return TextResult({
        {"sort", self ? "self_samples" : "incl_samples"},
        {"thread", scope},
        {"total_samples", total},
        {"sampling_period_ns", period},
        {"note", "self = samples with the function on top of the stack, "
                 "incl = samples with it anywhere on the stack"},
        {"functions", arr},
        {"busiest_threads", BusiestThreads(w, 8)},
    });
}

// Frame-time distribution of one frame set (default: the main FrameMark set).
json Frames(const json& args) {
    auto& w = Load(PathArg(args, "path"));
    const std::string wanted = args.value("name", std::string());

    const tracy::FrameData* fd = nullptr;
    json sets = json::array();
    for (const auto* f : w.GetFrames()) {
        const std::string n = FrameSetName(w, *f);
        sets.push_back(n);
        if (wanted.empty() ? f == w.GetFramesBase() : wanted == n) fd = f;
    }
    if (!fd) throw std::runtime_error("no frame set named '" + wanted + "'; sets: " + sets.dump());

    // For continuous sets the first frame runs from program start to the first mark and
    // the last one is cut off by the end of the capture; neither is a real frame.
    const size_t count = w.GetFrameCount(*fd);
    const size_t first = fd->continuous ? 1 : 0;
    const size_t last = fd->continuous && count > 0 ? count - 1 : count;
    std::vector<int64_t> times;
    for (size_t i = first; i < last; i++) {
        const int64_t t = w.GetFrameTime(*fd, i);
        if (t > 0) times.push_back(t);
    }
    if (times.empty()) return TextResult({{"frame_set", FrameSetName(w, *fd)}, {"frame_sets", sets}, {"frames", 0}});
    std::sort(times.begin(), times.end());

    double sum = 0, sumSq = 0;
    for (auto t : times) {
        sum += (double)t;
        sumSq += (double)t * (double)t;
    }
    const double n = (double)times.size();
    const double mean = sum / n;
    auto pct = [&](double p) {
        return times[std::min(times.size() - 1, (size_t)(p * (n - 1) + 0.5))];
    };
    auto over = [&](int64_t limit) {
        return std::count_if(times.begin(), times.end(), [limit](int64_t t) { return t > limit; });
    };
    return TextResult({
        {"frame_set", FrameSetName(w, *fd)},
        {"frame_sets", sets},
        {"frames", times.size()},
        {"mean_ns", mean},
        {"median_ns", pct(0.5)},
        {"p90_ns", pct(0.9)},
        {"p99_ns", pct(0.99)},
        {"min_ns", times.front()},
        {"max_ns", times.back()},
        {"std_ns", times.size() > 1 ? std::sqrt(std::max(0.0, (sumSq - sum * mean) / (n - 1))) : 0.0},
        {"fps_from_mean", mean > 0 ? 1e9 / mean : 0.0},
        {"over_16_7ms", over(16666667)},
        {"over_33_3ms", over(33333333)},
    });
}

}  // namespace

json ToolList() {
    json pathProp = {{"type", "string"}, {"description", "Absolute path to a .tracy capture file"}};
    return json::array({
        {{"name", "capture_info"},
         {"description", "Summary of a .tracy capture: program, duration, zone/thread/frame counts."},
         {"inputSchema", {{"type", "object"}, {"required", {"path"}},
                          {"properties", {{"path", pathProp}}}}}},
        {{"name", "hotspots"},
         {"description", "Top zones by time. The first tool to reach for: 'what is slow?'."},
         {"inputSchema", {{"type", "object"}, {"required", {"path"}}, {"properties", {
             {"path", pathProp},
             {"top_n", {{"type", "integer"}, {"description", "How many zones (default 15)"}}},
             {"self_time", {{"type", "boolean"}, {"description", "Rank by self time (exclude children). Default false."}}}}}}}},
        {{"name", "zone"},
         {"description", "Stats for zones whose name matches a substring."},
         {"inputSchema", {{"type", "object"}, {"required", {"path", "name"}}, {"properties", {
             {"path", pathProp},
             {"name", {{"type", "string"}, {"description", "Substring of the zone name"}}},
             {"self_time", {{"type", "boolean"}}},
             {"case_sensitive", {{"type", "boolean"}}}}}}}},
        {{"name", "compare_captures"},
         {"description", "Diff zone times between two captures (b - a). Find regressions."},
         {"inputSchema", {{"type", "object"}, {"required", {"path_a", "path_b"}}, {"properties", {
             {"path_a", {{"type", "string"}, {"description", "Baseline .tracy"}}},
             {"path_b", {{"type", "string"}, {"description", "New .tracy to compare against baseline"}}},
             {"top_n", {{"type", "integer"}}},
             {"self_time", {{"type", "boolean"}}}}}}}},
        {{"name", "samples"},
         {"description", "Top functions by call-stack samples (the GUI's Statistics -> Sampling view). "
                         "Covers code without zones: libraries, drivers, the kernel."},
         {"inputSchema", {{"type", "object"}, {"required", {"path"}}, {"properties", {
             {"path", pathProp},
             {"top_n", {{"type", "integer"}, {"description", "How many functions (default 20)"}}},
             {"self_time", {{"type", "boolean"}, {"description", "Rank by self samples (default true); false ranks by inclusive samples"}}},
             {"separate_inlines", {{"type", "boolean"}, {"description", "List inlined functions on their own instead of folding them into their caller. Default false."}}},
             {"name", {{"type", "string"}, {"description", "Only functions whose name contains this (case-insensitive)"}}},
             {"thread", {{"type", "string"}, {"description", "Only this thread: its id, or part of its name. Default: all threads. The result lists the busiest threads."}}}}}}}},
        {{"name", "frames"},
         {"description", "Frame-time distribution: mean, median, p90, p99, min, max, frames over 16.7/33.3 ms."},
         {"inputSchema", {{"type", "object"}, {"required", {"path"}}, {"properties", {
             {"path", pathProp},
             {"name", {{"type", "string"}, {"description", "Frame set name as listed in frame_sets (e.g. '[0] Vsync'); omit for the main FrameMark set"}}}}}}}},
    });
}

json CallTool(const std::string& name, const json& args) {
    if (name == "capture_info") return CaptureInfo(args);
    if (name == "hotspots") return Hotspots(args);
    if (name == "zone") return Zone(args);
    if (name == "compare_captures") return CompareCaptures(args);
    if (name == "samples") return Samples(args);
    if (name == "frames") return Frames(args);
    throw std::runtime_error("unknown tool: " + name);
}

}  // namespace tracymcp
