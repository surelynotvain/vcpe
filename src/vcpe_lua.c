#include "vcpe_lua.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

static void *allocate(void *context, void *ptr, size_t old_size, size_t new_size)
{
    VcpeLua *vm = context;
    if (!ptr) old_size = 0;
    if (!new_size) {
        free(ptr);
        vm->used -= old_size;
        return NULL;
    }
    if (new_size > old_size && new_size - old_size > vm->limit - vm->used)
        return NULL;
    void *next = realloc(ptr, new_size);
    if (!next) return NULL;
    vm->used = vm->used - old_size + new_size;
    if (vm->used > vm->peak) vm->peak = vm->used;
    return next;
}

static void instruction_hook(lua_State *state, lua_Debug *debug)
{
    void *context = NULL;
    (void)debug;
    lua_getallocf(state, &context);
    VcpeLua *vm = context;
    vm->instructions += 10000;
    if (vm->instructions >= vm->instruction_limit)
        luaL_error(state, "script instruction limit exceeded");
}

static int open_libraries(lua_State *state)
{
    luaL_openlibs(state);
    return 0;
}

int vcpe_lua_open(VcpeLua *vm, size_t memory_limit)
{
    memset(vm, 0, sizeof(*vm));
    vm->limit = memory_limit;
    /* Lua 5.1's io library is not safe to abandon halfway through registering
     * the process standard streams. Reserve enough for complete library init. */
    if (memory_limit < 65536) {
        snprintf(vm->error, sizeof(vm->error), "Lua memory limit must be at least 64 KiB");
        return -1;
    }
    vm->state = lua_newstate(allocate, vm);
    if (!vm->state) {
        snprintf(vm->error, sizeof(vm->error), "cannot allocate Lua state");
        return -1;
    }
    /* lua_cpcall also protects creation of the initial C closure at tiny caps. */
    if (lua_cpcall(vm->state, open_libraries, NULL)) {
        const char *error = lua_tostring(vm->state, -1);
        snprintf(vm->error, sizeof(vm->error), "%s", error ? error : "Lua library startup failed");
        lua_close(vm->state);
        vm->state = NULL;
        return -1;
    }
    return 0;
}

void vcpe_lua_close(VcpeLua *vm)
{
    if (vm->state) lua_close(vm->state);
    vm->state = NULL;
}

void vcpe_lua_instruction_limit(VcpeLua *vm, unsigned limit)
{
    vm->instruction_limit = limit;
    lua_sethook(vm->state, limit ? instruction_hook : NULL, limit ? LUA_MASKCOUNT : 0, 10000);
}

int vcpe_lua_call(VcpeLua *vm, int nargs, int nresults)
{
    int base = lua_gettop(vm->state) - nargs - 1;
    if (base < 0) {
        snprintf(vm->error, sizeof(vm->error), "Lua stack has no function/arguments");
        return -1;
    }
    vm->instructions = 0;
    vm->error[0] = 0;
    if (lua_pcall(vm->state, nargs, nresults, 0)) {
        const char *error = lua_tostring(vm->state, -1);
        snprintf(vm->error, sizeof(vm->error), "%s", error ? error : "Lua call failed");
        lua_settop(vm->state, base);
        return -1;
    }
    return 0;
}

static int load_error(VcpeLua *vm, int code)
{
    if (!code) return 0;
    const char *error = lua_tostring(vm->state, -1);
    snprintf(vm->error, sizeof(vm->error), "%s", error ? error : "Lua load failed");
    lua_pop(vm->state, 1);
    return -1;
}

int vcpe_lua_file(VcpeLua *vm, const char *path)
{
    if (load_error(vm, luaL_loadfile(vm->state, path))) return -1;
    return vcpe_lua_call(vm, 0, 0);
}

int vcpe_lua_string(VcpeLua *vm, const char *source, const char *name)
{
    if (load_error(vm, luaL_loadbuffer(vm->state, source, strlen(source), name))) return -1;
    return vcpe_lua_call(vm, 0, 0);
}

const char *vcpe_lua_error(const VcpeLua *vm) { return vm->error; }
