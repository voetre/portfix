-- portfix for Windower 4. The fixes are in portfix.dll, next to this file.
_addon.name     = 'portfix'
_addon.author   = 'voetre'
_addon.version  = '1.3.1'
_addon.commands = {'portfix'}

local fixes = {'water', 'halo', 'shine', 'disint'}
local about = {water = 'black water fix', halo = 'summon halo fix', shine = 'sphere-map shine fix', disint = 'disintegrate effect fix'}
local state = {water = true, halo = true, shine = true, disint = true}

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
        core.set(state.water, state.halo, state.disint, state.shine)
    end
end
apply()

-- A newer release on GitHub, told once you're logged in. The dll makes one request from start(); this only asks
-- the dll for its answer.
local function check_update()
    if not core or not core.update then
        return
    end
    if windower.ffxi.get_info().logged_in then
        local result = core.update()
        if type(result) == 'string' then
            log(result)
            return
        elseif result then
            return
        end
    end
    coroutine.schedule(check_update, 5)
end
check_update()

windower.register_event('unload', function()
    if core then
        core.set(false, false, false, false)
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
