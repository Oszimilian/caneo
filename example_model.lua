-- example_model.lua
--
-- Beispielmodell für caneo.
-- Aufruf: ./caneo --tui --log --model example_model.lua vcan0:dbc/file.dbc
--
-- Passe Interface, Message- und Signalnamen an deine DBC-Datei an.

local IFACE = "vcan0"

-- Moving-Average-Fenster für den Bremsdruck
local brake_window = {}
local BRAKE_WINDOW_SIZE = 20

local function moving_average(window, new_value, max_size)
    table.insert(window, new_value)
    if #window > max_size then
        table.remove(window, 1)
    end
    local sum = 0
    for _, v in ipairs(window) do sum = sum + v end
    return sum / #window
end

function run()
    local brake = get_signal(IFACE, "BrakeMsg",    "BrakePressure")
    local air   = get_signal(IFACE, "AirMsg",      "AirPressure")
    local speed = get_signal(IFACE, "VehicleSpeed", "Speed")

    -- Gleitender Mittelwert Bremsdruck
    local brake_avg = moving_average(brake_window, brake, BRAKE_WINDOW_SIZE)

    -- Differenz Brems- zu Luftdruck (Fehlerindikator)
    local pressure_diff = brake - air

    -- Einfache Fehlerkennung: Bremsdruck ohne nennenswerte Verzögerung
    local brake_without_decel = 0.0
    if brake > 10.0 and speed > 5.0 then
        brake_without_decel = 1.0
    end

    return {
        brake_avg           = brake_avg,
        pressure_diff       = pressure_diff,
        brake_without_decel = brake_without_decel,
    }
end
