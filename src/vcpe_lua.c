/* VCPE Lua: a sandboxed Lua 5.1 VM for games whose original logic is in Lua. Two limits protect the game from a
 * runaway script: a memory cap (every allocation goes through allocate(), which refuses to go over the limit) and an
 * instruction cap (a hook every 10000 instructions raises an error once the budget for one call is spent). Errors
 * never crash: they are caught and kept as text in vm->error. */
#include "vcpe_lua.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

/* Lua's allocator: realloc with a byte budget, counting current and peak use. */
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

/* Called by Lua every 10000 instructions: count, and abort the script when it has run too long. */
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

/* Create the VM with its memory cap and open the standard libraries. */
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

/* Call the function on the stack in protected mode (errors become a message, the stack is cleaned up), with a fresh
 * instruction budget. */
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

/* Repeatable scripts: scripts that seed randomness from os.time() or os.clock() would differ every run. In autotest
 * runs, these replacements return a fixed time and a clock advancing 1/60 s per call. */
static double fixed_t, fixed_clock;
static int fixed_time(lua_State *L) { lua_pushnumber(L, fixed_t); return 1; }
static int fixed_clock_fn(lua_State *L) { fixed_clock += 1.0 / 60; lua_pushnumber(L, fixed_clock); return 1; }

void vcpe_lua_fixed_clock(VcpeLua *vm, double t)
{
    fixed_t = t;
    fixed_clock = 0;
    lua_getglobal(vm->state, "os");
    if (lua_istable(vm->state, -1)) {
        lua_pushcfunction(vm->state, fixed_time);
        lua_setfield(vm->state, -2, "time");
        lua_pushcfunction(vm->state, fixed_clock_fn);
        lua_setfield(vm->state, -2, "clock");
    }
    lua_pop(vm->state, 1);
}
