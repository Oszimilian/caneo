#include "LogDocument.hpp"

#include "config/Config.hpp"
#include "decoder/DbcPppWrapper.hpp"
#include "frame/CanFrame.hpp"
#include "logger/BlfReader.hpp"
#include "logger/LogReader.hpp"
#include "logger/McapReader.hpp"

#include <algorithm>
#include <format>

namespace {

using SeriesMap = std::map<std::string, LogDocument::Series>;

LogDocument::Series& series_for(SeriesMap& map, const std::string& iface, const std::string& msg,
                                const std::string& signal, const std::string& unit)
{
    const std::string label = iface + "/" + msg + "/" + signal;
    auto [it, inserted] = map.try_emplace(label);
    if (inserted) {
        it->second.iface  = iface;
        it->second.msg    = msg;
        it->second.signal = signal;
        it->second.unit   = unit;
        it->second.label  = label;
    }
    return it->second;
}

void flatten(SeriesMap& map, LogDocument::Data& d)
{
    for (auto& [label, s] : map) {
        if (!s.t.empty()) d.duration = std::max(d.duration, s.t.back());
        d.series.push_back(std::move(s));
    }
}

// BLF raw frames -> series, one DBC per interface.
void decode_raws(LogDocument::Data& d, const std::map<std::string, std::string>& dbcs, std::atomic<uint64_t>& progress)
{
    d.series.clear();
    d.unmatched.clear();
    d.dbc_errors.clear();
    d.duration = 0.0;

    std::map<uint16_t, std::unique_ptr<DbcPppWrapper>> decoders;
    for (const auto& [ch, iface] : d.channels) {
        const auto it = dbcs.find(iface);
        if (it == dbcs.end() || it->second.empty()) continue;
        try {
            decoders[ch] = std::make_unique<DbcPppWrapper>(expand_path(it->second));
        } catch (const std::exception& e) {
            d.dbc_errors[iface] = e.what();
        }
    }

    SeriesMap map;
    std::map<std::pair<uint16_t, uint32_t>, LogDocument::Unmatched> unmatched;
    for (const auto& raw : *d.raws) {
        ++progress;
        const auto dec = decoders.find(raw.channel);
        CanFrame frame(CanHeader{raw.id, d.channels[raw.channel], static_cast<uint8_t>(raw.data.size())}, raw.data);
        if (dec != decoders.end())
            dec->second->decode(frame);
        if (frame.msg_name().empty()) {
            auto& u = unmatched[{raw.channel, raw.id}];
            if (u.count++ == 0) {
                u.iface   = d.channels[raw.channel];
                u.id      = raw.id;
                u.has_dbc = dec != decoders.end();
                u.first_t = raw.t;
            }
            u.last_t    = raw.t;
            u.dlc       = raw.data.size();
            u.last_data = raw.data;
            continue;
        }
        for (const auto& sig : frame.decoded()) {
            auto& s = series_for(map, frame.header().interface, frame.msg_name(), sig.name, sig.unit);
            s.t.push_back(raw.t);
            s.v.push_back(sig.value);
        }
    }
    flatten(map, d);
    for (auto& [key, u] : unmatched) d.unmatched.push_back(std::move(u));
}

} // namespace

LogDocument::~LogDocument() = default;   // jthread joins

std::string LogDocument::status() const
{
    if (busy_) return std::format("loading... {} frames", progress_.load());
    std::lock_guard lock(mutex_);
    return status_;
}

bool LogDocument::poll()
{
    std::lock_guard lock(mutex_);
    if (!pending_) return false;
    data_    = std::move(*pending_);
    pending_.reset();
    return true;
}

void LogDocument::finish(Data&& d, std::string error)
{
    {
        std::lock_guard lock(mutex_);
        if (error.empty()) {
            d.base_series = d.series.size();
            status_ = std::format("{} signals, {:.1f} s", d.series.size(), d.duration);
            pending_ = std::move(d);
        } else {
            status_ = "error: " + error;
        }
    }
    busy_ = false;
}

