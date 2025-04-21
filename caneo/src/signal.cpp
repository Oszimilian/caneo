

#include "signal.h"

void caneo::Signal::set_signal(std::shared_ptr<dbcppp::ISignal> signal) {
    m_signal = signal;
}

void caneo::Signal::set_decode_value(double value) {
    m_decode_value = value;
}

void caneo::Signal::set_raw_value(uint64_t raw) {
    m_decode_value = m_signal->RawToPhys(raw);
}

std::string caneo::Signal::get_name() {
    return m_signal->Name();
}

uint64_t caneo::Signal::get_raw_value() {
    return m_signal->PhysToRaw(m_decode_value);
}

double caneo::Signal::get_decode_value() {
    return m_decode_value;
}

void caneo::Signal::encode(can_frame& frame) {
    m_signal->Encode(get_raw_value(), frame.data);
}