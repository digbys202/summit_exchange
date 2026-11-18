-- Copyright 2026 Roman Altenburg (Digby)
-- Licensed under the Apache License, Version 2.0.
-- See LICENSE for the license text.

local memory_name = "Pro3PatchSender"
local snapshot_size = 4693

local summit_header = "\240\0\32\41\1\17\1\51"

local function valid_summit_snapshot(bytes)
    if bytes:sub(1, 8) ~= summit_header or bytes:byte(-1) ~= 247 then return false end
    local command = bytes:byte(9)
    if not ((#bytes == 527 and command == 0) or (#bytes == 1039 and command == 2)) then return false end
    for index = 10, 16 do
        if bytes:byte(index) ~= 0 then return false end
    end
    for index = 17, #bytes - 1 do
        if bytes:byte(index) >= 128 then return false end
    end
    return true
end

local function normalize_summit_snapshot(bytes)
    if valid_summit_snapshot(bytes) then return bytes end
    local command = bytes:byte(9)
    if bytes:sub(1, 8) == summit_header
        and ((#bytes == 527 and command == 1) or (#bytes == 1039 and command == 3))
        and bytes:byte(13) >= 1 and bytes:byte(13) <= 4 and bytes:byte(14) < 128 then
        for _, index in ipairs({10, 11, 12, 15, 16}) do
            if bytes:byte(index) ~= 0 then return nil, "Invalid Summit stored-program header" end
        end
        local normalized = bytes:sub(1, 8) .. string.char(command - 1)
            .. string.rep("\0", 7) .. bytes:sub(17)
        if valid_summit_snapshot(normalized) then return normalized end
    end
    return nil, "Select one valid Summit Single or Multi .syx patch"
end

local function summit_patch_name(bytes)
    if not valid_summit_snapshot(bytes) then return nil end
    local offset = #bytes == 1039 and 411 or 16
    return (bytes:sub(offset + 1, offset + 16):gsub("[^ -~]", " "):gsub("%s+$", ""))
end

local function valid_snapshot(bytes)
    if #bytes ~= snapshot_size or bytes:sub(1, 4) ~= "\240\1\49\3" or bytes:byte(-1) ~= 0xf7 then
        return false
    end
    for index = 5, snapshot_size - 1 do
        if bytes:byte(index) >= 0x80 then return false end
    end
    return true
end

local function normalize_edit_snapshot(bytes)
    if valid_snapshot(bytes) then return bytes end
    if type(bytes) ~= "string" or #bytes ~= 4695
        or bytes:sub(1, 4) ~= "\240\1\49\2"
        or bytes:byte(5) > 7 or bytes:byte(6) > 127 or bytes:byte(-1) ~= 0xf7 then
        return nil, "Select one valid Pro 3 edit-buffer or stored-program .syx dump"
    end
    for index = 7, 4694 do
        if bytes:byte(index) >= 0x80 then
            return nil, "Stored-program dump contains invalid SysEx data"
        end
    end
    return "\240\1\49\3" .. bytes:sub(7, 4694) .. "\247"
end

local function find_destination(list, client_name, destination_name)
    client_name = client_name or "PRO 3"
    destination_name = destination_name or "PRO 3 MIDI 1"
    local current_client, matches = nil, {}
    for line in list:gmatch("[^\r\n]+") do
        local client, name = line:match("^client%s+(%d+):%s+'([^']+)'")
        if client then
            current_client = name == client_name and client or nil
        elseif current_client then
            local port, port_name = line:match("^%s+(%d+)%s+'([^']+)'")
            if port and port_name:match("^%s*(.-)%s*$") == destination_name then
                matches[#matches + 1] = current_client .. ":" .. port
            end
        end
    end
    if #matches ~= 1 then
        return nil, #matches == 0 and client_name .. " ALSA MIDI output not found"
            or "Multiple " .. client_name .. " MIDI outputs found"
    end
    return matches[1]
end

local function make_midi_file(bytes, validator)
    if not (validator or valid_snapshot)(bytes) then return nil, "Invalid Pro 3/Summit edit-buffer snapshot" end
    local length = #bytes - 1
    local encoded = {string.char(length & 127)}
    length = length >> 7
    while length > 0 do
        table.insert(encoded, 1, string.char((length & 127) | 128))
        length = length >> 7
    end
    local track = "\0\240" .. table.concat(encoded) .. bytes:sub(2) .. "\0\255\47\0"
    return "MThd" .. string.pack(">I4I2I2I2", 6, 0, 1, 96)
        .. "MTrk" .. string.pack(">I4", #track) .. track
end

local function patch_name(bytes)
    if not valid_snapshot(bytes) then return nil end
    local characters = {}
    for name_index = 0, 19 do
        local raw_index = 321 + name_index
        local group_index = math.floor(raw_index / 7)
        local bit_index = raw_index % 7
        local mask_index = 5 + group_index * 8
        local low_byte = bytes:byte(mask_index + 1 + bit_index)
        local high_bit = (bytes:byte(mask_index) >> bit_index) & 1
        local value = low_byte + (high_bit << 7)
        characters[#characters + 1] = string.char(value >= 32 and value <= 126 and value or 32)
    end
    return table.concat(characters):gsub("%s+$", "")
end

local function default_syx_filename(bytes, decoder)
    local name = (decoder or patch_name)(bytes)
    name = name and name:gsub("[/\\:*?\"<>|]", "_"):gsub("[%s%.]+$", "") or ""
    if name == "" then name = "Pro3_patch" end
    return name .. ".syx"
end

local function ensure_syx_extension(path)
    if type(path) ~= "string" or path == "" then
        return nil, "No filename selected"
    end
    local extension = path:match("%.([^./\\]+)$")
    if not extension then return path .. ".syx" end
    if extension:lower() ~= "syx" then
        return nil, "Choose a .syx filename"
    end
    return path
end

if not reaper then
    return {
        valid_snapshot = valid_snapshot,
        normalize_edit_snapshot = normalize_edit_snapshot,
        find_destination = find_destination,
        make_midi_file = make_midi_file,
        patch_name = patch_name,
        default_syx_filename = default_syx_filename,
        ensure_syx_extension = ensure_syx_extension,
        valid_summit_snapshot = valid_summit_snapshot,
        normalize_summit_snapshot = normalize_summit_snapshot,
        summit_patch_name = summit_patch_name,
    }
end

local function run_command(command, timeout)
    local output = reaper.ExecProcess(command, timeout)
    if not output then return nil, "Could not start command" end
    local status, text = output:match("^([%-]?%d+)\r?\n(.*)$")
    if not status or tonumber(status) ~= 0 then
        return nil, "Command failed: " .. output
    end
    return text
end

local function send_snapshot(bytes, profile)
    local midi_file, error_message = make_midi_file(bytes, profile.validate)
    if not midi_file then return false, error_message end

    local ports
    ports, error_message = run_command("/usr/bin/aconnect -o -l", 3000)
    if not ports then return false, error_message end
    local destination
    destination, error_message = find_destination(ports, profile.client, profile.port)
    if not destination then return false, error_message end

    local temporary
    temporary, error_message = run_command("/usr/bin/mktemp -d /tmp/pro3-sender-XXXXXXXXXX", 3000)
    if not temporary then return false, error_message end
    local directory = temporary:match("^(/tmp/pro3%-sender%-%w+)%s*$")
    if not directory then return false, "Unexpected temporary directory returned" end
    local path = directory .. "/patch.mid"
    local file
    file, error_message = io.open(path, "wb")
    if not file then
        os.remove(directory)
        return false, "Could not create temporary MIDI file: " .. tostring(error_message)
    end
    local written, write_error = file:write(midi_file)
    local closed, close_error = file:close()
    if not written or not closed then
        os.remove(path)
        os.remove(directory)
        return false, "Could not write temporary MIDI file: " .. tostring(write_error or close_error)
    end

    local output
    output, error_message = run_command("/usr/bin/aplaymidi -p " .. destination .. " -d 0 " .. path, 10000)
    os.remove(path)
    os.remove(directory)
    if not output then return false, error_message end
    return true
end

local function save_snapshot(bytes, profile)
    if not profile.validate(bytes) then return false, "Invalid edit-buffer snapshot" end

    local selected, path = reaper.GetUserFileName(
        0,
        "Save " .. profile.label .. " Patch",
        default_syx_filename(bytes, profile.name),
        "SysEx files|*.syx|All files|*.*"
    )
    if not selected then return nil, "cancelled" end

    local save_path, error_message = ensure_syx_extension(path)
    if not save_path then return false, error_message end

    local file
    file, error_message = io.open(save_path, "wb")
    if not file then return false, "Could not open file for writing: " .. tostring(error_message) end
    local written, write_error = file:write(bytes)
    local closed, close_error = file:close()
    if not written or not closed then
        return false, "Could not write SysEx file: " .. tostring(write_error or close_error)
    end
    return true, save_path
end

local function load_snapshot(profile)
    local selected, path = reaper.GetUserFileName(
        1,
        "Load " .. profile.label .. " Patch",
        ".syx",
        "SysEx files|*.syx|All files|*.*"
    )
    if not selected then return nil, "cancelled" end

    local file, error_message = io.open(path, "rb")
    if not file then return false, "Could not open SysEx file: " .. tostring(error_message) end
    local bytes, read_error = file:read(profile.maximum + 3)
    local closed, close_error = file:close()
    if not bytes then
        return false, "Could not read SysEx file: " .. tostring(read_error or close_error)
    end
    if not closed then return false, "Could not close SysEx file: " .. tostring(close_error) end
    local snapshot, normalize_error = profile.normalize(bytes)
    if not snapshot then return false, normalize_error end
    return true, snapshot
end

local profiles = {
    {memory = memory_name, label = "Pro 3", maximum = snapshot_size,
        validate = valid_snapshot, normalize = normalize_edit_snapshot, name = patch_name},
    {memory = "SummitPatchSender", label = "Summit", maximum = 1039,
        validate = valid_summit_snapshot, normalize = normalize_summit_snapshot,
        name = summit_patch_name, client = "Summit", port = "Summit MIDI 1"},
}

local now = reaper.time_precise()
for _, profile in ipairs(profiles) do
    reaper.gmem_attach(profile.memory)
    local previous_heartbeat = reaper.gmem_read(3)
    if previous_heartbeat > 0 and math.abs(now - previous_heartbeat) < 2 then
        reaper.ShowMessageBox("Exchange Helper is already running.", "Exchange Helper", 0)
        return
    end
end

local owner = math.floor(now * 1000000) % 2147483646 + 1
for _, profile in ipairs(profiles) do
    reaper.gmem_attach(profile.memory)
    reaper.gmem_write(4, owner)
    reaper.gmem_write(3, now)
    if reaper.gmem_read(0) ~= 0 then
        reaper.gmem_write(0, 5)
        profile.finished_at = now
    end
end

if reaper.set_action_options then reaper.set_action_options(1) end
local _, _, section_id, command_id = reaper.get_action_context()
if section_id and command_id and command_id > 0 then
    reaper.SetToggleCommandState(section_id, command_id, 1)
    reaper.RefreshToolbar2(section_id, command_id)
end

local function process_profile(profile)
    reaper.gmem_attach(profile.memory)
    local current_owner = reaper.gmem_read(4)
    if current_owner == 0 then
        reaper.gmem_write(4, owner)
    elseif current_owner ~= owner then
        return
    end
    now = reaper.time_precise()
    reaper.gmem_write(3, now)
    local state = reaper.gmem_read(0)
    if state == 2 then
        local request_owner = reaper.gmem_read(2)
        local request_time = reaper.gmem_read(5)
        local operation = reaper.gmem_read(6)
        reaper.gmem_write(0, 3)
        local data = {}
        local valid = request_time > 0 and math.abs(now - request_time) < 5
        if valid and operation ~= 3 then
            local length = profile.memory == memory_name and snapshot_size or reaper.gmem_read(7)
            valid = length == profile.maximum or (profile.label == "Summit" and length == 527)
            for index = 0, valid and length - 1 or -1 do
                local value = reaper.gmem_read(16 + index)
                if value < 0 or value > 255 or value ~= math.floor(value) then
                    valid = false
                    break
                end
                data[index + 1] = string.char(value)
            end
        end
        local ok, error_message = false, "Expired request or invalid data received from effect"
        if valid then
            if operation == 1 then
                ok, error_message = send_snapshot(table.concat(data), profile)
            elseif operation == 2 then
                ok, error_message = save_snapshot(table.concat(data), profile)
            elseif operation == 3 then
                ok, error_message = load_snapshot(profile)
            else
                error_message = "Unknown " .. profile.label .. " companion operation"
            end
        end
        local result_state = ok and 4 or error_message == "cancelled" and 6 or 5
        reaper.gmem_attach(profile.memory)
        local owns_request = reaper.gmem_read(0) == 3 and reaper.gmem_read(2) == request_owner
        if owns_request then
            if operation == 3 and ok then
                reaper.gmem_write(7, #error_message)
                for index = 1, #error_message do
                    reaper.gmem_write(15 + index, error_message:byte(index))
                end
            end
            reaper.gmem_write(0, result_state)
            profile.finished_at = reaper.time_precise()
        end
        if not ok and error_message ~= "cancelled" then
            reaper.ShowConsoleMsg("Exchange Helper (" .. profile.label .. "): " .. tostring(error_message) .. "\n")
        end
    elseif (state == 4 or state == 5 or state == 6) and profile.finished_at and now - profile.finished_at >= 10 then
        reaper.gmem_write(0, 0)
        profile.finished_at = nil
    elseif state == 0 then
        profile.finished_at = nil
    end
end

local function run()
    for _, profile in ipairs(profiles) do process_profile(profile) end
    reaper.defer(run)
end

reaper.atexit(function()
    for _, profile in ipairs(profiles) do
    reaper.gmem_attach(profile.memory)
    if reaper.gmem_read(4) == owner then
        local state = reaper.gmem_read(0)
        if state == 1 or state == 2 then reaper.gmem_write(0, 5) end
        reaper.gmem_write(3, 0)
        reaper.gmem_write(4, 0)
    end
    end
    if section_id and command_id and command_id > 0 then
        reaper.SetToggleCommandState(section_id, command_id, 0)
        reaper.RefreshToolbar2(section_id, command_id)
    end
end)

reaper.defer(run)