// XCP self-checks and a small command line master.
//
//   caneo_xcp_test --selftest                      value encode / decode
//   caneo_xcp_test --elf <file> [filter]           list the variables of an ELF
//   caneo_xcp_test --a2l <file> [--elf <file>] [filter]   list the variables of an A2L (addresses from the ELF)
//   caneo_xcp_test --iface vcan0 --tx 0x7F0 --rx 0x7F1 [--fd] [--nopad] (--elf <file> | --a2l <file>)
//                  [--read <var>]... [--write <var>=<value>]... [--poll <var>]... [--daq <var>:<event>]...
//                  [--seconds N]
#include "xcp/A2lReader.hpp"
#include "xcp/ElfReader.hpp"
#include "xcp/XcpMaster.hpp"
#include "compat/print.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <map>
#include <optional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;

static void check(bool ok, const std::string& what)
{
    if (!ok) {
        std::fputs(std::format("FAIL: {}\n", what).c_str(), stderr);
        ++failures;
    }
}

static int selftest()
{
    for (bool big : {false, true}) {
        for (XcpType t : {XcpType::U8, XcpType::I8, XcpType::U16, XcpType::I16, XcpType::U32, XcpType::I32,
                          XcpType::I64, XcpType::F32, XcpType::F64}) {
            XcpVar v;
            v.type = t;
            v.size = xcp_type_size(t);
            const double val = xcp_type_float(t) ? -12.5 : xcp_type_signed(t) ? -100.0 : 200.0;
            const auto bytes = xcp_encode(val, v, big);
            check(bytes.size() == v.size, std::format("{} size", to_string(t)));
            check(xcp_decode(bytes.data(), v, big) == val, std::format("{} roundtrip ({})", to_string(t), big ? "BE" : "LE"));
        }
        // bitfield: 3 bits at bit 5 of a 16 bit storage unit
        XcpVar b;
        b.type = XcpType::U16; b.size = 2; b.bit_offset = 5; b.bit_size = 3;
        const std::vector<uint8_t> cur = big ? std::vector<uint8_t>{0xFF, 0xFF} : std::vector<uint8_t>{0xFF, 0xFF};
        const auto w = xcp_encode(2.0, b, big, cur);
        check(xcp_decode(w.data(), b, big) == 2.0, "bitfield value");
        XcpVar rest = b; rest.bit_offset = 0; rest.bit_size = 5;
        check(xcp_decode(w.data(), rest, big) == 31.0, "bitfield neighbours untouched");
        XcpVar sb = b; sb.type = XcpType::I16;
        const auto sw = xcp_encode(-3.0, sb, big, cur);
        check(xcp_decode(sw.data(), sb, big) == -3.0, "signed bitfield");
    }
    XcpVar u8; u8.type = XcpType::U8; u8.size = 1;
    bool threw = false;
    try { xcp_encode(300.0, u8, false); } catch (const std::exception&) { threw = true; }
    check(threw, "range check");
    XcpVar le; le.type = XcpType::U32; le.size = 4;
    const auto lb = xcp_encode(0x11223344, le, false);
    check(lb == std::vector<uint8_t>{0x44, 0x33, 0x22, 0x11}, "little endian layout");
    // conversions: LINEAR phys = 0.1 * raw - 40 on an int16
    XcpVar lin; lin.type = XcpType::I16; lin.size = 2; lin.conv = {-40.0, 0.1, 1.0, 0.0};
    const auto lb2 = xcp_encode(25.0, lin, false);
    check(lb2 == std::vector<uint8_t>{0x8A, 0x02}, "linear encode (raw 650)");
    check(std::abs(xcp_decode(lb2.data(), lin, false) - 25.0) < 1e-9, "linear decode");
    threw = false;
    try { xcp_encode(4000.0, lin, false); } catch (const std::exception&) { threw = true; }
    check(threw, "linear range check on the raw value");

    // A2L parsing
    const char* a2l = R"A2L(ASAP2_VERSION 1 71
/begin PROJECT P "" /begin MODULE ecu "the ECU"
  /begin MOD_COMMON "" BYTE_ORDER MSB_FIRST /end MOD_COMMON
  /begin COMPU_METHOD cm_temp "temp" LINEAR "%6.1" "degC" COEFFS_LINEAR 0.1 -40 /end COMPU_METHOD
  /begin COMPU_METHOD cm_rat "" RAT_FUNC "%6.2" "rpm" COEFFS 0 2 0 0 0 1 /end COMPU_METHOD
  /begin COMPU_METHOD cm_mode "" TAB_VERB "%3.0" "" COMPU_TAB_REF vt_mode /end COMPU_METHOD
  /begin COMPU_VTAB vt_mode "" TAB_VERB 2 0 "OFF" 1 "ON \"fast\"" /end COMPU_VTAB
  /begin RECORD_LAYOUT rl_f32 FNC_VALUES 1 FLOAT32_IEEE COLUMN_DIR DIRECT /end RECORD_LAYOUT
  /* comment with /begin inside */
  /begin MEASUREMENT model_B.Temp "model/Sensor/Temp" SWORD cm_temp 0 0 -40 100
    ECU_ADDRESS 0x1000 /begin IF_DATA XCP /begin DAQ_EVENT FIXED_EVENT_LIST EVENT 1 /end DAQ_EVENT /end IF_DATA
  /end MEASUREMENT
  /begin MEASUREMENT speed "" UWORD cm_rat 0 0 0 30000 ECU_ADDRESS 0x1002 READ_WRITE /end MEASUREMENT
  /begin MEASUREMENT mode "" UBYTE cm_mode 0 0 0 1 ECU_ADDRESS 0x1004 BIT_MASK 0x0C /end MEASUREMENT
  /begin CHARACTERISTIC model_P.Gain "model/Ctrl/Gain/Gain" VALUE 0x2000 rl_f32 0 NO_COMPU_METHOD -10 10 /end CHARACTERISTIC
  /begin CHARACTERISTIC model_P.Tab "model/Ctrl/Lookup/Table" VAL_BLK 0x2010 rl_f32 0 NO_COMPU_METHOD 0 1 MATRIX_DIM 2 3 /end CHARACTERISTIC
/end MODULE /end PROJECT)A2L";
    try {
        const A2lModule mod = parse_a2l(a2l, "test.a2l");
        check(mod.objects.size() == 5, std::format("a2l objects ({})", mod.objects.size()));
        check(mod.little_endian == false, "a2l byte order");
        const ElfImage img = a2l_image(mod, nullptr);
        const auto* t = img.find("model_B.Temp");
        check(t && t->address == 0x1000 && t->type == XcpType::I16 && t->unit == "degC" && t->read_only, "a2l measurement");
        check(t && std::abs(t->conv.to_phys(650) - 25.0) < 1e-9, "a2l linear conversion");
        check(t && t->description == "model/Sensor/Temp" && t->name == "Temp", "a2l long identifier");
        const auto* sp = img.find("speed");
        check(sp && !sp->read_only && std::abs(sp->conv.to_phys(100) - 50.0) < 1e-9, "a2l rat_func (raw = 2 * phys)");
        check(sp && std::abs(sp->conv.to_raw(50) - 100.0) < 1e-9, "a2l rat_func inverse");
        const auto* md = img.find("mode");
        check(md && md->bit_offset == 2 && md->bit_size == 2 && md->enumerators.size() == 2 &&
                  md->enumerators[1].second == "ON \"fast\"", "a2l bit mask + verbal table");
        const auto* g = img.find("model_P.Gain");
        check(g && g->type == XcpType::F32 && g->has_limits && g->upper == 10.0 && !g->read_only, "a2l characteristic");
        const auto* e = img.find("model_P.Tab[1][2]");
        check(e && e->address == 0x2010 + 5 * 4, "a2l matrix element address");
        check(img.variables.size() == 3 && img.variables[0].name == "model", "a2l tree by long identifier");
    } catch (const std::exception& ex) {
        check(false, std::format("a2l parse: {}", ex.what()));
    }
    bool bad = false;
    try { parse_a2l("/begin PROJECT x /end MODULE"); } catch (const std::exception&) { bad = true; }
    check(bad, "a2l syntax error detected");

    std::puts(failures ? "selftest FAILED" : "selftest ok");
    return failures ? 1 : 0;
}

