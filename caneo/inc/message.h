#ifndef _MESSAGE_H_
#define _MESSAGE_H_

#include <memory>
#include <string>
#include <dbcppp/Network.h>
#include <vector>
#include <tuple>

namespace caneo {
    class Message {
        public:
            Message() = default;

            void set_message(std::shared_ptr<dbcppp::IMessage> message);

            bool has_signal(std::string name);

            std::shared_ptr<dbcppp::ISignal> get_signal(std::string name);

            uint64_t get_id();

            uint64_t get_dlc();

            std::vector<std::tuple<std::string, double>> get_signal_values(); 

        private:
            std::shared_ptr<dbcppp::IMessage> m_message;
    };
}

#endif