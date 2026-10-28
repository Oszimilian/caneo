-- Example guard: watches the steering target SS_ELMO_TARGET.SS_ELMO_TARGET_LWS.
--
-- Add it in the GUI: right-click the signal in Trace > can0 > "Add guard...",
-- or Guards > "+ Add guard". Outputs appear in Trace > Guards and are logged
-- as guard/<name>/<output>.
--
-- check(value, t) runs for every received frame of the message.
--   value   current value of the guarded signal
--   t       seconds since caneo started
-- Return a number, a boolean or a table {name = value, ...}.
--   get_signal(iface, msg, signal)   last value of any other signal (0 if none)
--   print(...)                       guard log (Script Editor)

local lower, upper = 3000, 13000     -- allowed LWS range
local max_rate     = 20000           -- max change per second
local last_value, last_t

function check(value, t)
    local rate = 0
    if last_t and t > last_t then
        rate = (value - last_value) / (t - last_t)
    end
    last_value, last_t = value, t

    local out_of_range = value < lower or value > upper
    if out_of_range then
        print(string.format("t=%.3f LWS target out of range: %.0f", t, value))
    end

    -- deviation between target and measured steering angle
    local actual = get_signal(interface, "RFC4800_LWS", "LWS_Sensor")

    return {
        out_of_range = out_of_range,
        too_fast     = math.abs(rate) > max_rate,
        rate         = rate,
        error        = actual - value,
    }
end
