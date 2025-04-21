#include "constant.h"

caneo::Constant::Constant() = default;

void caneo::Constant::set_start_time(std::chrono::time_point<std::chrono::steady_clock> start_time) {
    m_start_time = start_time;
}

void caneo::Constant::set_period(std::chrono::milliseconds period) {
    m_period = period;
}

void caneo::Constant::set_constant(double value) {
    m_value = value;
}

void caneo::Constant::set_signal(std::shared_ptr<dbcppp::ISignal> signal) {
    auto sig = std::make_shared<Signal>();
    sig->set_signal(signal);
    m_signal = sig;
}

uint64_t caneo::Constant::get_raw() {
    m_signal->set_decode_value(m_value);
    return m_signal->get_raw_value();
}

std::shared_ptr<caneo::Signal> caneo::Constant::get_signal() {
    return m_signal;
}