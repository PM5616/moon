require("base.io")
require("base.os")
require("base.string")
require("base.table")
require("base.math")
require("base.util")
require("base.class")

---@type core
local core = require("mooncore")
local json = require("json")
local seri = require("seri")
-- local buffer = require("buffer")

local pairs = pairs
local type = type
local setmetatable = setmetatable
local tremove = table.remove
local tointeger = math.tointeger

local jencode = json.encode

local co_create = coroutine.create
local co_running = coroutine.running
local co_yield = coroutine.yield
local co_resume = coroutine.resume

local _send = core.send

local unpack = seri.unpack
local pack = seri.pack

local PTYPE_SYSTEM = 1
local PTYPE_TEXT = 2
local PTYPE_LUA = 3
local PTYPE_SOCKET = 4
local PTYPE_ERROR = 5
local PTYPE_SOCKET_WS = 6
local PTYPE_DEBUG = 7

---@class moon : core
local moon = {
    PTYPE_TEXT = PTYPE_TEXT,
    PTYPE_LUA = PTYPE_LUA,
    PTYPE_SOCKET = PTYPE_SOCKET,
    PTYPE_SOCKET_WS = PTYPE_SOCKET_WS
}

setmetatable(moon, {__index = core})

moon.cache = require("codecache")

moon.pack = pack
moon.unpack = unpack

--export global variable
local _g = _G
moon.exports = {}
setmetatable(
    moon.exports,
    {
        __newindex = function(_, name, value)
            rawset(_g, name, value)
        end,
        __index = function(_, name)
            return rawget(_g, name)
        end
    }
)

-- disable create unexpected global variable
setmetatable(
    _g,
    {
        __newindex = function(_, name,value)
            if name:sub(1,4)~='sol.' then --ignore sol2 registed library
                local msg = string.format('USE "moon.exports.%s = <value>" INSTEAD OF SET GLOBAL VARIABLE', name)
                print(debug.traceback(msg, 2))
                print("")
            else
                rawset(_g, name, value)
            end
        end
    }
)

moon.add_package_path = function(p)
    package.path = package.path .. p
end

local sid_ = core.id()

local uuid = 0
local session_id_coroutine = {}
local protocol = {}
local session_watcher = {}

local function coresume(co, ...)
    local ok, err = co_resume(co, ...)
    if not ok then
        error(debug.traceback(co, err))
    end
    return ok, err
end

---make map<coroutine,sessionid>
local function make_response(receiver)
    uuid = uuid + 1
    if uuid == 0x7FFFFFFF then
        uuid = 1
    end

    assert(nil == session_id_coroutine[uuid])

    if receiver then
        session_watcher[uuid] = receiver
    end

    session_id_coroutine[uuid] = co_running()
    return uuid
end

--- 取消等待session的回应
function moon.cancel_session(sessionid)
    session_id_coroutine[sessionid] = false
end

moon.make_response = make_response

---
---注册服务启动回掉函数,这个函数的调用时机:
---1.如果服务是在配置文件中配置的服务，回掉函数会在所有配置的服务
---被创建之后调用。所以可以在回掉函数内查询其它唯一服务的 serviceid。
---2.运行时动态创建的服务，第一次update之前调用
---@param callback fun()
function moon.start(callback)
    core.set_cb('s', callback)
end

---注册进程退出信号回掉,注册此回掉后, 除非调用moon.quit, 否则服务不会退出。
---在回掉函数中可以处理异步逻辑（如带协程的数据库访问操作，收到退出信号后，保存数据）。
---注意：处理完成后必须要调用moon.quit,使服务自身退出,否则server进程将无法正常退出。
---@param callback fun()
function moon.exit(callback)
    assert(callback)
    core.set_cb('e', callback)
end

---注册服务对象销毁时的回掉函数，这个函数会在服务正常销毁时调用
---@param callback fun()
function moon.destroy(callback)
    core.set_cb('d', callback)
end