void LogDocument::open(std::string path, std::vector<std::string> fallback_names,
                       std::map<std::string, std::string> dbcs)
{
    if (busy_) return;
    busy_     = true;
    progress_ = 0;
    worker_   = std::jthread([this, path = std::move(path), names = std::move(fallback_names),
                              dbcs = std::move(dbcs)] {
        Data d;
        try {
            d.path = path;
            auto reader = open_log_reader(path);

            if (auto* mcap = dynamic_cast<McapReader*>(reader.get())) {
                d.format = Format::Mcap;
                SeriesMap map;
                uint64_t  t0 = 0;
                bool      first = true;
                mcap->read_signals([&](uint64_t ts, const std::string& iface, const std::string& msg,
                                       const std::vector<DecodedSignal>& sigs) {
                    if (first) { t0 = ts; first = false; }
                    const double t = static_cast<double>(ts - t0) * 1e-9;
                    ++d.frames[iface];
                    ++progress_;
                    for (const auto& sig : sigs) {
                        auto& s = series_for(map, iface, msg, sig.name, sig.unit);
                        s.t.push_back(t);
                        s.v.push_back(sig.value);
                    }
                    return true;
                });
                for (const auto& [iface, n] : d.frames) d.interfaces.push_back(iface);
                flatten(map, d);
            } else if (auto* blf = dynamic_cast<BlfReader*>(reader.get())) {
                d.format = Format::Blf;
                blf->set_channel_names(names);
                auto raws = std::make_shared<std::vector<RawFrame>>();
                uint64_t t0 = 0;
                std::map<uint16_t, uint64_t> per_channel;
                blf->read([&](const BlfReader::RawFrame& fr) {
                    if (raws->empty()) t0 = fr.ts_ns;
                    raws->push_back({static_cast<double>(fr.ts_ns >= t0 ? fr.ts_ns - t0 : 0) * 1e-9,
                                     fr.channel, fr.id, fr.data});
                    ++per_channel[fr.channel];
                    ++progress_;
                    return true;
                });
                for (const auto& [ch, n] : per_channel) {
                    const std::string name = blf->channel_name(ch);
                    d.channels[ch]  = name;
                    d.frames[name] += n;
                    d.interfaces.push_back(name);
                }
                if (!raws->empty()) d.duration = raws->back().t;
                d.raws = std::move(raws);
                progress_ = 0;
                decode_raws(d, dbcs, progress_);
            }
        } catch (const std::exception& e) {
            finish(std::move(d), e.what());
            return;
        }
        finish(std::move(d), {});
    });
}

void LogDocument::decode(std::map<std::string, std::string> dbcs)
{
    if (busy_ || data_.format != Format::Blf) return;
    busy_     = true;
    progress_ = 0;
    Data base;   // everything but the (large) series
    base.path = data_.path;       base.format = data_.format;   base.interfaces = data_.interfaces;
    base.frames = data_.frames;   base.raws = data_.raws;       base.channels = data_.channels;
    worker_   = std::jthread([this, d = std::move(base), dbcs = std::move(dbcs)]() mutable {
        try {
            decode_raws(d, dbcs, progress_);
        } catch (const std::exception& e) {
            finish(std::move(d), e.what());
            return;
        }
        finish(std::move(d), {});
    });
}

void LogDocument::set_extra_series(std::vector<Series> extra)
{
    if (busy_) return;
    data_.series.resize(std::min(data_.base_series, data_.series.size()));
    for (auto& s : extra) data_.series.push_back(std::move(s));
}

std::ostream& operator<<(std::ostream& os, const LogDocument& doc)
{
    const auto& d = doc.data();
    return os << "LogDocument(" << d.path << ", " << d.series.size() << " signals, " << d.duration << " s)";
}
