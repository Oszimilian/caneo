#ifndef _CONSTANT_H_
#define _CONSTANT_H_

#include "wave_pattern.h"

namespace caneo {
    class Constant : public WavePattern{
        public:
            Constant();
            virtual ~Constant() = default;

        public:
            virtual void set_start_time(std::chrono::time_point<std::chrono::steady_clock> start_time);
            virtual void set_period(std::chrono::milliseconds period);
            virtual void set_signal(std::shared_ptr<dbcppp::ISignal> signal);
            virtual uint64_t get_raw();
            virtual std::shared_ptr<Signal> get_signal();

        public:
            virtual void set_constant(double value) override;

        private:
            double m_value;
    };
}

#endif