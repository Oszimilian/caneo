

#include "message.h"

void caneo::Message::set_message(std::shared_ptr<dbcppp::IMessage> message) {
    m_message = message;
}

bool caneo::Message::has_signal(std::string name) {
    for (const auto& sig : m_message->Signals()) {
        if (sig.Name() == name) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<dbcppp::ISignal> caneo::Message::get_signal(std::string name) {
    for (const auto& sig : m_message->Signals()) {
        if (sig.Name() == name) {
            return sig.Clone();
        }
    }
    return nullptr;
}

uint64_t caneo::Message::get_id() {
    return m_message->Id();
}

uint64_t caneo::Message::get_dlc() {
    return m_message->MessageSize();
}

std::vector<std::tuple<std::string, double>> caneo::Message::get_signal_values() {

}