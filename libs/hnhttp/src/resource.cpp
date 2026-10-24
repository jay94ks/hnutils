#include <hnhttp/resource.hpp>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace hnhttp {
namespace {
using namespace std::chrono;
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}
struct Tag { std::string_view value; bool weak = false; };
bool tag(std::string_view& input, Tag& out) {
    out.weak = input.starts_with("W/");
    if (out.weak) input.remove_prefix(2);
    if (input.empty() || input.front() != '"') return false;
    input.remove_prefix(1);
    auto end = input.find('"');
    if (end == input.npos) return false;
    out.value = input.substr(0, end);
    for (unsigned char c : out.value) if (c < 0x21 || c == 0x7f) return false;
    input.remove_prefix(end + 1);
    return true;
}
bool singleTag(std::string_view value, Tag& out) {
    value = trim(value);
    return tag(value, out) && trim(value).empty();
}
/** Consume entire lists even after a match, to reject malformed trailing input. */
int matches(const std::vector<std::string>& fields, const Tag& current, bool exists, bool hasTag, bool strong) {
    bool matched = false, any = false;
    for (const auto& field : fields) {
        auto s = trim(field);
        if (s == "*") return fields.size() == 1 ? (exists ? 1 : 0) : -1;
        while (!s.empty()) {
            if (s.front() == ',') { s = trim(s.substr(1)); continue; }
            Tag candidate;
            if (!tag(s, candidate)) return -1;
            any = true;
            matched |= exists && hasTag && candidate.value == current.value &&
                       (!strong || (!candidate.weak && !current.weak));
            s = trim(s);
            if (!s.empty()) {
                if (s.front() != ',') return -1;
                s = trim(s.substr(1));
            }
        }
    }
    return any ? (matched ? 1 : 0) : -1;
}
int decimal(std::string_view s) {
    int n = 0;
    if (s.empty()) return -1;
    for (char c : s) { if (c < '0' || c > '9') return -1; n = n * 10 + c - '0'; }
    return n;
}
constexpr std::string_view months = "JanFebMarAprMayJunJulAugSepOctNovDec";
constexpr std::string_view weekdays = "SunMonTueWedThuFriSat";
int monthNumber(std::string_view s) {
    if (s.size() != 3) return 0;
    auto at = months.find(s);
    return at != months.npos && at % 3 == 0 ? static_cast<int>(at / 3 + 1) : 0;
}
std::optional<sys_seconds> date(std::string_view s) {
    int y = -1, m = 0, d = -1;
    std::string_view clock;
    auto shortDay = [](std::string_view name) {
        auto at = weekdays.find(name);
        return name.size() == 3 && at != weekdays.npos && at % 3 == 0;
    };
    // IMF-fixdate, obsolete RFC850 and ANSI C asctime are all accepted as HTTP dates.
    if (s.size() == 29 && s.substr(3, 2) == ", " && s[7] == ' ' && s[11] == ' ' &&
        s[16] == ' ' && s.substr(25) == " GMT") {
        if (!shortDay(s.substr(0, 3))) return {};
        y = decimal(s.substr(12, 4)); m = monthNumber(s.substr(8, 3));
        d = decimal(s.substr(5, 2)); clock = s.substr(17, 8);
    } else if (auto comma = s.find(','); comma != s.npos && s.size() - comma == 24) {
        auto rest = s.substr(comma + 1);
        constexpr std::string_view names[]{"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
        if (std::find(std::begin(names), std::end(names), s.substr(0, comma)) == std::end(names)) return {};
        if (rest[0] != ' ' || rest[3] != '-' || rest[7] != '-' || rest[10] != ' ' ||
            rest.substr(19) != " GMT") return {};
        d = decimal(rest.substr(1, 2)); m = monthNumber(rest.substr(4, 3));
        int yy = decimal(rest.substr(8, 2)); if (yy < 0) return {};
        int now = int(year_month_day{floor<days>(system_clock::now())}.year());
        y = (now / 100) * 100 + yy; if (y > now + 50) y -= 100;
        clock = rest.substr(11, 8);
    } else if (s.size() == 24 && s[3] == ' ' && s[7] == ' ' && s[10] == ' ' && s[19] == ' ') {
        if (!shortDay(s.substr(0, 3))) return {};
        y = decimal(s.substr(20, 4)); m = monthNumber(s.substr(4, 3));
        d = decimal(trim(s.substr(8, 2))); clock = s.substr(11, 8);
    } else return {};
    if (clock.size() != 8 || clock[2] != ':' || clock[5] != ':') return {};
    int h = decimal(clock.substr(0, 2)), min = decimal(clock.substr(3, 2)), sec = decimal(clock.substr(6, 2));
    year_month_day day{year{y}, month{static_cast<unsigned>(m)}, std::chrono::day{static_cast<unsigned>(d)}};
    if (y < 1601 || y > 9999 || !day.ok() || h < 0 || h > 23 || min < 0 || min > 59 || sec < 0 || sec > 59) return {};
    return sys_days{day} + hours{h} + minutes{min} + seconds{sec};
}
std::string dateText(sys_seconds t) {
    auto day = floor<days>(t);
    year_month_day ymd{day};
    int y = int(ymd.year());
    if (y < 1601 || y > 9999) throw std::invalid_argument("HTTP Last-Modified year");
    hh_mm_ss time{t - day}; char text[40];
    auto w = weekdays.substr(weekday{day}.c_encoding() * 3, 3);
    auto m = months.substr((unsigned(ymd.month()) - 1) * 3, 3);
    std::snprintf(text, sizeof(text), "%.*s, %02u %.*s %04d %02d:%02d:%02d GMT", 3, w.data(),
        unsigned(ymd.day()), 3, m.data(), y, int(time.hours().count()), int(time.minutes().count()), int(time.seconds().count()));
    return text;
}
bool integer(std::string_view s, std::uint64_t& out) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    if (r.ec == std::errc::result_out_of_range) out = UINT64_MAX;
    return r.ec == std::errc{} || r.ec == std::errc::result_out_of_range;
}
} // namespace

