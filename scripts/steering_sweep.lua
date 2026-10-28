-- Example Lua action: steering sweep for SS_ELMO.
--
-- Add it in the GUI under Actions > "+ Add action" > Lua script,
-- interface can0, period 10 ms.
--
-- tick(t) is called every period; t = seconds the action has been running.
--   send(msg, {SIGNAL = value, ...} [, iface])  encode with the DBC and send
--   send_raw(id, {byte, ...} [, iface])          send raw bytes
--   get_signal(iface, msg, signal)               last received value (0 if none)
--   print(...)                                   write to the action log

local middle    = 6000      -- LWS centre position
local amplitude = 2000
local period_s  = 4.0       -- one full sweep
local last_print = -1

function init()
    print(string.format("sweep on %s, every %d ms", interface, period_ms))
end

function tick(t)
    -- ramp the amplitude up over the first two seconds
    local ramp   = math.min(t / 2.0, 1.0)
    local target = middle + ramp * amplitude * math.sin(2 * math.pi * t / period_s)

    send("SS_ELMO_TARGET", { SS_ELMO_TARGET_LWS = target })

    -- once per second: compare with the measured steering angle
    if math.floor(t) ~= last_print then
        last_print = math.floor(t)
        local actual = get_signal(interface, "RFC4800_LWS", "LWS_Sensor")
        print(string.format("t=%5.1f  target=%7.1f  actual=%7.1f  error=%7.1f",
                            t, target, actual, actual - target))
    end
end
