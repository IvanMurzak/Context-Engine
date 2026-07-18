// The L-47 Tracy/Perfetto trace export implementation (see trace_export.h). Emits the Chrome Trace
// Event Format by hand (no JSON dependency) — the format is tiny and fixed, and keeping the profile
// module free of the contract JSON layer preserves its STL-only footprint.

#include "context/runtime/profile/trace_export.h"

#include <array>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace context::runtime::profile
{
namespace
{
// A lane's Chrome-trace thread id (each scheduler lane is its own track; L-38 maps a lane to a
// thread). The GC channel gets a dedicated track well clear of the lane ids.
constexpr int kGcTid = 10;

int lane_tid(Lane lane) noexcept { return static_cast<int>(lane) + 1; }

// Microseconds, 3-decimal fixed, formatted in an isolated stream so the caller's ostream flags are
// never mutated.
std::string fmt_us(double us)
{
    std::ostringstream s;
    s << std::fixed << std::setprecision(3) << us;
    return s.str();
}

std::string json_escape(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 2);
    for (const char c : in)
    {
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                std::ostringstream esc;
                esc << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(c));
                out += esc.str();
            }
            else
            {
                out += c;
            }
        }
    }
    return out;
}

// One complete ("ph":"X") duration event line.
std::string complete_event(const std::string& name, const std::string& cat, int tid, double ts_us,
                           double dur_us, std::uint64_t tick)
{
    std::ostringstream e;
    e << "{\"name\":\"" << json_escape(name) << "\",\"cat\":\"" << json_escape(cat)
      << "\",\"ph\":\"X\",\"pid\":1,\"tid\":" << tid << ",\"ts\":" << fmt_us(ts_us)
      << ",\"dur\":" << fmt_us(dur_us) << ",\"args\":{\"tick\":" << tick << "}}";
    return e.str();
}

std::string thread_name_event(int tid, const std::string& name)
{
    std::ostringstream e;
    e << "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":" << tid
      << ",\"args\":{\"name\":\"" << json_escape(name) << "\"}}";
    return e.str();
}
} // namespace

std::uint64_t write_chrome_trace(const SpanChannel& spans, const GcPauseChannel* gc,
                                 std::uint64_t tick_hz, std::ostream& out)
{
    const double hz = tick_hz > 0 ? static_cast<double>(tick_hz) : 60.0;
    const double tick_us = 1'000'000.0 / hz; // one fixed-timestep window

    std::vector<std::string> metadata;
    std::vector<std::string> events;

    // Thread-name metadata for each lane that actually ran (so the tracks are labelled in Tracy /
    // Perfetto), derived from the descriptor table.
    std::array<bool, 3> lane_used{false, false, false};
    for (const SystemDescriptor& d : spans.systems())
    {
        const std::size_t li = static_cast<std::size_t>(d.lane);
        if (li < lane_used.size())
            lane_used[li] = true;
    }
    for (std::size_t li = 0; li < lane_used.size(); ++li)
        if (lane_used[li])
            metadata.push_back(thread_name_event(lane_tid(static_cast<Lane>(li)),
                                                 lane_name(static_cast<Lane>(li))));

    // Per-(tick, lane) cursor: within a tick each lane lays its spans end-to-end from the tick base.
    // Index 0..2 = lanes, index 3 = the GC track.
    std::map<std::uint64_t, std::array<double, 4>> cursor;

    std::uint64_t event_count = 0;
    for (const SystemSpan& s : spans.samples())
    {
        if (s.system >= spans.systems().size())
            continue;
        const SystemDescriptor& d = spans.systems()[s.system];
        const std::size_t li = static_cast<std::size_t>(d.lane);
        if (li >= lane_used.size())
            continue;
        std::array<double, 4>& cur = cursor[s.tick];
        const double dur_us = s.duration_ms * 1000.0;
        const double ts_us = static_cast<double>(s.tick) * tick_us + cur[li];
        cur[li] += dur_us;
        events.push_back(
            complete_event(d.name, lane_name(d.lane), lane_tid(d.lane), ts_us, dur_us, s.tick));
        ++event_count;
    }

    if (gc != nullptr && !gc->samples().empty())
    {
        metadata.push_back(thread_name_event(kGcTid, "js-gc"));
        for (const GcPauseSample& p : gc->samples())
        {
            std::array<double, 4>& cur = cursor[p.tick];
            const double dur_us = p.duration_ms * 1000.0;
            const double ts_us = static_cast<double>(p.tick) * tick_us + cur[3];
            cur[3] += dur_us;
            const std::string name = p.in_window ? "gc.window" : "gc.mid-tick";
            events.push_back(complete_event(name, "js-gc", kGcTid, ts_us, dur_us, p.tick));
            ++event_count;
        }
    }

    out << "{\"traceEvents\":[";
    bool first = true;
    for (const std::string& m : metadata)
    {
        if (!first)
            out << ',';
        out << m;
        first = false;
    }
    for (const std::string& e : events)
    {
        if (!first)
            out << ',';
        out << e;
        first = false;
    }
    out << "],\"displayTimeUnit\":\"ms\",\"otherData\":{\"producer\":\"context-engine\",\"surface\":"
           "\"profile\"}}";

    return event_count;
}

} // namespace context::runtime::profile
