
function run()

    local target = get_signal("can0", "SS_ELMO_TARGET",    "SS_ELMO_TARGET_LWS")
    local act = get_signal("can0", "RFC4800_LWS", "LWS_Sensor")


    return {
        error = act - target
    }
end
