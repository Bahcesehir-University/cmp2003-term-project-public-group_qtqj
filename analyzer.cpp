#include "analyzer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;

namespace {

// tiny helpers
static inline bool is_space(unsigned char c) { return std::isspace(c) != 0; }
static inline bool is_digit(unsigned char c) { return std::isdigit(c) != 0; }

// trim range [b,e) over a string without allocating
static inline void trim_range(const string& s, size_t& b, size_t& e) {
    while (b < e && is_space((unsigned char)s[b])) ++b;
    while (e > b && is_space((unsigned char)s[e - 1])) --e;
}

// Parse hour from "YYYY-MM-DD HH:MM" within [b, e)
// (kept consistent with your HackerRank-style logic)
static bool parse_hour_from_datetime(const string& s, size_t b, size_t e, int& hour_out) {
    trim_range(s, b, e);
    if (b >= e) return false;

    size_t sp = s.find(' ', b);
    if (sp == string::npos || sp >= e) return false;

    size_t colon = s.find(':', sp + 1);
    if (colon == string::npos || colon >= e) return false;

    // minute: 2 digits after ':'
    size_t m0 = colon + 1;
    if (m0 + 1 >= e) return false;
    if (!is_digit((unsigned char)s[m0]) || !is_digit((unsigned char)s[m0 + 1])) return false;

    int minute = (s[m0] - '0') * 10 + (s[m0 + 1] - '0');
    if (minute < 0 || minute > 59) return false;

    // hour: 1-2 digits before ':' ignoring spaces
    if (colon == 0) return false;
    size_t i = colon - 1;
    while (i > b && is_space((unsigned char)s[i])) --i;
    if (!is_digit((unsigned char)s[i])) return false;

    int hour = s[i] - '0';

    // optional tens digit for 10..23
    if (i > b) {
        size_t p = i - 1;
        while (p > b && is_space((unsigned char)s[p])) --p;
        if (is_digit((unsigned char)s[p])) {
            hour = (s[p] - '0') * 10 + hour;
        }
    }

    if (hour < 0 || hour > 23) return false;
    hour_out = hour;
    return true;
}

// deterministic tie-break sorting
static inline bool better_zone(const ZoneCount& a, const ZoneCount& b) {
    if (a.count != b.count) return a.count > b.count; // count desc
    return a.zone < b.zone;                           // zone asc
}

static inline bool better_slot(const SlotCount& a, const SlotCount& b) {
    if (a.count != b.count) return a.count > b.count; // count desc
    if (a.zone != b.zone)   return a.zone < b.zone;   // zone asc
    return a.hour < b.hour;                           // hour asc
}

} // namespace

void TripAnalyzer::ingestFile(const string& csvPath) {
    zoneCounts_.clear();
    slotCounts_.clear();

    ifstream file(csvPath);
    if (!file.is_open()) return;

    string line;

    // header line (may exist) - match HR behavior: read & discard first line
    if (!getline(file, line)) return;

    while (getline(file, line)) {
        if (line.empty()) continue;

        // Need at least 6 columns -> at least 5 commas:
        // 0 TripID, 1 PickupZoneID, 2 DropoffZoneID, 3 PickupDateTime, 4 DistanceKm, 5 FareAmount
        size_t c0 = line.find(',');
        if (c0 == string::npos) continue;
        size_t c1 = line.find(',', c0 + 1);
        if (c1 == string::npos) continue;
        size_t c2 = line.find(',', c1 + 1);
        if (c2 == string::npos) continue;
        size_t c3 = line.find(',', c2 + 1);
        if (c3 == string::npos) continue;
        size_t c4 = line.find(',', c3 + 1);
        if (c4 == string::npos) continue;

        // PickupZoneID: (c0+1 .. c1)
        size_t z_b = c0 + 1, z_e = c1;
        trim_range(line, z_b, z_e);
        if (z_b >= z_e) continue;

        // PickupDateTime: (c2+1 .. c3)
        size_t dt_b = c2 + 1, dt_e = c3;
        trim_range(line, dt_b, dt_e);
        if (dt_b >= dt_e) continue;

        int hour = -1;
        if (!parse_hour_from_datetime(line, dt_b, dt_e, hour)) continue;

        string zone = line.substr(z_b, z_e - z_b);

        zoneCounts_[zone] += 1;
        slotCounts_[zone][hour] += 1;
    }
}

vector<ZoneCount> TripAnalyzer::topZones(int k) const {
    if (k <= 0 || zoneCounts_.empty()) return {};

    vector<ZoneCount> v;
    v.reserve(zoneCounts_.size());
    for (const auto& kv : zoneCounts_) {
        v.push_back(ZoneCount{kv.first, kv.second});
    }

    if ((int)v.size() <= k) {
        sort(v.begin(), v.end(), better_zone);
        return v;
    }

    auto nth = v.begin() + k;
    nth_element(v.begin(), nth, v.end(), better_zone);
    v.resize(k);
    sort(v.begin(), v.end(), better_zone);
    return v;
}

vector<SlotCount> TripAnalyzer::topBusySlots(int k) const {
    if (k <= 0 || slotCounts_.empty()) return {};

    // keep k best in a heap where top() is the "worst among kept"
    auto worse = [](const SlotCount& a, const SlotCount& b) {
        return better_slot(a, b);
    };
    priority_queue<SlotCount, vector<SlotCount>, decltype(worse)> pq(worse);

    for (const auto& zk : slotCounts_) {
        const string& zone = zk.first;
        const auto& hoursMap = zk.second;

        for (const auto& hk : hoursMap) {
            int hour = hk.first;
            long long cnt = hk.second;
            if (cnt == 0) continue;

            SlotCount cand{zone, hour, cnt};

            if ((int)pq.size() < k) {
                pq.push(cand);
            } else if (better_slot(cand, pq.top())) {
                pq.pop();
                pq.push(cand);
            }
        }
    }

    vector<SlotCount> out;
    out.reserve(pq.size());
    while (!pq.empty()) {
        out.push_back(pq.top());
        pq.pop();
    }

    sort(out.begin(), out.end(), better_slot);
    return out;
}
