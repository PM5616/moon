#include "lua_service.h"
#include "message.hpp"
#include "server.h"
#include "worker.h"
#include "common/hash.hpp"
#include "rapidjson/document.h"
#include "service_config.hpp"
#include "server_config.hpp"

using namespace moon;

constexpr size_t mb_memory = 1024 * 1024;

void *lua_service::lalloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    lua_service *l = reinterpret_cast<lua_service *>(ud);
    size_t mem = l->mem;

    l->mem += nsize;

    if (ptr)
        l->mem -= osize;

    if (l->mem_limit != 0 && l->mem > l->mem_limit)
    {
        if (ptr == nullptr || nsize > osize)
        {
            l->mem = mem;
            l->logger()->logstring(true, moon::LogLevel::Error,
                                   moon::format("%s Memory error current %.2f M, limit %.2f M", l->name().data(), (float)(l->mem) / mb_memory, (float)l->mem_limit / mb_memory), l->id());
            return nullptr;
        }
    }

    if (l->mem > l->mem_report)
    {
        l->mem_report *= 2;
        l->logger()->logstring(true, moon::LogLevel::Warn,
                               moon::format("%s Memory warning %.2f M", l->name().data(), (float)l->mem / mb_memory), l->id());
    }

    if (nsize == 0)
    {
        free(ptr);
        return nullptr;
    }
    else
    {
        return ::realloc(ptr, nsize);
    }
}

lua_service::lua_service()
    : lua_(sol::default_at_panic, lalloc, this)
{
}

lua_service::~lua_service()
{
}

bool lua_service::init(std::string_view config)
{
    try
    {
        service_config_parser<lua_service> conf;
        MOON_CHECK(conf.parse(this, config), "lua service init failed: parse config failed.");
        auto luafile = conf.get_value<std::string>("file");
        MOON_CHECK(!luafile.empty(), "lua service init failed: config does not provide lua file.");
        mem_limit = static_cast<size_t>(conf.get_value<int64_t>("memlimit"));

        lua_.open_libraries();

        lua_.add_package_loader(custom_package_loader);

        sol::table module = lua_.create_table();
        lua_bind lua_bind(module);
        lua_bind.bind_service(this)
            .bind_log(logger(), id())
            .bind_util()
            .bind_timer(this)
            .bind_message()
            .bind_socket(this)
            .bind_datetime(this);

        lua_bind::registerlib(lua_.lua_state(), "mooncore", module);

        open_custom_libraries(lua_.lua_state());

        sol::object json = lua_.require("json", luaopen_json, false);

        {
            auto cpaths = conf.get_value<std::string_view>("cpath");
            std::string strpath;
            strpath.append("package.cpath ='");
            strpath.append(cpaths);
            strpath.append(router_->get_env("CPATH"));
            strpath.append("'..package.cpath");
            lua_.script(strpath);
        }

        {
            auto paths = conf.get_value<std::string_view>("path");
            std::string strpath;
            strpath.append("package.path ='");
            strpath.append(paths);
            strpath.append(router_->get_env("PATH"));
            strpath.append("'..package.path");
            lua_.script(strpath);
        }

        sol::load_result load_result = lua_.load_file(luafile);
        if (!load_result.valid())
        {
            auto errmsg = sol::stack::get<std::string>(load_result.lua_state(), -1);
            MOON_CHECK(false, moon::format("lua service init failed: %s.", errmsg.data()));
        }
        sol::table tconfig = json.as<sol::table>().get<sol::function>("decode").call(config).get<sol::table>();
        sol::protected_function_result call_result = load_result.call(tconfig);
        if (!call_result.valid())
        {
            sol::error err = call_result;
            MOON_CHECK(false, moon::format("lua service init failed: %s.", err.what()));
        }

        if (unique())
        {
            MOON_CHECK(router_->set_unique_service(name(), id()), moon::format("lua service init failed: unique service name %s repeated.", name().data()).data());
        }

        logger()->logstring(true, moon::LogLevel::Info, moon::format("[WORKER %u] new service [%s:%08X]", worker_->id(), name().data(), id()), id());
        ok_ = true;
    }
    catch (std::exception &e)
    {
        CONSOLE_ERROR(logger(), "lua service init failed with config: %s", config.data());
        error(e.what(), false);
    }
    return ok_;
}