---@param msg message
---@param PTYPE string
local function _default_dispatch(msg, PTYPE)
    local p = protocol[PTYPE]
    if not p then
        error(string.format( "handle unknown PTYPE: %s. sender %u",PTYPE, msg:sender()))
    end

    local sessionid = msg:sessionid()
    if sessionid > 0 and PTYPE ~= PTYPE_ERROR then
        session_watcher[sessionid] = nil
        local co = session_id_coroutine[sessionid]
        if co then
            session_id_coroutine[sessionid] = nil
            --print(coroutine.status(co))
            if p.unpack then
                coresume(co, p.unpack(msg:cstr()))
            else
                coresume(co, msg)
            end
            --print(coroutine.status(co))
            return
        end

        if co ~= false then
            error(string.format( "%s: response [%u] can not find co.",moon.name(), sessionid))
        end
	else
        if not p.dispatch then
			error(string.format( "[%s] dispatch PTYPE [%u] is nil",moon.name(), p.PTYPE))
			return
        end
        p.dispatch(msg, p.unpack)
    end
end

core.set_cb('m', _default_dispatch)

---
---向指定服务发送消息,消息内容会根据协议类型进行打包
---@param PTYPE string @协议类型
---@param receiver integer @接收者服务id
---@param header string @message header
---@param vararg any
---@return boolean
function moon.send(PTYPE, receiver, header, ...)
    local p = protocol[PTYPE]
    if not p then
        error(string.format("moon send unknown PTYPE[%s] message", PTYPE))
    end
    header = header or ''
    _send(sid_, receiver, p.pack(...), header, 0, p.PTYPE)
    return true
end

---向指定服务发送消息，消息内容不进行协议打包
---@param PTYPE string @协议类型
---@param receiver integer @接收者服务id
---@param header string @Message Header
---@param data string|userdata @消息内容
---@param sessionid integer
---@return boolean
function moon.raw_send(PTYPE, receiver, header, data, sessionid)
	local p = protocol[PTYPE]
    if not p then
        error(string.format("moon send unknown PTYPE[%s] message", PTYPE))
    end
    header = header or ''
    sessionid = sessionid or 0
    _send(sid_, receiver, data, header, sessionid, p.PTYPE)
	return true
end

---获取当前的服务id
---@return integer
function moon.sid()
    return sid_
end

---async 创建一个新的服务
---@param stype string @服务类型，根据所注册的服务类型，可选有 'lua'
---@param config table @服务的启动配置，数据类型table, 可以用来向服务传递参数
---@param unique boolean @default false, 是否是唯一服务，唯一服务可以用moon.queryservice(name)查询服务id
---@param workerid integer @default 0 ,在指定工作者线程创建该服务，并绑定该线程。默认0,服务将轮询加入工作者线程。
---@return integer @返回服务id
function moon.new_service(stype, config, unique, workerid)
    unique = unique or false
    workerid = workerid or 0
    config = jencode(config)
    local sessionid = make_response()
    core.new_service(stype, config, unique, workerid, sid_, sessionid)
    return tointeger(co_yield())
end

---异步移除指定的服务
---@param sid integer @服务id
---@param iswait boolean @可选，为true时可以 使用协程等待移除结果
---@return string
function moon.remove_service(sid, iswait)
    if iswait then
        local sessionid = make_response()
        core.remove_service(sid,sid_,sessionid)
        return co_yield()
    else
        core.remove_service(sid,0,0)
    end
end

---使当前服务退出
function moon.quit()
    moon.remove_service(sid_)
end

---根据服务name获取服务id,注意只能查询创建时配置unique=true的服务
---@param name string
---@return integer @ 0 表示服务不存在
function moon.queryservice(name)
	if type(name)=='string' then
		return core.queryservice(name)
	end
	return name
end

function moon.set_env_pack(name, ...)
    return core.set_env(name, seri.packs(...))
end

function moon.get_env_unpack(name)
    return seri.unpack(core.get_env(name))
end

---获取服务器时间, 可以调用 moon.advtime 偏移时间
function moon.time()
    return moon.now()//1000
end

-------------------------协程操作封装--------------------------

local co_num = 0

local co_pool = setmetatable({}, {__mode = "kv"})

