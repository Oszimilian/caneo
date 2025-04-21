#ifndef _WAVE_PATTERN_H_
#define _WAVE_PATTERN_H_

#include <chrono>
#include <memory>
#include <dbcppp/Network.h>

#include "signal.h"

namespace caneo {

    class WavePattern {
        public:
            WavePattern();
            virtual ~WavePattern() = default;

        public:
            std::chrono::time_point<std::chrono::steady_clock> m_start_time;
            std::chrono::milliseconds m_period;

            std::shared_ptr<Signal> m_signal;

        public:
            virtual void set_start_time(std::chrono::time_point<std::chrono::steady_clock> start_time) = 0;
            virtual void set_period(std::chrono::milliseconds period) = 0;
            virtual void set_signal(std::shared_ptr<dbcppp::ISignal> signal) = 0;
            virtual uint64_t get_raw() = 0;
            virtual std::shared_ptr<Signal> get_signal() = 0;

        public:
            virtual void set_constant(double value) {};
    };
}

#endif