void lua_service::start()
{
    if (is_start() || !ok())
        return;
    service::start();
    try
    {
        if (start_.valid())
        {
            auto result = start_();
            if (!result.valid())
            {
                sol::error err = result;
                CONSOLE_ERROR(logger(), "%s", err.what());
            }
        }
    }
    catch (std::exception &e)
    {
        error(moon::format("lua_service::start :\n%s\n", e.what()));
    }
}

void lua_service::dispatch(message *msg)
{
    if (!ok())
        return;

    MOON_ASSERT(dispatch_.valid(), "should initialize callbacks first.")

    try
    {
        auto result = dispatch_(msg, msg->type());
        if (!result.valid())
        {
            sol::error err = result;
            if (msg->sessionid() >= 0 || msg->receiver() == 0) //socket mesage receiver==0
            {
                logger()->logstring(true, moon::LogLevel::Error, moon::format("%s dispatch:\n%s", name().data(), err.what()), id());
            }
            else
            {
                msg->set_sessionid(-msg->sessionid());
                router_->response(msg->sender(), "lua_service::dispatch "sv, err.what(), msg->sessionid(), PTYPE_ERROR);
            }
        }
    }
    catch (std::exception &e)
    {
        error(moon::format("lua_service::dispatch:\n%s\n", e.what()));
    }
}

void lua_service::on_timer(uint32_t timerid, bool remove)
{
    if (!ok())
        return;
    try
    {
        auto result = on_timer_(timerid, remove);
        if (!result.valid())
        {
            sol::error err = result;
            CONSOLE_ERROR(logger(), "%s", err.what());
        }
    }
    catch (std::exception &e)
    {
        error(moon::format("lua_service::on_timer:\n%s\n", e.what()));
    }
}

void lua_service::exit()
{
    if (!ok())
        return;

    try
    {
        if (exit_.valid())
        {
            auto result = exit_();
            if (!result.valid())
            {
                sol::error err = result;
                CONSOLE_ERROR(logger(), "%s", err.what());
            }
            return;
        }
    }
    catch (std::exception &e)
    {
        error(moon::format("lua_service::exit :%s\n", e.what()));
    }

    service::exit();
}

void lua_service::destroy()
{
    logger()->logstring(true, moon::LogLevel::Info, moon::format("[WORKER %u] destroy service [%s:%08X] ", worker_->id(), name().data(), id()), id());
    if (!ok())
        return;

    try
    {
        if (destroy_.valid())
        {
            auto result = destroy_();
            if (!result.valid())
            {
                sol::error err = result;
                CONSOLE_ERROR(logger(), "%s", err.what());
            }
        }
    }
    catch (std::exception &e)
    {
        error(moon::format("lua_service::destroy :%s\n", e.what()));
    }

    service::destroy();
}

void lua_service::error(const std::string &msg, bool initialized)
{
    CONSOLE_ERROR(logger(), "%s %s", name().data(), msg.data());

    if (initialized)
    {
        destroy();
        quit();
    }

    if (unique())
    {
        CONSOLE_ERROR(logger(), "unique service %s crashed, server will abort.", name().data());
        server_->stop();
    }
}

void lua_service::set_callback(char c, sol_function_t f)
{
    switch (c)
    {
    case 's':
    {
        start_ = f;
        break;
    }
    case 'm':
    {
        dispatch_ = f;
        break;
    }
    case 'e':
    {
        exit_ = f;
        break;
    }
    case 'd':
    {
        destroy_ = f;
        break;
    }
    case 't':
    {
        on_timer_ = f;
        break;
    }
    }
}
