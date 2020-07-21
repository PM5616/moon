#include "lua.hpp"
#include "config.hpp"
#include "common/buffer.hpp"

using namespace moon;

static int clear(lua_State* L)
{
    auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
    if (buf == NULL) { return luaL_error(L, "null buffer pointer"); }
    buf->clear();
    return 0;
}

static int size(lua_State* L)
{
    auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
    if (buf == NULL) { return luaL_error(L, "null buffer pointer"); }
    lua_pushinteger(L, buf->size());
    return 1;
}

static int substr(lua_State* L)
{
    auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
    if (buf == NULL) { return luaL_error(L, "null buffer pointer"); }
    auto pos = static_cast<size_t>(luaL_checkinteger(L, 2));
    auto count = static_cast<size_t>(luaL_checkinteger(L, 3));

    std::string_view sw(buf->data(), buf->size());
    std::string_view sub = sw.substr(pos, count);
    lua_pushlstring(L, sub.data(), sub.size());
    return 1;
}

static int str(lua_State* L)
{
    auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
    if (buf == NULL) { return luaL_error(L, "null buffer pointer"); }
    lua_pushlstring(L, buf->data(), buf->size());
    return 1;
}

static int cstr(lua_State* L)
{
    auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
    if (buf == NULL) { return luaL_error(L, "null buffer pointer"); }
    int offset = 0;
    if (lua_type(L, 2) == LUA_TNUMBER)
    {
        offset = static_cast<int>(lua_tointeger(L, 2));
        if (offset > static_cast<int>(buf->size()))
        {
            return luaL_error(L, "out of range");
        }
    }
    lua_pushlightuserdata(L, (void*)(buf->data() + offset));
    lua_pushinteger(L, buf->size() - offset);
    return 2;
}

static int read(lua_State* L)
{
    auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
    if (buf == NULL) { return luaL_error(L, "null buffer pointer"); }
    auto count = static_cast<int>(luaL_checkinteger(L, 2));
    if (count > static_cast<int>(buf->size()))
    {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "out off index");
        return 2;
    }

    lua_pushlstring(L, buf->data(), count);
    buf->seek(count);
    return 1;
}

static int write_front(lua_State* L)
{
    auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
    if (buf == NULL) { return luaL_error(L, "null buffer pointer"); }
    size_t len = 0;
    auto data = luaL_checklstring(L, 2, &len);
    bool ok = buf->write_front(data, len);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static int write_back(lua_State* L)
{
    auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
    if (buf == NULL) { return luaL_error(L, "null buffer pointer"); }
    size_t len = 0;
    auto data = luaL_checklstring(L, 2, &len);
    buf->write_back(data, len);
    return 0;
}

static int seek(lua_State* L)
{
    auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
    if (buf == NULL) { return luaL_error(L, "null buffer pointer"); }
    auto pos = static_cast<int>(luaL_checkinteger(L, 2));
    auto origin = buffer::seek_origin::Current;
    if (lua_type(L, 3) == LUA_TNUMBER)
    {
        origin = static_cast<buffer::seek_origin>(luaL_checkinteger(L, 3));
    }
    buf->seek(pos, origin);
    return 0;
}

static int commit(lua_State* L)
{
    auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
    if (buf == NULL) { return luaL_error(L, "null buffer pointer"); }
    auto n = static_cast<int64_t>(luaL_checkinteger(L, 2));
    if (0 == n) { return luaL_error(L, "Invalid buffer commit param"); }
    buf->commit(n);
    return 0;
}

static int prepare(lua_State* L)
{
    auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
    if (buf == NULL) { return luaL_error(L, "null buffer pointer"); }
    auto n = static_cast<int64_t>(luaL_checkinteger(L, 2));
    if (0 == n) { return luaL_error(L, "Invalid buffer prepare param"); }
    buf->prepare(n);
    return 0;
}

static int unsafe_delete(lua_State* L)
{
    auto buf = reinterpret_cast<buffer*>(lua_touserdata(L, 1));
    if (buf == NULL) { return luaL_error(L, "null buffer pointer"); }
    delete buf;
    return 0;
}

static int unsafe_new(lua_State* L)
{
    size_t capacity = static_cast<size_t>(luaL_checkinteger(L, 1));
    buffer* buf = new buffer(capacity, BUFFER_HEAD_RESERVED);
    lua_pushlightuserdata(L, buf);
    return 1;
}

extern "C"
{
    int LUAMOD_API luaopen_buffer(lua_State* L)
    {
        luaL_Reg l[] = {
            {"unsafe_new",unsafe_new},
            {"delete",unsafe_delete },
            { "clear",clear },
            { "size",size },
            { "substr", substr},
            { "str", str},
            { "cstr", cstr},
            { "read", read},
            { "write_front", write_front},
            { "write_back", write_back},
            { "seek", seek},
            { "commit", commit},
            { "prepare", prepare},
            {NULL,NULL}
        };
        luaL_checkversion(L);
        luaL_newlib(L, l);
        return 1;
    }
}

