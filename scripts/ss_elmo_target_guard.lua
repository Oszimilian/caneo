-- caneo guard
--
-- check(value, t) runs for every received frame of the guarded message.
--   value   current value of the guarded signal
--   t       seconds since caneo started
-- Return the outputs: a number, a boolean, or a table {name = value, ...}.
-- They are shown in Trace > Guards, can be plotted and are logged as
-- guard/<guard name>/<output> while logging is active.
--
--   get_signal(iface, msg, signal)   last value of any other signal (0 if none)
--   print(...)                       guard log (shown in the Script Editor)
--   interface, msg, signal           what this guard watches

local limit = 5000

function check(value, t)
    return {
        too_high = value > limit,     -- booleans become 1 / 0
    }
end
