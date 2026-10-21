-- portfix for Windower 4. The fixes are in portfix.dll, next to this file.
_addon.name     = 'portfix'
_addon.author   = 'voetre'
_addon.version  = '1.0'
_addon.commands = {'portfix'}

local fixes = {'water', 'halo'}
local about = {water = 'black water fix', halo = 'summon halo fix'}
local state = {water = true, halo = true}

local function log(text)
    windower.add_to_chat(207, '[portfix] ' .. text)
end

local open, err = package.loadlib(windower.addon_path .. 'portfix.dll', 'luaopen_portfix')
local core = open and open()
if not core then
    log('portfix.dll not loaded: ' .. (err or 'LuaCore.dll not found'))
else
    local ok, reason = core.start()
    if not ok then
        log('not active: ' .. reason)
        core = nil
    end
end

local function apply()
    if core then
        core.set(state.water, state.halo)
    end
end
apply()

windower.register_event('unload', function()
    if core then
        core.set(false, false)
    end
end)

windower.register_event('addon command', function(name, arg)
    name = name and name:lower() or ''
    arg  = arg and arg:lower()
    if about[name] then
        if arg == 'on' then
            state[name] = true
        elseif arg == 'off' then
            state[name] = false
        else
            state[name] = not state[name]
        end
        apply()
        log(name .. ' - ' .. (state[name] and 'on' or 'off'))
    elseif name == '' then
        for _, n in ipairs(fixes) do
            log(n .. ' - ' .. (state[n] and 'on' or 'off'))
        end
    else
        log('//portfix - list fixes and their state')
        for _, n in ipairs(fixes) do
            log('//portfix ' .. n .. ' [on|off] - ' .. about[n])
        end
        log('//portfix help - this list')
    end
end)
