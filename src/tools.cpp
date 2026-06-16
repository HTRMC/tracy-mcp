#include "tools.hpp"

#include "TracyFileRead.hpp"
#include "TracyWorker.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>
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
            std::to_string(v & 0xff) + " -- this build links Tracy 0.13.1; bump third_party/tracy to match");
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
    });
}

json CallTool(const std::string& name, const json& args) {
    if (name == "capture_info") return CaptureInfo(args);
    if (name == "hotspots") return Hotspots(args);
    if (name == "zone") return Zone(args);
    if (name == "compare_captures") return CompareCaptures(args);
    throw std::runtime_error("unknown tool: " + name);
}

}  // namespace tracymcp
