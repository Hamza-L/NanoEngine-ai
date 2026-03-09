#include "ne_app.h"
#include "ne_log.h"
#include "ne_renderer.h"
#include "ne_renderer_buffer.h"
#include "ne_window.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if TESTING_ENABLED

/* ── Test Data ──────────────────────────────────────────────────────────── */

typedef struct Vertex {
    float position[2];
    float color[3];
} Vertex;

static const Vertex k_test_vertices[] = {
    {{ 0.0f,  0.5f}, {1.0f, 0.0f, 0.0f}},
    {{-0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},
    {{ 0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}},
};

static const uint16_t k_test_indices[] = {0, 1, 2};

/* ── Test Callbacks ────────────────────────────────────────────────────────── */

[[maybe_unused]] static void on_close(NEWindow *window, void *user_data) {
    (void)window;
    (void)user_data;
}

[[maybe_unused]] static void on_key_down(NEWindow *window, NEKeyEvent event, void *user_data) {
    (void)user_data;
    if (event.key == NE_KEY_ESCAPE) {
        ne_window_request_close(window);
    }
}

/* ── Buffer Tests ──────────────────────────────────────────────────────── */

/**
 * Test basic buffer creation and destruction.
 */
static bool test_buffer_create_destroy(NERenderer *renderer) {
    NE_LOG_INFO("=== Test: Buffer Create/Destroy ===");

    /* Create a small buffer */
    NEBufferHandle buf = ne_buffer_create(renderer, &(NEBufferDesc){
        .size = 256,
        .usage = NE_BUFFER_USAGE_VERTEX,
        .initial_data = NULL,
    });

    if (!ne_buffer_handle_valid(buf)) {
        NE_LOG_ERROR("failed to create buffer");
        return false;
    }
    NE_LOG_INFO("✓ buffer created (handle id=%u, size=256 bytes)", buf.id);

    /* Destroy the buffer */
    ne_buffer_destroy(renderer, buf);
    NE_LOG_INFO("✓ buffer destroyed");

    /* Test destroying invalid handle (should be no-op) */
    ne_buffer_destroy(renderer, NE_BUFFER_HANDLE_NULL);
    NE_LOG_INFO("✓ destroying null handle is safe");

    return true;
}

/**
 * Test buffer creation with initial data.
 */
static bool test_buffer_initial_data(NERenderer *renderer) {
    NE_LOG_INFO("=== Test: Buffer Initial Data ===");

    /* Create vertex buffer with initial data */
    NEBufferHandle vbo = ne_buffer_create(renderer, &(NEBufferDesc){
        .size = sizeof(k_test_vertices),
        .usage = NE_BUFFER_USAGE_VERTEX,
        .initial_data = k_test_vertices,
    });

    if (!ne_buffer_handle_valid(vbo)) {
        NE_LOG_ERROR("failed to create vertex buffer");
        return false;
    }
    NE_LOG_INFO("✓ vertex buffer created with initial data (handle id=%u, size=%zu bytes)",
                vbo.id, sizeof(k_test_vertices));

    /* Create index buffer with initial data */
    NEBufferHandle ibo = ne_buffer_create(renderer, &(NEBufferDesc){
        .size = sizeof(k_test_indices),
        .usage = NE_BUFFER_USAGE_INDEX,
        .initial_data = k_test_indices,
    });

    if (!ne_buffer_handle_valid(ibo)) {
        NE_LOG_ERROR("failed to create index buffer");
        ne_buffer_destroy(renderer, vbo);
        return false;
    }
    NE_LOG_INFO("✓ index buffer created with initial data (handle id=%u, size=%zu bytes)",
                ibo.id, sizeof(k_test_indices));

    /* Cleanup */
    ne_buffer_destroy(renderer, vbo);
    ne_buffer_destroy(renderer, ibo);
    NE_LOG_INFO("✓ buffers destroyed successfully");

    return true;
}

/**
 * Test buffer update functionality.
 */
static bool test_buffer_update(NERenderer *renderer) {
    NE_LOG_INFO("=== Test: Buffer Update ===");

    /* Create a buffer */
    NEBufferHandle buf = ne_buffer_create(renderer, &(NEBufferDesc){
        .size = sizeof(k_test_vertices),
        .usage = NE_BUFFER_USAGE_VERTEX,
        .initial_data = k_test_vertices,
    });

    if (!ne_buffer_handle_valid(buf)) {
        NE_LOG_ERROR("failed to create buffer");
        return false;
    }
    NE_LOG_INFO("✓ buffer created (handle id=%u)", buf.id);

    /* Update buffer data */
    Vertex updated_vertices[] = {
        {{ 0.2f,  0.5f}, {1.0f, 0.5f, 0.0f}},
        {{-0.5f, -0.3f}, {0.5f, 1.0f, 0.0f}},
        {{ 0.5f, -0.3f}, {0.0f, 0.5f, 1.0f}},
    };

    ne_buffer_update(renderer, buf, updated_vertices, sizeof(updated_vertices), 0);
    NE_LOG_INFO("✓ buffer updated successfully (size=%zu bytes)", sizeof(updated_vertices));

    /* Test partial update */
    Vertex single_vertex = {{ 0.1f, 0.2f}, {1.0f, 1.0f, 1.0f}};
    ne_buffer_update(renderer, buf, &single_vertex, sizeof(single_vertex), 0);
    NE_LOG_INFO("✓ partial buffer update successful (offset=0, size=%zu bytes)", sizeof(single_vertex));

    /* Test update on invalid handle (should be safe no-op) */
    ne_buffer_update(renderer, NE_BUFFER_HANDLE_NULL, &single_vertex, sizeof(single_vertex), 0);
    NE_LOG_INFO("✓ updating null handle is safe");

    /* Cleanup */
    ne_buffer_destroy(renderer, buf);
    NE_LOG_INFO("✓ buffer destroyed");

    return true;
}

/**
 * Test multiple buffer usage flags.
 */
static bool test_buffer_usage_flags(NERenderer *renderer) {
    NE_LOG_INFO("=== Test: Buffer Usage Flags ===");

    const uint32_t test_cases[] = {
        NE_BUFFER_USAGE_VERTEX,
        NE_BUFFER_USAGE_INDEX,
        NE_BUFFER_USAGE_UNIFORM,
        NE_BUFFER_USAGE_STORAGE,
        NE_BUFFER_USAGE_VERTEX | NE_BUFFER_USAGE_UNIFORM,  /* Combined flags */
    };

    const char *usage_names[] = {
        "VERTEX",
        "INDEX",
        "UNIFORM",
        "STORAGE",
        "VERTEX|UNIFORM",
    };

    for (uint32_t i = 0; i < sizeof(test_cases) / sizeof(test_cases[0]); i++) {
        NEBufferHandle buf = ne_buffer_create(renderer, &(NEBufferDesc){
            .size = 128,
            .usage = test_cases[i],
            .initial_data = NULL,
        });

        if (!ne_buffer_handle_valid(buf)) {
            NE_LOG_ERROR("failed to create buffer with usage flags: %s", usage_names[i]);
            return false;
        }
        NE_LOG_INFO("✓ buffer created with usage: %s", usage_names[i]);

        ne_buffer_destroy(renderer, buf);
    }

    return true;
}

/**
 * Main buffer test suite runner.
 * Returns true if all tests pass, false otherwise.
 */
bool test_buffer(NEApp *app, NEWindow *window) {
    NE_LOG_INFO("========== BUFFER TEST SUITE ==========");

    /* Create renderer */
    NERenderer *renderer = ne_renderer_create(app, &(NERendererDesc){.enable_validation = true});
    if (!renderer) {
        NE_LOG_ERROR("failed to create renderer");
        return false;
    }
    NE_LOG_INFO("✓ renderer created");

    /* Create surface */
    NERenderSurface *surface = ne_renderer_create_surface(renderer,
                                                          window,
                                                          &(NERenderSurfaceDesc){
                                                              .vsync = true,
                                                              .clear_color_rgba = {0.1f, 0.1f, 0.12f, 1.0f}});
    if (!surface) {
        NE_LOG_ERROR("failed to create render surface");
        ne_renderer_destroy(renderer);
        return false;
    }
    NE_LOG_INFO("✓ render surface created");

    /* Run test suite */
    bool all_passed = true;

    all_passed = test_buffer_create_destroy(renderer) && all_passed;
    NE_LOG_INFO("");

    all_passed = test_buffer_initial_data(renderer) && all_passed;
    NE_LOG_INFO("");

    all_passed = test_buffer_update(renderer) && all_passed;
    NE_LOG_INFO("");

    all_passed = test_buffer_usage_flags(renderer) && all_passed;
    NE_LOG_INFO("");

    /* Cleanup */
    ne_renderer_destroy_surface(renderer, surface);
    ne_renderer_destroy(renderer);
    NE_LOG_INFO("✓ renderer and surface destroyed");

    if (all_passed) {
        NE_LOG_INFO("========== ALL TESTS PASSED ==========");
    } else {
        NE_LOG_ERROR("========== SOME TESTS FAILED ==========");
    }

    return all_passed;
}

#endif /* TESTING_ENABLED */

