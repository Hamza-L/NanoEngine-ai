#ifndef NE_COMMAND_RECORDER_H
#define NE_COMMAND_RECORDER_H

/* ── cmd resource pool ─────────────────────────────────────────────── */

#include <stdint.h>

typedef struct NECommandStream { uint32_t id; } NECommandStream;
#define NE_COMMAND_HANDLE_NULL ((NECommandStream){0})

typedef enum {
    NE_RENDER_BEGIN_FRAME,
    NE_RENDER_END_FRAME,
    NE_RENDERPASS_SET_PIP,
    NE_RENDERPASS_VERT_BUFF,
    NE_RENDERPASS_IND_BUFF,
    NE_RENDERPASS_DRAW,
    NE_COMMAND_TYPE_COUNT,
} NECommandType;

typedef struct NECommandParam {
    uint32_t size;
    char* data[256];
} NECommandParam;

typedef struct NECommandData{
    NECommandParam params[8];
} NECommandData;

#define CMD_REGISTER_TYPE(X) sizeof(X)
void ne_command_register_cmd(NECommandType type, void* func, uint32_t paramCount, ...);

NECommandStream ne_command_create_command_stream();
void ne_command_reset_command_stream(NECommandStream stream);

void ne_command_record(NECommandStream recording, void* func);
void ne_command_push_param(NECommandStream recording, void* data, size_t size);

void ne_command_record_on_current_stream(void* func);
void ne_command_push_param_on_current_stream(void* data, size_t size);

void ne_command_stream_replay(NECommandStream recording);

#endif //NE_COMMAND_RECORDER_H
