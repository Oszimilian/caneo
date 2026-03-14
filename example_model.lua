
function run()
    local error = 0
    local brake = get_signal("can0", "SS_ELMO_TARGET",    "SS_ELMO_TARGET_LWS")

    if brake < 100 then
        error = 0
    else 
        error = 1
    end

    return {
        error = error,
    }
end
