#ifndef _WAVE_MESSAGE_H_
#define _WAVE_MESSAGE_H_

#include <linux/can.h>
#include <linux/can/raw.h>

#include "message.h"
#include "wave_pattern.h"

namespace caneo {
    class WaveMessage {
        public:
            WaveMessage();

            void set_message(std::shared_ptr<dbcppp::IMessage> message);

            bool set_wave_const(std::string signal_name, double value);

            void set_period(std::chrono::milliseconds period);

            bool is_complete();

            bool period_elapsed();

            can_frame get_frame();

        private:
            std::shared_ptr<Message> m_message;
            std::vector<std::shared_ptr<WavePattern>> m_wave_patterns;

        private:
            std::chrono::milliseconds m_period;
            std::chrono::time_point<std::chrono::steady_clock> m_last_time;
    };
}

#endif