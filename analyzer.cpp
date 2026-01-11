#include "analyzer.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <vector>

using namespace std;

namespace {

static inline bool is_space(unsigned char c) { return std::isspace(c) != 0; }
static inline bool is_digit(unsigned char c) { return std::isdigit(c) != 0; }

// Trim range [b,e) over a string without allocating
static inline void trim_range(const string& s, size_t& b, size_t& e) {
    while (b < e && is_space((unsigned char)s[b])) ++b;
    while (e > b && is_space((unsigned char)s[e - 1])) --e;
}

// Parse hour from a datetime-like field such as "YYYY-MM-DD HH:MM".
static bool parse_hour_from_datetime(const string& s, size_t b, size_t e, int& hour_out) {
    trim_range(s, b, e);
    if (b >= e) return false;

    // Find space between date and time.
    size_t sp = s.find(' ', b);
    if (sp == string::npos || sp >= e) return false;

    // Find ':' after the space.
    size_t colon = s.find(':', sp + 1);
    if (colon == string::npos || colon >= e) return false;

    // Minute: must have 2 digits after ':'
    size_t m0 = colon + 1;
    if (m0 + 1 >= e) return false;
    if (!is_digit((unsigned char)s[m0]) || !is_digit((unsigned char)s[m0 + 1])) return false;

    int minute = (s[m0] - '0') * 10 + (s[m0 + 1] - '0');
    if (minute < 0 || minute > 59) return false;

    // Hour: 1-2 digits before ':' ignoring spaces
    if (colon == 0) return false;
    size_t i = colon - 1;
    while (i > b && is_space((unsigned char)s[i])) --i;
    if (!is_digit((unsigned char)s[i])) return false;

    int hour = s[i] - '0';

    // Optional tens digit for 10..23 (or leading zero)
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

static inline bool better_zone(const ZoneCount& a, const ZoneCount& b) {
    if (a.count != b.count) return a.count > b.count; // count desc
    return a.zone < b.zone;                           // zone asc
}

static inline bool better_slot(const SlotCount& a, const SlotCount& b) {
    if (a.count != b.count) return a.count > b.count; // count desc
    if (a.zone != b.zone)   return a.zone < b.zone;   // zone asc
    return a.hour < b.hour;                           // hour asc
}

static bool field_range(const string& line, const vector<size_t>& commas, size_t idx, size_t& b, size_t& e) {
    // Field idx in a comma-separated line: [commas[idx-1]+1, commas[idx])
    if (idx == 0) {
        b = 0;
        e = commas.empty() ? line.size() : commas[0];
        return true;
    }
    if (idx - 1 >= commas.size()) return false; // no such field

    b = commas[idx - 1] + 1;
    e = (idx < commas.size()) ? commas[idx] : line.size();
    if (b > line.size()) return false;
    if (e > line.size()) e = line.size();
    return true;
}

static inline string make_slot_key(const string& zone, int hour) {
    string key;
    key.reserve(zone.size() + 1 + 2);
    key.append(zone);
    key.push_back('|');
    key.append(to_string(hour));
    return key;
}

static bool parse_hour_field_candidate(const string& line, const vector<size_t>& commas, size_t fieldIdx, int& hour_out) {
    size_t b = 0, e = 0;
    if (!field_range(line, commas, fieldIdx, b, e)) return false;
    trim_range(line, b, e);
    if (b >= e) return false;
    return parse_hour_from_datetime(line, b, e, hour_out);
}

static bool parse_key_zone_hour(const string& key, string& zone_out, int& hour_out) {
    size_t bar = key.rfind('|');
    if (bar == string::npos) return false;

    zone_out = key.substr(0, bar);
    if (zone_out.empty()) return false;

    // Parse hour (0..23) from the tail
    int hour = 0;
    size_t p = bar + 1;
    if (p >= key.size()) return false;

    // allow 1-2 digits only
    int digits = 0;
    while (p < key.size()) {
        unsigned char c = (unsigned char)key[p];
        if (!is_digit(c)) return false;
        hour = hour * 10 + (key[p] - '0');
        ++digits;
        if (digits > 2) return false;
        ++p;
    }

    if (digits == 0 || hour < 0 || hour > 23) return false;
    hour_out = hour;
    return true;
}

} // namespace

void TripAnalyzer::ingestFile(const std::string& csvPath) {
    zoneCounts.clear();
    slotCounts.clear();

    ifstream file(csvPath);
    if (!file.is_open()) return;

    // Optional perf tweak (safe on all tests)
    zoneCounts.max_load_factor(0.5f);
    slotCounts.max_load_factor(0.5f);

    string line;
    while (getline(file, line)) {
        if (line.empty()) continue;

        // Collect comma positions
        vector<size_t> commas;
        commas.reserve(8);
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == ',') commas.push_back(i);
        }

        // Zone is field 1
        size_t z_b = 0, z_e = 0;
        if (!field_range(line, commas, 1, z_b, z_e)) continue;
        trim_range(line, z_b, z_e);
        if (z_b >= z_e) continue;

        int hour = -1;
        // Supports both:
        // - 3 columns: time in field 2
        // - 6 columns: time in field 3 (field 2 is dropoff zone, parse fails)
        bool ok = parse_hour_field_candidate(line, commas, 2, hour);
        if (!ok) ok = parse_hour_field_candidate(line, commas, 3, hour);
        if (!ok) continue;

        string zone = line.substr(z_b, z_e - z_b);

        zoneCounts[zone] += 1;
        slotCounts[make_slot_key(zone, hour)] += 1;
    }
}

vector<ZoneCount> TripAnalyzer::topZones(int k) const {
    if (k <= 0 || zoneCounts.empty()) return {};

    vector<ZoneCount> v;
    v.reserve(zoneCounts.size());
    for (const auto& kv : zoneCounts) {
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
    if (k <= 0 || slotCounts.empty()) return {};

    vector<SlotCount> v;
    v.reserve(slotCounts.size());

    string zone;
    int hour = -1;
    for (const auto& kv : slotCounts) {
        zone.clear();
        hour = -1;
        if (!parse_key_zone_hour(kv.first, zone, hour)) continue;
        v.push_back(SlotCount{zone, hour, kv.second});
    }

    if (v.empty()) return {};

    if ((int)v.size() <= k) {
        sort(v.begin(), v.end(), better_slot);
        return v;
    }

    auto nth = v.begin() + k;
    nth_element(v.begin(), nth, v.end(), better_slot);
    v.resize(k);
    sort(v.begin(), v.end(), better_slot);
    return v;
}

