#include "ne_command_recorder.h"
#include "ne_alloc.h"
#include "ne_log.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

#define MAX_COMMAND_PARAMS 6
#define DEFAULT_COMMAND_QUEUE_CAPACITY 32

typedef struct NECommand {
    void* func;
    uint32_t paramOffsets[MAX_COMMAND_PARAMS];
    char paramsData[256];
    uint32_t paramCount;
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
    bool recording;
} NECommandStreamSlot;

typedef struct NECommander {
    NECommandStreamSlot* cmdRecordings;
    uint32_t count;
    uint32_t capacity;
    bool replaying;

    NECommandStream currentStream;
} NECommander;

static NECommander* g_commander = {};

NECommandStream ne_command_start_command_stream() {
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
    slot->recording = true;
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
    slot->recording = true;
    slot->cmdQueue = (NECommand*)calloc(DEFAULT_COMMAND_QUEUE_CAPACITY, sizeof(NECommand));
    slot->capacity = DEFAULT_COMMAND_QUEUE_CAPACITY;
    slot->count = 0;

    time_t t;
    time(&t);

    slot->startTime = t;
}

void ne_command_end_command_stream(NECommandStream stream) {
    NECommandStreamSlot *slot = &g_commander->cmdRecordings[stream.id - 1];
    slot->recording = false;
}

void ne_command_end_current_command_stream() {
    NECommandStreamSlot *slot = &g_commander->cmdRecordings[g_commander->currentStream.id - 1];
    slot->recording = false;
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

    if(g_commander->replaying) return;


    NECommandStreamSlot *slot = &g_commander->cmdRecordings[recording.id - 1];
    if(!slot->recording) return;

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

    if(g_commander->replaying) return;

    NECommandStreamSlot *slot = &g_commander->cmdRecordings[recording.id - 1];
    if(!slot->recording) return;

    NECommand *command = &slot->cmdQueue[slot->count - 1]; //top of the stack

    NE_LOG_INFO("pushing param %d for stream %d", command->paramCount, g_commander->currentStream.id);

    size_t dataOffset = command->paramOffsets[command->paramCount];
    memcpy(&command->paramsData[dataOffset], &data, size);
    command->paramOffsets[command->paramCount + 1] = dataOffset + size;
    command->paramCount++;
}

void ne_command_record_on_current_stream(void *func) {
    ne_command_record(g_commander->currentStream, func);
}

void ne_command_push_param_on_current_stream(void *data, size_t size) {
    ne_command_push_param(g_commander->currentStream, data, size);
}

void ne_command_stream_replay(NECommandStream recording) {
    NECommandStreamSlot *slot = &g_commander->cmdRecordings[recording.id - 1];
    g_commander->replaying = true;
    for (uint32_t i = 0; i < slot->count; i++) {
        NECommand *command = &slot->cmdQueue[i]; //top of the stack

        switch (command->paramCount) {
            case 0: {
                ((void(*)())command->func)();
                break;
            }
            case 1: {
                ((void(*)(void*))command->func)(
                    *(void**)&command->paramsData[command->paramOffsets[0]]
                );
                break;
            }
            case 2: {
                ((void(*)(void*, void*))command->func)(
                    *(void**)&command->paramsData[command->paramOffsets[0]],
                    *(void**)&command->paramsData[command->paramOffsets[1]]
                );
                break;
            }
            case 3: {
                ((void(*)(void*, void*, void*))command->func)(
                    *(void**)&command->paramsData[command->paramOffsets[0]],
                    *(void**)&command->paramsData[command->paramOffsets[1]],
                    *(void**)&command->paramsData[command->paramOffsets[2]]
                );
                break;
            }
            case 4: {
                ((void(*)(void*, void*, void*, void*))command->func)(
                    *(void**)&command->paramsData[command->paramOffsets[0]],
                    *(void**)&command->paramsData[command->paramOffsets[1]],
                    *(void**)&command->paramsData[command->paramOffsets[2]],
                    *(void**)&command->paramsData[command->paramOffsets[3]]
                );
                break;
            }
        }
    }
    g_commander->replaying = false;
}
