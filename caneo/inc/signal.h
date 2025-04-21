#ifndef _SIGNAL_H_
#define _SIGNAL_H_

#include <memory>
#include <dbcppp/Network.h>
#include <linux/can.h>
#include <linux/can/raw.h>

namespace caneo {
    class Signal {
        public:
            Signal() = default;

            void set_signal(std::shared_ptr<dbcppp::ISignal> signal);

            void set_raw_value(uint64_t raw);
            void set_decode_value(double value);

            std::string get_name();
            uint64_t get_raw_value();
            double get_decode_value();

            void encode(can_frame& frame);


        private:
            double m_decode_value;
            std::shared_ptr<dbcppp::ISignal> m_signal;
    };
}

#endif