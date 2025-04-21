#include <fstream>

#include "dbc.h"
#include "dbcppp/Network2Functions.h"

bool caneo::Dbc::load_dbc(const std::string& dbc_file_path) {
    std::ifstream idbc(dbc_file_path);
    m_inet = dbcppp::INetwork::LoadDBCFromIs(idbc);
    if (!m_inet) return false;

    for (const auto& msg : m_inet->Messages()) {
        m_imsgs.emplace(msg.Id(), msg.Clone());
    }

    return true;
}

bool caneo::Dbc::has_message(const std::string &msg_name) {
    for (const auto& [id, msg] : m_imsgs) {
        if (msg->Name() == msg_name) {
            return true;
        }
    }
    return false;
}

bool caneo::Dbc::has_message(uint64_t msg_id) {
    if (m_imsgs.count(msg_id)) {
        return true;
    }
    return false;
}

std::shared_ptr<dbcppp::IMessage> caneo::Dbc::get_message(const std::string &msg_name) {
    for (const auto& [id, msg] : m_imsgs) {
        if (msg->Name() == msg_name) {
            return msg;
        }
    }
    return nullptr;
}

std::shared_ptr<dbcppp::IMessage> caneo::Dbc::get_message(uint64_t msg_id) {
    if (m_imsgs.count(msg_id)) {
        return m_imsgs[msg_id];
    }
    return nullptr;
}



std::ostream& caneo::operator<<(std::ostream& os, const caneo::Dbc& ref) {
    return dbcppp::Network2Human::operator<<(std::cout, *ref.m_inet);
}

std::ostream &caneo::operator<<(std::ostream &os, const std::shared_ptr<dbcppp::IMessage> &ref) {
    return dbcppp::Network2Human::operator<<(std::cout, *ref);
}