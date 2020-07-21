local moon = require("moon")

local conf = ...

if conf.slave then
    local command = {}

    command.QUIT = function ()
        print("recv quit cmd, bye bye")
        moon.quit()
    end

    local function docmd(sender,header,...)
    -- body
        local f = command[header]
        if f then
            f(sender,...)
        else
            error(string.format("Unknown command %s", tostring(header)))
        end
    end

    moon.start(function()
        print("conf:", conf.message)

        moon.dispatch('lua',function(msg,unpack)
            local sender = msg:sender()
            local header = msg:header()
            docmd(sender,header, unpack(msg:cstr()))
        end)

        if conf.auto_quit then
            print("auto quit, bye bye")
            -- 使服务退出
            moon.quit()
        end
    end)
else
    moon.start(function()
        moon.async(function()
            -- 动态创建服务, 配置同时可以用来传递一些信息
            moon.new_service("lua", {
                name = "create_service",
                file = "example_create_service.lua",
                message = "Hello create_service",
                slave = true,
                auto_quit = true
            })

            -- 动态创建服务，获得服务ID，方便用来通信
            local serviceid =  moon.new_service("lua", {
                name = "create_service",
                file = "example_create_service.lua",
                slave = true,
                message = "Hello create_service2"
            })

            print("new service",string.format("%X",serviceid))

            moon.send("lua", serviceid, "QUIT")

            moon.abort()
        end)
    end)

end



