

#include "wave_message.h"

#include "constant.h"

caneo::WaveMessage::WaveMessage() : m_message(std::make_shared<Message>()) {

}

void caneo::WaveMessage::set_message(std::shared_ptr<dbcppp::IMessage> message) {
    m_message->set_message(message);
}

void caneo::WaveMessage::set_period(std::chrono::milliseconds period) {
    m_period = period;
}

bool caneo::WaveMessage::set_wave_const(std::string signal_name, double value) {
    if (!m_message->has_signal(signal_name)) return false;
    auto wave = std::make_shared<caneo::Constant>();
    wave->set_period(m_period);
    wave->set_start_time(std::chrono::steady_clock::now());
    wave->set_constant(value);
    wave->set_signal(m_message->get_signal(signal_name));
    m_wave_patterns.push_back(std::move(wave));
    return true;
}

bool caneo::WaveMessage::period_elapsed() {
    if ((std::chrono::steady_clock::now() - m_last_time) > m_period) {
        return true;
    } else {
        return false;
    }
}

can_frame caneo::WaveMessage::get_frame() {
    can_frame frame;

    for (auto wave_pattern : m_wave_patterns) {
        wave_pattern->get_signal()->encode(frame);
    }
    frame.can_dlc = m_message->get_dlc();
    frame.can_id = m_message->get_id();

    return frame;
}

bool caneo::WaveMessage::is_complete() {
    return true;
}

bool caneo::WaveMessage::is_complete() {
    for (const auto dbc_sig : m_message)
}