SResourceDecision CHttpResource::evaluate(EHttpMethod method, const IReadOnlyHttpHeaders& request,
                                         const SResourceMetadata& resource) {
    SResourceDecision out;
    out.length = resource.size;
    const bool safe = method == HTTP_GET || method == HTTP_HEAD;
    out.sendBody = method != HTTP_HEAD;
    Tag current;
    if (!resource.entityTag.empty() && !singleTag(resource.entityTag, current))
        throw std::invalid_argument("invalid representation ETag");
    if (resource.exists) {
        if (!resource.entityTag.empty()) out.headers.push_back({"ETag", resource.entityTag});
        if (resource.lastModified) out.headers.push_back({"Last-Modified", dateText(*resource.lastModified)});
        out.headers.push_back({"Accept-Ranges", "bytes"});
    }
    auto terminal = [&](std::uint16_t status) {
        out.status = status; out.sendBody = false; out.offset = out.length = 0; return out;
    };
    auto match = request.values("if-match", 0, 4096);
    if (!match.empty()) {
        int result = matches(match, current, resource.exists, !resource.entityTag.empty(), true);
        if (result < 0) return terminal(400);
        if (!result) return terminal(412);
    } else if (auto value = request.get("if-unmodified-since"); value && resource.exists && resource.lastModified) {
        if (auto t = date(*value); t && *resource.lastModified > *t) return terminal(412);
    }
    auto none = request.values("if-none-match", 0, 4096);
    if (!none.empty()) {
        int result = matches(none, current, resource.exists, !resource.entityTag.empty(), false);
        if (result < 0) return terminal(400);
        if (result) return terminal(safe ? 304 : 412);
    } else if (auto value = request.get("if-modified-since"); value && safe && resource.exists && resource.lastModified) {
        if (auto t = date(*value); t && *resource.lastModified <= *t) return terminal(304);
    }
    if (!resource.exists) return safe ? terminal(404) : out;
    if (method != HTTP_GET) return out;
    auto ranges = request.values("range", 0, 2);
    if (ranges.size() != 1) return out;
    auto validators = request.values("if-range", 0, 2);
    if (validators.size() > 1) return out;
    if (!validators.empty()) {
        const auto& value = validators.front();
        Tag validator;
        bool valid = singleTag(value, validator) && !resource.entityTag.empty() && !validator.weak &&
                     !current.weak && validator.value == current.value;
        if (!valid && resource.strongLastModified && resource.lastModified) {
            auto t = date(value); valid = t && *t == *resource.lastModified;
        }
        if (!valid) return out;
    }
    auto range = trim(ranges.front());
    if (range.size() < 6) return out;
    constexpr std::string_view unit = "bytes=";
    for (std::size_t i = 0; i < 6; ++i) {
        auto c = range[i]; if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != unit[i]) return out;
    }
    range.remove_prefix(6);
    auto dash = range.find('-');
    if (dash == range.npos || range.find(',') != range.npos) return out;
    std::uint64_t first = 0, last = 0;
    bool unsatisfied = !resource.size;
    if (dash == 0) {
        if (!integer(range.substr(1), last)) return out;
        unsatisfied |= last == 0;
        out.length = (std::min)(last, resource.size); out.offset = resource.size - out.length;
    } else {
        if (!integer(range.substr(0, dash), first)) return out;
        auto end = range.substr(dash + 1);
        if (!end.empty() && (!integer(end, last) || last < first)) return out;
        unsatisfied |= first >= resource.size;
        last = end.empty() ? (resource.size ? resource.size - 1 : 0) :
                            (std::min)(last, resource.size ? resource.size - 1 : 0);
        out.offset = first; out.length = unsatisfied ? 0 : last - first + 1;
    }
    if (unsatisfied) {
        out.headers.push_back({"Content-Range", "bytes */" + std::to_string(resource.size)});
        return terminal(416);
    }
    out.status = 206;
    out.headers.push_back({"Content-Range", "bytes " + std::to_string(out.offset) + "-" +
        std::to_string(out.offset + out.length - 1) + "/" + std::to_string(resource.size)});
    return out;
}
} // namespace hnhttp
