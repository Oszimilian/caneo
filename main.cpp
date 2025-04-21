#include <chrono>
#include <iostream>

#include "wave_pattern.h"
#include "constant.h"
#include "dbc.h"
#include "signal.h"
#include "message.h"
#include "wave_message.h"


using namespace caneo;

int main(void) {
    std::cout << "CANEO" << std::endl;

    caneo::Dbc d;
    d.load_dbc("/home/maximilian/Git/Charger/dbc/CAN4_Accu_1.dbc");
    if (d.has_message("BMaS_Status")) {
        auto msg = d.get_message("BMaS_Status");
        std::cout << msg << std::endl;
    }

    caneo::WaveMessage msg;
    msg.set_message(d.get_message("BMaS_Status"));
    msg.set_period(std::chrono::seconds(1));
    msg.set_wave_const("BMoS_Error", 1.0);
    
    

    caneo::Message m;
    m.set_message(d.get_message("BMaS_Status"));


    caneo::Signal s;
    s.set_signal(m.get_signal("Debug_Plausible_TS_EN"));
    s.set_raw_value(100);
    

    WavePattern *p = new Constant();

    p->set_start_time(std::chrono::steady_clock::now());
    p->set_period(std::chrono::seconds(1));
}