-- Based on example code by Hadriel Kaplan
--
--
-- NOTE:
-- THE DEBUG PRINT STUFF - NOT SURE HOW TO MAKE IT WORK YET
--


----------------------------------------
-- do not modify this table
local debug_level = {
    DISABLED = 0,
    LEVEL_1  = 1,
    LEVEL_2  = 2
}

-- set this DEBUG to debug_level.LEVEL_1 to enable printing debug_level info
-- set it to debug_level.LEVEL_2 to enable really verbose printing
-- note: this will be overridden by user's preference settings
local DEBUG = debug_level.LEVEL_1

local default_settings =
{
    debug_level  = DEBUG,
    port         = 65333,
    heur_enabled = false,
}

-- for testing purposes, we want to be able to pass in changes to the defaults
-- from the command line; because you can't set lua preferences from the command
-- line using the '-o' switch (the preferences don't exist until this script is
-- loaded, so the command line thinks they're invalid preferences being set)
-- so we pass them in as command arguments insetad, and handle it here:
local args={...} -- get passed-in args
if args and #args > 0 then
    for _, arg in ipairs(args) do
        local name, value = arg:match('(.+)=(.+)')
        if name and value then
            if tonumber(value) then
                value = tonumber(value)
            elseif value == 'true' or value == 'TRUE' then
                value = true
            elseif value == 'false' or value == 'FALSE' then
                value = false
            elseif value == 'DISABLED' then
                value = debug_level.DISABLED
            elseif value == 'LEVEL_1' then
                value = debug_level.LEVEL_1
            elseif value == 'LEVEL_2' then
                value = debug_level.LEVEL_2
            else
                error('invalid commandline argument value')
            end
        else
            error('invalid commandline argument syntax')
        end

        default_settings[name] = value
    end
end

local dprint = function() end
local dprint2 = function() end
local function reset_debug_level()
    if default_settings.debug_level > debug_level.DISABLED then
        dprint = function(...)
            print(table.concat({'Lua:', ...},' '))
        end

        if default_settings.debug_level > debug_level.LEVEL_1 then
            dprint2 = dprint
        end
    end
end
-- call it now
reset_debug_level()

dprint2('Wireshark version = ', get_version())
dprint2('Lua version = ', _VERSION)





----------------------------------------
-- creates a Proto object, but doesn't register it yet
--


local eink = Proto('eink1', 'Eink Protocol 1')

local MkPF = {}
local fields = {}
local f = {}
setmetatable(MkPF, {
    __index = function(self, typ)
        return function(key, ...)
            local pf = ProtoField[typ]('eink.' .. key, ...)
            local chain = {}
            for match in string.gmatch(key,'([^%.]+)') do
                table.insert(chain, match)
            end
            local lastIndex = #chain
            local last = chain[lastIndex]
            table.remove(chain, lastIndex)
            local tbl = f
            for _,key in pairs(chain) do
                if tbl[key] == nil then
                    tbl[key] = {}
                end
                tbl = tbl[key]
            end
            tbl[last] = pf
            fields[key] = pf
            return MkPF
        end
    end,
})


MkPF.none  ('boilerplate', 'Boilerplate')
    .string('usb.magic', 'USB MS magic number')
    .uint32('ctl.magic', 'EInk driver magic number', base.HEX)
    .uint32('ctl.len', 'Size of follow-up packet')
    .uint16('ctl.flags', 'Flags', base.HEX)
    .bool  ('ctl.direction', 'Direction', 16, { 'in', 'out' }, 0x8000)
    .uint8 ('ctl.cmdlen', 'Bytes left in this packet')
    .uint16('ctl.opcode', 'The mystery 0xfe', base.HEX)
    .uint32('ctl.addr', 'Address', base.HEX)
    .uint8 ('ctl.cmd', 'Command (94 = BLIT, A8 = XFER, 83 = READ, 84 = WRITE)', base.HEX)
    .uint16('ctl.arg1', 'arg 1')
    .uint16('ctl.arg2', 'arg 2')
    .uint16('ctl.arg3', 'arg 3')
    .uint16('ctl.arg4', 'arg 4')
    .uint16('ctl.blit.x', 'arg 1 (x)     ')
    .uint16('ctl.blit.y', 'arg 2 (y)     ')
    .uint16('ctl.blit.w', 'arg 3 (width) ')
    .uint16('ctl.blit.h', 'arg 4 (height)')

    .uint8 ('ctl.zero', 'Always 0 except now?', base.HEX)

    .uint32('blit.addr', 'Address', base.HEX)
    .uint32('blit.1', 'unsure', base.HEX)
    .uint32('blit.x', 'x', base.DEC)
    .uint32('blit.y', 'y', base.DEC)
    .uint32('blit.w', 'w', base.DEC)
    .uint32('blit.h', 'h', base.DEC)
    .uint32('blit.0', 'Always 0?', base.HEX)

    .none  ('read.payload', 'Read payload')
    .none  ('write.payload', 'Write payload')



eink.fields = fields

f.direction = Field.new('usb.endpoint_address.direction')
local ctl_direction = Field.new('eink.ctl.direction')



local state = {
    IDLE = -1,
    ISSUED = -2,
    PERFORMED = -3,
    packet_data = {},
}
local cmd = {
    NONE  = -10,
    BLIT  = -11,
    XFER  = -12,
    READ  = -13,
    WRITE = -14,
    DOORKNOCK = -15,
}
state.curr = state.IDLE


----------------------------------------
-- The following creates the callback function for the dissector.
-- The 'tvbuf' is a Tvb object, 'pktinfo' is a Pinfo object, and 'root' is a TreeItem object.
-- Whenever Wireshark dissects a packet that our Proto is hooked into, it will call
-- this function and pass it these arguments for the packet it's dissecting.
function eink.dissector(tvbuf, pktinfo, root)
    dprint2('eink.dissector called')



    local pktlen = tvbuf:reported_length_remaining()


    local tree = root:add(eink, tvbuf:range(0, pktlen))

    local usbms
    if pktlen >= 4 then
        usbms = tvbuf:range(0, 4):string()
    else
        usbms = ''
    end
    local my_data

    -- FIXME this assumes all USB traffic pertains to this protocol
    -- we need to filter it out to specific endpoints
    if pktinfo.visited == false then
        if usbms == 'USBC' or usbms == 'USBS' then

            if tvbuf:range(4, 4):le_uint() ~= 0x89518961 then -- not for us
                return
            end
            state.cmd = cmd.NONE

            if usbms == 'USBC' then
                state.curr = state.ISSUED
                local cmd_code = tvbuf:range(21, 2):uint()
                if cmd_code == 0x9400 then
                    state.cmd = cmd.BLIT
                elseif bit32.band(cmd_code, 0xff00) == 0xa800 then
                    state.cmd = cmd.XFER
                elseif cmd_code == 0x8300 then
                    state.cmd = cmd.READ
                elseif cmd_code == 0x8400 then
                    state.cmd = cmd.WRITE
                elseif cmd_code == 0x8000 then
                    state.cmd = cmd.DOORKNOCK
                end
            elseif usbms == 'USBS' then
                state.curr = state.IDLE
            end
        else
            if state.curr == state.ISSUED then
                state.curr = state.PERFORMED
            else
                return
            end
        end
        state.packet_data[pktinfo.number] = {
            state = state.curr,
            cmd = state.cmd,
        }
        return pktlen
    else
        my_data = state.packet_data[pktinfo.number]
        if my_data == nil then
            return
        end
    end

    local my_state = my_data.state
    local my_cmd = my_data.cmd

    pktinfo.cols.protocol:set('E-Ink')
    pktinfo.cols.protocol:set('E-Ink' .. my_cmd)


    if my_state == state.PERFORMED then

        local direction = f.direction().value

        if direction == 1 then
            pktinfo.cols.info:set('◀—□')
        else
            pktinfo.cols.info:set('□—▶')
        end
        pktinfo.cols.info:append(' (' .. pktlen .. ' bytes)')

        local my_cmd = state.packet_data[pktinfo.number].cmd


        if my_cmd == cmd.BLIT then
            tree:add(f.blit.addr, tvbuf:range(0, 4))
            tree:add(f.blit['1'], tvbuf:range(4, 4))
            tree:add(f.blit.x, tvbuf:range(8, 4))
            tree:add(f.blit.y, tvbuf:range(12, 4))
            tree:add(f.blit.w, tvbuf:range(16, 4))
            tree:add(f.blit.h, tvbuf:range(20, 4))

            x = tvbuf:range(8, 4):uint()
            y = tvbuf:range(12, 4):uint()
            w = tvbuf:range(16, 4):uint()
            h = tvbuf:range(20, 4):uint()

            pktinfo.cols.info:append('  BLIT(' .. x .. ', ' .. y .. ', ' .. w .. ', ' .. h .. ')')
            if tvbuf:range(24, 4):uint() ~= 0 then
                tree:add(f.blit.zero, tvbuf:range(24, 4):uint())
            end
        elseif my_cmd == cmd.XFER then
            tree:add(f.write.payload, tvbuf:range(0, pktlen))
            pktinfo.cols.info:append('  XFER')
        elseif my_cmd == cmd.READ then
            tree:add(f.read.payload, tvbuf:range(0, pktlen))
            pktinfo.cols.info:append('  READ')
        elseif my_cmd == cmd.WRITE then
            tree:add(f.write.payload, tvbuf:range(0, pktlen))
            pktinfo.cols.info:append('  WRITE')
        elseif my_cmd == cmd.DOORKNOCK then
            tree:add(f.write.payload, tvbuf:range(0, pktlen))
            pktinfo.cols.info:append('  DOORKNOCK')
        end
    end

    state.cmd = cmd.NONE




    if my_state == state.IDLE then
        pktinfo.cols.info:set('  ✓')
        tree:add(f.usb.magic, tvbuf:range(0, 4))

    elseif my_state == state.ISSUED then
        local boilerplate = tree:add(f.boilerplate, tvbuf:range(0, 17))
        boilerplate:add(f.usb.magic, tvbuf:range(0, 4))
        boilerplate:add_le(f.ctl.magic, tvbuf:range(4, 4))

        boilerplate:add_le(f.ctl.len, tvbuf:range(8, 4))

        local flags = boilerplate:add(f.ctl.flags, tvbuf:range(12, 2))
        flags:add(f.ctl.direction, tvbuf:range(12, 2))

        local ctl_direction_in = ctl_direction().value
        if ctl_direction_in then
            pktinfo.cols.info:set('Command packet ◀—□')
        else
            pktinfo.cols.info:set('Command packet □—▶')
        end

        boilerplate:add_le(f.ctl.cmdlen, tvbuf:range(14, 1))


        boilerplate:add_le(f.ctl.opcode, tvbuf:range(15, 2))
        tree:add(f.ctl.addr, tvbuf:range(17, 4))
        tree:add(f.ctl.cmd,  tvbuf:range(21, 1))
        if my_cmd == BLIT then
            tree:add(f.ctl.blit.arg1, tvbuf:range(22, 2))
            tree:add(f.ctl.blit.arg2, tvbuf:range(24, 2))
            tree:add(f.ctl.blit.arg3, tvbuf:range(26, 2))
            tree:add(f.ctl.blit.arg4, tvbuf:range(28, 2))
        else
            tree:add(f.ctl.arg1, tvbuf:range(22, 2))
            tree:add(f.ctl.arg2, tvbuf:range(24, 2))
            tree:add(f.ctl.arg3, tvbuf:range(26, 2))
            tree:add(f.ctl.arg4, tvbuf:range(28, 2))
        end
    end


    return pktlen
end

----------------------------------------
-- we want to have our protocol dissection invoked for a specific UDP port,
-- so get the udp dissector table and add our protocol to it
DissectorTable.get('usb.bulk'):add(65535, eink)

