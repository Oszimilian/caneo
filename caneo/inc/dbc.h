#ifndef _DBC_H_
#define _DBC_H_

#include <string>
#include <memory>
#include <dbcppp/Network.h>
#include <unordered_map>
#include <ostream>

namespace caneo {
    class Dbc {
        public:
            Dbc() = default;

            bool load_dbc(const std::string& dbc_file_path);

            bool has_message(const std::string& msg_name);
            bool has_message(uint64_t msg_id);

            std::shared_ptr<dbcppp::IMessage> get_message(const std::string& msg_name);
            std::shared_ptr<dbcppp::IMessage> get_message(uint64_t msg_id);


        public:
            friend std::ostream& operator<<(std::ostream& os, const Dbc& ref);

        private:
            std::unique_ptr<dbcppp::INetwork> m_inet;
            std::unordered_map<uint64_t, std::shared_ptr<dbcppp::IMessage>> m_imsgs;
    };

    std::ostream& operator<<(std::ostream& os, const Dbc& ref);
    std::ostream& operator<<(std::ostream& os, const std::shared_ptr<dbcppp::IMessage>& ref);
}

#endif