local function routine(fn)
    local co = co_running()
    while true do
        co_num = co_num + 1
        fn()
        co_num = co_num - 1
        co_pool[#co_pool + 1] = co
        fn = co_yield()
    end
end

---启动一个异步
---@param func fun()
---@return thread
function moon.async(func)
    local co = tremove(co_pool)
    if not co then
        co = co_create(routine)
    end
    local _, res = coresume(co, func) --func 作为 routine 的参数
    if res then
        return res
    end
    return co
end

---返回运行中的协程个数,和协程池空闲的协程个数
function moon.coroutine_num()
    return co_num, #co_pool
end

--------------------------timer-------------
local function remove_all_timer(t)
    for k,_ in pairs(t) do
        core.remove_timer(k)
    end
end

local timer_cb = setmetatable({},{
	__gc = remove_all_timer,
})

---@param mills integer
---@param times integer
---@param fn function
---@return integer
function moon.repeated(mills, times, fn)
    local timerid = core.repeated(mills, times)
    timer_cb[timerid] = fn
    return timerid
end

---@param timerid integer
function moon.remove_timer(timerid)
    timer_cb[timerid] = nil
    core.remove_timer(timerid)
end

core.set_cb('t',
    function(timerid, last)
        local cb = timer_cb[timerid]
        if cb then
            cb(timerid, last)
        end

        if last then
            timer_cb[timerid] = nil
        end
    end
)

local _repeated = moon.repeated

---async
---异步等待 mills 毫秒
---@param mills integer
---@return integer
function moon.sleep(mills)
    local co = co_running()
    _repeated(
        mills,
        1,
        function(tid)
            coresume(co, tid)
        end
    )
    return co_yield()
end

local json_object_mt = {__jsonobject=true}
function moon.make_json_object()
    return setmetatable({}, json_object_mt)
end

------------------------------------------

---async
---@param serviceid integer
---@return string
function moon.co_remove_service(serviceid)
    return moon.remove_service(serviceid, true)
end

---@param command string
---@return string
function moon.co_runcmd(command)
    local sessionid = make_response()
    core.runcmd(moon.sid(), command, sessionid)
    return co_yield()
end

---RPC形式调用，发送消息附带一个responseid，对方收到后把responseid发送回来，必须调用moon.response应答.
---@param PTYPE string @协议类型
---@param receiver integer @接收者服务id
---@param vararg any @发送的数据
---@return any|bool,string @如果没有错误, 返回调用结果。如果发生错误第一个参数是false,后面是错误信息。
function moon.co_call(PTYPE, receiver, ...)
    local p = protocol[PTYPE]
    if not p then
        error(string.format("moon call unknown PTYPE[%s] message", PTYPE))
    end
    local sessionid = make_response(receiver)
	_send(sid_, receiver, p.pack(...), "", sessionid, p.PTYPE)
    return co_yield()
end

---回应moon.call
---@param PTYPE string @协议类型
---@param receiver integer  @接收者服务id
---@param sessionid integer
---@param vararg any @返回的数据
function moon.response(PTYPE, receiver, sessionid, ...)
    if sessionid == 0 then return end
    local p = protocol[PTYPE]
    if not p then
        error("handle unknown message")
    end
    _send(sid_, receiver, p.pack(...), '', sessionid, p.PTYPE)
end

------------------------------------
function moon.register_protocol(t)
    local PTYPE = t.PTYPE
    if protocol[PTYPE] then
        print("Warning attemp register duplicated PTYPE", t.name)
    end
    protocol[PTYPE] = t
    protocol[t.name] = t
end

local reg_protocol = moon.register_protocol


---设置指定协议消息的消息处理函数
---@param PTYPE string
---@param cb fun(msg:message,ptype:table)
---@return boolean
function moon.dispatch(PTYPE, cb)
    local p = protocol[PTYPE]
    if cb then
        local ret = p.dispatch
        p.dispatch = cb
        return ret
    else
        return p and p.dispatch
    end
end

reg_protocol {
    name = "lua",
    PTYPE = PTYPE_LUA,
    pack = seri.pack,
    unpack = unpack,
    dispatch = function()
        error("PTYPE_LUA dispatch not implemented")
    end
}

reg_protocol {
    name = "text",
    PTYPE = PTYPE_TEXT,
    pack = function(...)
        return ...
    end,
    unpack = moon.tostring,
    dispatch = function()
        error("PTYPE_TEXT dispatch not implemented")
    end
}

reg_protocol {
    name = "error",
    PTYPE = PTYPE_ERROR,
    pack = function(...)
        return ...
    end,
    unpack = moon.tostring,
    dispatch = function(msg, unpack_fn)
        local sessionid = msg:sessionid()
        local content = msg:header()
        local data = unpack_fn(msg:cstr())
        if data and #data >0 then
            content = content..":"..data
        end
        local co = session_id_coroutine[sessionid]
        if co then
            session_id_coroutine[sessionid] = nil
            coresume(co, false, content)
            return
        end
    end
}

local system_command = {}

local ref_services = {}

--- mark a service, should not exit before this service
moon.retain =function(name)
    local id = moon.queryservice(name)
    if id == 0 then return end
    moon.send("system", id, "retain", moon.name())
end

moon.release =function(name)
    local id = moon.queryservice(name)
    if id == 0 then return end
    moon.send("system", id, "release", moon.name())
end

moon.co_wait_exit = function()
    while true do
        local num = 0
        for _,_ in pairs(ref_services) do
            num=num+1
        end
        if num ==0 then
            break
        else
            moon.sleep(100)
        end
    end
    moon.quit()
end

system_command.exit = function(sender, msg)
    local data = msg:bytes()
    for k, v in pairs(session_watcher) do
        if v == sender then
            local co = session_id_coroutine[k]
            if co then
                session_id_coroutine[k] = nil
                coresume(co, false, data)
                return
            end
        end
    end
end

system_command.retain = function(_, msg)
    local data = msg:bytes()
    ref_services[data] = true
end

system_command.release = function(_, msg)
    local data = msg:bytes()
    ref_services[data] = nil
end

reg_protocol {
    name = "system",
    PTYPE = PTYPE_SYSTEM,
    pack = function(...)
        return ...
    end,
    unpack = moon.tostring,
    dispatch = function(msg, _)
        local sender = msg:sender()
        local header = msg:header()
        local func = system_command[header]
        if func then
            func(sender, msg)
        end
    end
}

reg_protocol{
    name = "socket",
    PTYPE = PTYPE_SOCKET,
    pack = function(...) return ... end,
    dispatch = function()
        error("PTYPE_SOCKET dispatch not implemented")
    end
}

reg_protocol{
    name = "websocket",
    PTYPE = PTYPE_SOCKET_WS,
    pack = function(...) return ... end,
    dispatch = function(_)
        error("PTYPE_SOCKET_WS dispatch not implemented")
    end
}

--------------------------DEBUG----------------------------

local debug_command = {}

debug_command.gc = function(sender, sessionid)
    collectgarbage("collect")
    moon.response("debug",sender,sessionid, collectgarbage("count"))
end

debug_command.mem = function(sender, sessionid)
    moon.response("debug",sender,sessionid, collectgarbage("count"))
end

debug_command.ping = function(sender, sessionid)
    moon.response("debug",sender,sessionid, "pong")
end

debug_command.state = function(sender, sessionid)
    local running_num, free_num = moon.coroutine_num()
    local s = string.format("coroutine-running %d coroutine-free %d cpu:%d",running_num,free_num, moon.cpu())
    moon.response("debug",sender,sessionid, s)
end

reg_protocol {
    name = "debug",
    PTYPE = PTYPE_DEBUG,
    pack = pack,
    unpack = unpack,
    dispatch = function(msg, unpack_fn)
        local sender = msg:sender()
        local sessionid = msg:sessionid()
        local params = {unpack_fn(msg:cstr())}
        local func = debug_command[params[1]]
        if func then
            func(sender, sessionid, table.unpack(params,2))
        else
            moon.response("debug",sender,sessionid, "unknow debug cmd "..params[1])
        end
    end
}

return moon