static void print_var(const ElfVariable& v, const std::string& filter, int depth)
{
    if (v.leaf() && (filter.empty() || v.path.find(filter) != std::string::npos || v.description.find(filter) != std::string::npos))
        std::println("{:<34} 0x{:08X} {:>3} {:<7}{}{}  {}", v.path, v.address, v.size, to_string(v.type),
                     v.bit_size ? std::format(" bits {}+{}", v.bit_offset, v.bit_size) : "",
                     v.unit.empty() ? "" : " [" + v.unit + "]", v.description.empty() ? v.type_name : v.description);
    for (const auto& c : v.children) print_var(c, filter, depth + 1);
}

int main(int argc, char* argv[])
{
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        std::puts("usage: see the comment at the top of tests/xcp_test.cpp");
        return 2;
    }
    if (args[0] == "--selftest") return selftest();

    std::string elf_path, a2l_path, iface;
    XcpTargetConfig target;
    std::vector<std::string> reads, polls;
    std::vector<std::pair<std::string, double>> writes;
    std::vector<std::pair<std::string, int>> daqs;
    double seconds = 2.0;
    std::string filter;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto next = [&] { return i + 1 < args.size() ? args[++i] : std::string{}; };
        const std::string& a = args[i];
        if (a == "--elf") elf_path = next();
        else if (a == "--a2l") a2l_path = next();
        else if (a == "--iface") target.interface = next();
        else if (a == "--tx") target.tx_id = static_cast<uint32_t>(std::stoul(next(), nullptr, 0));
        else if (a == "--rx") target.rx_id = static_cast<uint32_t>(std::stoul(next(), nullptr, 0));
        else if (a == "--fd") target.fd = true;
        else if (a == "--nopad") target.pad = false;
        else if (a == "--pad") target.pad = true;
        else if (a == "--read") reads.push_back(next());
        else if (a == "--poll") polls.push_back(next());
        else if (a == "--seconds") seconds = std::stod(next());
        else if (a == "--write") {
            const auto s = next();
            const auto eq = s.find('=');
            writes.emplace_back(s.substr(0, eq), std::stod(s.substr(eq + 1)));
        } else if (a == "--daq") {
            const auto s = next();
            const auto c = s.rfind(':');
            daqs.emplace_back(s.substr(0, c), std::stoi(s.substr(c + 1)));
        } else filter = a;
    }

    ElfImage img;
    if (!a2l_path.empty()) {
        const A2lModule mod = load_a2l(a2l_path);
        std::println("{}", mod);
        std::optional<ElfImage> elf;
        if (!elf_path.empty()) elf = load_elf(elf_path);
        img = a2l_image(mod, elf ? &*elf : nullptr);
    } else {
        img = load_elf(elf_path);
    }
    std::println("{}", img);
    for (const auto& w : img.warnings) std::println("warning: {}", w);
    if (target.interface.empty()) {
        for (const auto& v : img.variables) print_var(v, filter, 0);
        return 0;
    }

    const auto var = [&](const std::string& name, int event) {
        const auto* v = img.find(name);
        if (!v) throw std::runtime_error("unknown variable " + name);
        return XcpVar::from(*v, 0, event);
    };

    XcpMaster m(target);
    std::mutex mu;
    std::map<std::string, std::pair<int, double>> seen;   // count, last
    m.set_sample_fn([&](const std::string& n, double t, double v) {
        std::lock_guard l(mu);
        auto& s = seen[n];
        ++s.first;
        s.second = v;
        (void)t;
    });
    m.connect();
    for (int i = 0; i < 100 && m.state() == XcpMaster::State::Connecting || m.state() == XcpMaster::State::Disconnected; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::println("state: {} - {}", to_string(m.state()), m.status());
    std::println("{}", m.info());
    for (const auto& e : m.events()) std::println("event {}: {} ({} s)", e.number, e.name, e.cycle_s);

    for (const auto& r : reads) m.read(var(r, -1));
    for (const auto& [n, v] : writes) m.write(var(n, -1), v);
    std::vector<XcpVar> meas;
    for (const auto& p : polls) meas.push_back(var(p, -1));
    for (const auto& [n, e] : daqs) meas.push_back(var(n, e));
    if (!meas.empty()) m.set_measurements(meas, 20);
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    std::println("rate: {:.1f} samples/s", m.rate_hz());
    for (const auto& msg : m.take_messages()) std::println("  {}", msg);
    std::lock_guard l(mu);
    for (const auto& [n, s] : seen) std::println("{:<40} {:>6} samples, last {}", n, s.first, s.second);
    m.disconnect();
    return m.state() == XcpMaster::State::Error ? 1 : 0;
}
