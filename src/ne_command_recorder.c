#include "ne_command_recorder.h"
#include "ne_alloc.h"
#include "ne_log.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

#define MAX_COMMAND_PARAMS 8
#define DEFAULT_COMMAND_QUEUE_CAPACITY 32
// #define DEFINE_FUNC_TYPE_4(name, ret_T, param_T_1, param_T_2, param_T_3, param_T_4) typedef ret_T (*name)(param_T_1, param_T_2, param_T_3, param_T_4)
// #define DEFINE_FUNC_TYPE_3(name, ret_T, param_T_1, param_T_2, param_T_3) typedef ret_T (*name)(param_T_1, param_T_2, param_T_3)
// #define DEFINE_FUNC_TYPE_2(name, ret_T, param_T_1, param_T_2) typedef ret_T (*name)(param_T_1, param_T_2)
// #define DEFINE_FUNC_TYPE_1(name, ret_T, param_T_1) typedef ret_T (*name)(param_T_1)
// #define DEFINE_FUNC_TYPE(name, ret_T) typedef ret_T (*name)(void)

uint32_t NE_COMMAND_TYPE_ARGSIZEOF_LIST[NE_COMMAND_TYPE_COUNT][MAX_COMMAND_PARAMS] = {};
uint32_t NE_COMMAND_TYPE_ARGCOUNT_LIST[NE_COMMAND_TYPE_COUNT] = {};
void* NE_COMMAND_TYPE_FUNC_LIST[NE_COMMAND_TYPE_COUNT] = {};

typedef struct NECommand {
    void* func;
    uint32_t paramOffsets[MAX_COMMAND_PARAMS];
    char paramsData[256];
    uint32_t currParam;
    time_t time;
} NECommand;

typedef struct NECommandStreamSlot{
    bool occupied;
    time_t startTime;
    time_t endTime;
    time_t timeOfLastCommand;

    NECommand* cmdQueue;
    uint32_t count;
    uint32_t capacity;
} NECommandStreamSlot;

typedef struct NECommander {
    NECommandStreamSlot* cmdRecordings;
    uint32_t count;
    uint32_t capacity;

    NECommandStream currentStream;
} NECommander;

static NECommander* g_commander = {};

void ne_command_register_cmd(NECommandType type, void *func, uint32_t paramCount, ...) {
    va_list args;

    memset(&NE_COMMAND_TYPE_ARGSIZEOF_LIST[type], 0, sizeof(NE_COMMAND_TYPE_ARGSIZEOF_LIST[type]));

    NE_COMMAND_TYPE_FUNC_LIST[type] = func;
    NE_COMMAND_TYPE_ARGCOUNT_LIST[type] = paramCount;

    va_start(args, paramCount);
    for (uint32_t i = 0; i < paramCount; i++) {
        NE_COMMAND_TYPE_ARGSIZEOF_LIST[type][i] = va_arg(args, uint32_t);
    }
    va_end(args);
}

NECommandStream ne_command_create_command_stream() {
    if (!g_commander) {
        g_commander = (NECommander*)calloc(1, sizeof(NECommander));
    }

    uint32_t slot_index = ne_pool_alloc(
        (void **)&g_commander->cmdRecordings,
        &g_commander->count,
        &g_commander->capacity,
        sizeof(NECommandStreamSlot)
    );

    if (slot_index == UINT32_MAX) {
        NE_LOG_ERROR("failed to allocate buffer slot from pool");
        return NE_COMMAND_HANDLE_NULL;
    }

    NECommandStreamSlot *slot = &g_commander->cmdRecordings[slot_index];
    slot->occupied = true;
    slot->cmdQueue = (NECommand*)calloc(DEFAULT_COMMAND_QUEUE_CAPACITY, sizeof(NECommand));
    slot->capacity = DEFAULT_COMMAND_QUEUE_CAPACITY;
    slot->count = 0;

    time_t t;
    time(&t);

    slot->startTime = t;

    /* Return handle (1-based ID for null safety) */
    NECommandStream handle = {slot_index + 1};
    g_commander->currentStream = handle;
    return handle;
}

void ne_command_reset_command_stream(NECommandStream stream) {
    NECommandStreamSlot *slot = &g_commander->cmdRecordings[stream.id - 1];

    slot->occupied = true;
    slot->cmdQueue = (NECommand*)calloc(DEFAULT_COMMAND_QUEUE_CAPACITY, sizeof(NECommand));
    slot->capacity = DEFAULT_COMMAND_QUEUE_CAPACITY;
    slot->count = 0;

    time_t t;
    time(&t);

    slot->startTime = t;
}

void ne_command_record(NECommandStream recording, void *func) {
    if (!recording.id) {
        NE_LOG_ERROR("recording id is invalid\n");
        return;
    }

    if (!func) {
        NE_LOG_ERROR("no dispatch function provided\n");
        return;
    }


    NECommandStreamSlot *slot = &g_commander->cmdRecordings[recording.id - 1];
    NECommand *command = &slot->cmdQueue[slot->count++];
    memset(command, 0, sizeof(NECommand));

    NE_LOG_INFO("recording command %d for stream %d", slot->count, g_commander->currentStream.id);

    time_t t;
    time(&t);
    slot->timeOfLastCommand = t;
    command->time = t;
    command->func = func;
}

void ne_command_push_param(NECommandStream recording, void *data, size_t size) {
    if (!recording.id) {
        NE_LOG_ERROR("recording id is invalid\n");
        return;
    }

    NECommandStreamSlot *slot = &g_commander->cmdRecordings[recording.id - 1];
    NECommand *command = &slot->cmdQueue[slot->count - 1]; //top of the stack

    NE_LOG_INFO("pushing param %d for stream %d", command->currParam, g_commander->currentStream.id);

    size_t dataOffset = command->paramOffsets[command->currParam];
    memcpy(&command->paramsData[dataOffset], data, size);
    command->paramOffsets[command->currParam + 1] = dataOffset + size;
    command->currParam++;
}

void ne_command_record_on_current_stream(void *func) {
    ne_command_record(g_commander->currentStream, func);
}

void ne_command_push_param_on_current_stream(void *data, size_t size) {
    ne_command_push_param(g_commander->currentStream, data, size);
}

void ne_command_stream_replay(NECommandStream recording) {
    NECommandStreamSlot *slot = &g_commander->cmdRecordings[recording.id - 1];
    for (uint32_t i = 0; i < slot->count; i++) {
        NECommand *command = &slot->cmdQueue[i]; //top of the stack

        typedef void(*func_cmd)(void*, void*);



        ((func_cmd)command->func)(
            &command->paramsData[command->paramOffsets[0]],
            &command->paramsData[command->paramOffsets[1]]
            );
    }
}
