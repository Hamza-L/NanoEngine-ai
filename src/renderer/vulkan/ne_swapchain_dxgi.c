/*
 * DXGI + DirectComposition swapchain backend.
 *
 * Creates a DXGI swapchain with DXGI_ALPHA_MODE_PREMULTIPLIED and presents
 * through DirectComposition, enabling per-pixel transparency. Back buffers
 * are shared with Vulkan via VK_KHR_external_memory_win32 so the renderer
 * draws into them with its existing pipeline.
 *
 * Sync model (Option A — CPU fence wait):
 *   The renderer submits to Vulkan with a fence. Before calling Present1()
 *   we wait on that fence to guarantee the GPU has finished rendering. This
 *   adds ~0.1ms of CPU latency but is simple and correct.
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>

/*
 * dcomp.h is C++ only (uses references, overloads). We load DCompositionCreateDevice
 * dynamically and define the minimal COM interfaces in C ourselves.
 */
#include <initguid.h>

/* DXGI GUIDs we reference (defined here to avoid linking dxguid.lib) */
DEFINE_GUID(IID_IDXGIDevice_NE,      0x54ec77fa, 0x1377, 0x44e6, 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c);
DEFINE_GUID(IID_IDXGIFactory2_NE,    0x50c83a1c, 0xe072, 0x4c48, 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0);
DEFINE_GUID(IID_IDXGISwapChain3_NE,  0x94d99bdb, 0xf1f8, 0x4ab0, 0xb2, 0x36, 0x7d, 0xa0, 0x17, 0x0e, 0xda, 0xb1);
DEFINE_GUID(IID_IDXGIResource1_NE,   0x30961379, 0x4609, 0x4a41, 0x99, 0x8e, 0x54, 0xfe, 0x56, 0x7e, 0xe0, 0xc1);
DEFINE_GUID(IID_ID3D11Texture2D_NE,  0x6f15aaf2, 0xd208, 0x4e89, 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c);
DEFINE_GUID(IID_IDXGIKeyedMutex_NE,  0x9d8e1289, 0xd7b3, 0x465f, 0x81, 0x26, 0x25, 0x0e, 0x34, 0x9a, 0xf8, 0x5d);

/* DirectComposition GUIDs */
DEFINE_GUID(IID_IDCompositionDevice_NE, 0xC37EA93A, 0xE7AA, 0x450D, 0xB1, 0x6F, 0x97, 0x46, 0xCB, 0x04, 0x07, 0xF3);
DEFINE_GUID(IID_IDCompositionTarget_NE, 0xEACDD04C, 0x117E, 0x4E17, 0x88, 0xF4, 0xD1, 0xB1, 0x2B, 0x0E, 0x13, 0x00);
DEFINE_GUID(IID_IDCompositionVisual_NE, 0x4D93059D, 0x097B, 0x4651, 0x9A, 0x60, 0xF0, 0xF2, 0x51, 0x16, 0xE2, 0xF3);

typedef struct IDCompositionDevice_C IDCompositionDevice_C;
typedef struct IDCompositionTarget_C IDCompositionTarget_C;
typedef struct IDCompositionVisual_C IDCompositionVisual_C;

typedef struct IDCompositionDeviceVtbl {
    /* IUnknown */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDCompositionDevice_C *This, REFIID riid, void **ppvObject);
    ULONG (STDMETHODCALLTYPE *AddRef)(IDCompositionDevice_C *This);
    ULONG (STDMETHODCALLTYPE *Release)(IDCompositionDevice_C *This);
    /* IDCompositionDevice */
    HRESULT (STDMETHODCALLTYPE *Commit)(IDCompositionDevice_C *This);
    HRESULT (STDMETHODCALLTYPE *WaitForCommitCompletion)(IDCompositionDevice_C *This);
    HRESULT (STDMETHODCALLTYPE *GetFrameStatistics)(IDCompositionDevice_C *This, void *stats);
    HRESULT (STDMETHODCALLTYPE *CreateTargetForHwnd)(IDCompositionDevice_C *This, HWND hwnd, BOOL topmost, IDCompositionTarget_C **target);
    HRESULT (STDMETHODCALLTYPE *CreateVisual)(IDCompositionDevice_C *This, IDCompositionVisual_C **visual);
    /* remaining methods not needed */
} IDCompositionDeviceVtbl;

struct IDCompositionDevice_C { const IDCompositionDeviceVtbl *lpVtbl; };

typedef struct IDCompositionTargetVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDCompositionTarget_C *This, REFIID riid, void **ppvObject);
    ULONG (STDMETHODCALLTYPE *AddRef)(IDCompositionTarget_C *This);
    ULONG (STDMETHODCALLTYPE *Release)(IDCompositionTarget_C *This);
    HRESULT (STDMETHODCALLTYPE *SetRoot)(IDCompositionTarget_C *This, IDCompositionVisual_C *visual);
} IDCompositionTargetVtbl;

struct IDCompositionTarget_C { const IDCompositionTargetVtbl *lpVtbl; };

typedef struct IDCompositionVisualVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IDCompositionVisual_C *This, REFIID riid, void **ppvObject);
    ULONG (STDMETHODCALLTYPE *AddRef)(IDCompositionVisual_C *This);
    ULONG (STDMETHODCALLTYPE *Release)(IDCompositionVisual_C *This);
    HRESULT (STDMETHODCALLTYPE *SetOffsetX_float)(IDCompositionVisual_C *This, float x);
    HRESULT (STDMETHODCALLTYPE *SetOffsetX_anim)(IDCompositionVisual_C *This, void *anim);
    HRESULT (STDMETHODCALLTYPE *SetOffsetY_float)(IDCompositionVisual_C *This, float y);
    HRESULT (STDMETHODCALLTYPE *SetOffsetY_anim)(IDCompositionVisual_C *This, void *anim);
    HRESULT (STDMETHODCALLTYPE *SetTransform_matrix)(IDCompositionVisual_C *This, const void *matrix);
    HRESULT (STDMETHODCALLTYPE *SetTransform_obj)(IDCompositionVisual_C *This, void *transform);
    HRESULT (STDMETHODCALLTYPE *SetTransformParent)(IDCompositionVisual_C *This, void *parent);
    HRESULT (STDMETHODCALLTYPE *SetEffect)(IDCompositionVisual_C *This, void *effect);
    HRESULT (STDMETHODCALLTYPE *SetBitmapInterpolationMode)(IDCompositionVisual_C *This, int mode);
    HRESULT (STDMETHODCALLTYPE *SetBorderMode)(IDCompositionVisual_C *This, int mode);
    HRESULT (STDMETHODCALLTYPE *SetClip_rect)(IDCompositionVisual_C *This, const void *rect);
    HRESULT (STDMETHODCALLTYPE *SetClip_obj)(IDCompositionVisual_C *This, void *clip);
    HRESULT (STDMETHODCALLTYPE *SetContent)(IDCompositionVisual_C *This, IUnknown *content);
    /* remaining methods not needed */
} IDCompositionVisualVtbl;

struct IDCompositionVisual_C { const IDCompositionVisualVtbl *lpVtbl; };

typedef HRESULT (WINAPI *PFN_DCompositionCreateDevice)(void *dxgiDevice, REFIID iid, void **dcompositionDevice);

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "ne_swapchain.h"
#include "ne_log.h"
#include "ne_window.h"

#include <stdlib.h>
#include <string.h>

/* ── Vulkan function pointers (resolved by the renderer) ───────────────── */

extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
extern PFN_vkCreateImage vkCreateImage;
extern PFN_vkDestroyImage vkDestroyImage;
extern PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
extern PFN_vkAllocateMemory vkAllocateMemory;
extern PFN_vkFreeMemory vkFreeMemory;
extern PFN_vkBindImageMemory vkBindImageMemory;
extern PFN_vkCreateImageView vkCreateImageView;
extern PFN_vkDestroyImageView vkDestroyImageView;
extern PFN_vkDeviceWaitIdle vkDeviceWaitIdle;
extern PFN_vkCreateFence vkCreateFence;
extern PFN_vkDestroyFence vkDestroyFence;
extern PFN_vkWaitForFences vkWaitForFences;
extern PFN_vkResetFences vkResetFences;
extern PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;

/* VK_KHR_external_memory_win32 function (loaded at creation) */
static PFN_vkGetMemoryWin32HandlePropertiesKHR s_vkGetMemoryWin32HandlePropertiesKHR;

/* ── Constants ─────────────────────────────────────────────────────────── */

#define NE_DXGI_BUFFER_COUNT 3

/* ── Backend-specific data ─────────────────────────────────────────────── */

typedef struct NESwapchainDXGI {
    NESwapchainI base;

    VkDevice device;
    VkPhysicalDevice phys;
    VkInstance instance;
    NEWindow *window;
    bool vsync;

    /* D3D11 (minimal — used only for DXGI resource ownership) */
    ID3D11Device *d3d_device;
    IDXGISwapChain1 *dxgi_swapchain;

    /* DirectComposition */
    IDCompositionDevice_C *dcomp_device;
    IDCompositionTarget_C *dcomp_target;
    IDCompositionVisual_C *dcomp_visual;
    HMODULE dcomp_lib;

    /* Intermediate shared textures (Vulkan renders into these, D3D11 copies to swapchain) */
    ID3D11Texture2D *shared_textures[NE_DXGI_BUFFER_COUNT];
    HANDLE shared_handles[NE_DXGI_BUFFER_COUNT];
    VkDeviceMemory vk_memories[NE_DXGI_BUFFER_COUNT];
    ID3D11DeviceContext *d3d_context;

    /* Sync: CPU fence for present safety */
    VkFence present_fence;
    bool present_fence_submitted;

    VkPhysicalDeviceMemoryProperties mem_props;
} NESwapchainDXGI;

/* ── Helpers ───────────────────────────────────────────────────────────── */

static uint32_t ne_dxgi_find_memory_type(NESwapchainDXGI *sc, uint32_t type_filter,
                                          VkMemoryPropertyFlags properties) {
    for (uint32_t i = 0; i < sc->mem_props.memoryTypeCount; i++) {
        if ((type_filter & (1u << i)) &&
            (sc->mem_props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

static void ne_dxgi_release_vk_images(NESwapchainDXGI *sc) {
    NESwapchainI *base = &sc->base;

    if (base->image_views) {
        for (uint32_t i = 0; i < base->image_count; i++) {
            if (base->image_views[i] != VK_NULL_HANDLE) {
                vkDestroyImageView(sc->device, base->image_views[i], NULL);
            }
        }
        free(base->image_views);
        base->image_views = NULL;
    }

    if (base->images) {
        for (uint32_t i = 0; i < base->image_count; i++) {
            if (base->images[i] != VK_NULL_HANDLE) {
                vkDestroyImage(sc->device, base->images[i], NULL);
            }
        }
        free(base->images);
        base->images = NULL;
    }

    for (uint32_t i = 0; i < NE_DXGI_BUFFER_COUNT; i++) {
        if (sc->vk_memories[i] != VK_NULL_HANDLE) {
            vkFreeMemory(sc->device, sc->vk_memories[i], NULL);
            sc->vk_memories[i] = VK_NULL_HANDLE;
        }
        if (sc->shared_handles[i]) {
            CloseHandle(sc->shared_handles[i]);
            sc->shared_handles[i] = NULL;
        }
        if (sc->shared_textures[i]) {
            ID3D11Texture2D_Release(sc->shared_textures[i]);
            sc->shared_textures[i] = NULL;
        }
    }

    base->image_count = 0;
}

/*
 * Create intermediate shared textures and import them into Vulkan.
 *
 * The DXGI swapchain buffers themselves can't be shared (they're owned by the
 * compositor). Instead we create our own D3D11 textures with SHARED_NTHANDLE,
 * import them into Vulkan as render targets, and at present time copy from the
 * shared texture to the swapchain back buffer via D3D11.
 *
 * Flow per buffer:
 *   D3D11CreateTexture2D(SHARED_NTHANDLE) → CreateSharedHandle → HANDLE
 *   VkImportMemoryWin32HandleInfoKHR → vkAllocateMemory → vkBindImageMemory
 */
static bool ne_dxgi_import_buffers(NESwapchainDXGI *sc) {
    NESwapchainI *base = &sc->base;
    HRESULT hr;

    base->images = (VkImage *)calloc(NE_DXGI_BUFFER_COUNT, sizeof(VkImage));
    base->image_views = (VkImageView *)calloc(NE_DXGI_BUFFER_COUNT, sizeof(VkImageView));
    if (!base->images || !base->image_views) return false;

    base->image_count = NE_DXGI_BUFFER_COUNT;

    /* Get D3D11 device context for copy operations at present time */
    if (!sc->d3d_context) {
        ID3D11Device_GetImmediateContext(sc->d3d_device, &sc->d3d_context);
    }

    for (uint32_t i = 0; i < NE_DXGI_BUFFER_COUNT; i++) {
        /* Create a shared D3D11 texture that Vulkan can import */
        D3D11_TEXTURE2D_DESC tex_desc;
        memset(&tex_desc, 0, sizeof(tex_desc));
        tex_desc.Width = base->extent.width;
        tex_desc.Height = base->extent.height;
        tex_desc.MipLevels = 1;
        tex_desc.ArraySize = 1;
        tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        tex_desc.SampleDesc.Count = 1;
        tex_desc.Usage = D3D11_USAGE_DEFAULT;
        tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        tex_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

        hr = ID3D11Device_CreateTexture2D(sc->d3d_device, &tex_desc, NULL, &sc->shared_textures[i]);
        if (FAILED(hr) || !sc->shared_textures[i]) {
            NE_LOG_ERROR("D3D11CreateTexture2D (shared %u) failed (hr=0x%08lx)", i, hr);
            return false;
        }

        /* Get shared handle from the texture */
        IDXGIResource1 *dxgi_resource = NULL;
        hr = ID3D11Texture2D_QueryInterface(sc->shared_textures[i], &IID_IDXGIResource1_NE, (void **)&dxgi_resource);
        if (FAILED(hr) || !dxgi_resource) {
            NE_LOG_ERROR("QueryInterface(IDXGIResource1) for shared tex %u failed (hr=0x%08lx)", i, hr);
            return false;
        }

        HANDLE shared_handle = NULL;
        hr = IDXGIResource1_CreateSharedHandle(
            dxgi_resource,
            NULL,
            DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
            NULL,
            &shared_handle);
        IDXGIResource1_Release(dxgi_resource);

        if (FAILED(hr) || !shared_handle) {
            NE_LOG_ERROR("CreateSharedHandle(%u) failed (hr=0x%08lx)", i, hr);
            return false;
        }

        sc->shared_handles[i] = shared_handle;

        /* Query what Vulkan memory types are compatible with this handle */
        VkMemoryWin32HandlePropertiesKHR handle_props;
        memset(&handle_props, 0, sizeof(handle_props));
        handle_props.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;

        VkResult vr = s_vkGetMemoryWin32HandlePropertiesKHR(
            sc->device,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,
            shared_handle,
            &handle_props);

        if (vr != VK_SUCCESS) {
            NE_LOG_ERROR("vkGetMemoryWin32HandlePropertiesKHR(%u) failed (vr=%d)", i, (int)vr);
            return false;
        }

        /* Create VkImage backed by external memory */
        VkExternalMemoryImageCreateInfo ext_img_ci;
        memset(&ext_img_ci, 0, sizeof(ext_img_ci));
        ext_img_ci.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        ext_img_ci.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;

        VkImageCreateInfo img_ci;
        memset(&img_ci, 0, sizeof(img_ci));
        img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_ci.pNext = &ext_img_ci;
        img_ci.imageType = VK_IMAGE_TYPE_2D;
        img_ci.format = base->format;
        img_ci.extent.width = base->extent.width;
        img_ci.extent.height = base->extent.height;
        img_ci.extent.depth = 1;
        img_ci.mipLevels = 1;
        img_ci.arrayLayers = 1;
        img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
        img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        img_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        img_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        vr = vkCreateImage(sc->device, &img_ci, NULL, &base->images[i]);
        if (vr != VK_SUCCESS) {
            NE_LOG_ERROR("vkCreateImage (DXGI import %u) failed (vr=%d)", i, (int)vr);
            return false;
        }

        /* Get memory requirements to know allocation size */
        VkMemoryRequirements mem_req;
        vkGetImageMemoryRequirements(sc->device, base->images[i], &mem_req);

        /* Find a compatible memory type from what the handle supports */
        uint32_t mem_type = ne_dxgi_find_memory_type(
            sc,
            handle_props.memoryTypeBits & mem_req.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (mem_type == UINT32_MAX) {
            /* Fallback: try any compatible type without requiring device-local */
            mem_type = ne_dxgi_find_memory_type(sc, handle_props.memoryTypeBits & mem_req.memoryTypeBits, 0);
        }
        if (mem_type == UINT32_MAX) {
            NE_LOG_ERROR("no compatible memory type for DXGI import (buffer %u)", i);
            return false;
        }

        /* Import the shared handle as Vulkan memory */
        VkImportMemoryWin32HandleInfoKHR import_info;
        memset(&import_info, 0, sizeof(import_info));
        import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
        import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        import_info.handle = shared_handle;

        VkMemoryDedicatedAllocateInfo dedicated_info;
        memset(&dedicated_info, 0, sizeof(dedicated_info));
        dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated_info.pNext = &import_info;
        dedicated_info.image = base->images[i];

        VkMemoryAllocateInfo alloc_info;
        memset(&alloc_info, 0, sizeof(alloc_info));
        alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc_info.pNext = &dedicated_info;
        alloc_info.allocationSize = mem_req.size;
        alloc_info.memoryTypeIndex = mem_type;

        vr = vkAllocateMemory(sc->device, &alloc_info, NULL, &sc->vk_memories[i]);
        if (vr != VK_SUCCESS) {
            NE_LOG_ERROR("vkAllocateMemory (DXGI import %u) failed (vr=%d)", i, (int)vr);
            return false;
        }

        vr = vkBindImageMemory(sc->device, base->images[i], sc->vk_memories[i], 0);
        if (vr != VK_SUCCESS) {
            NE_LOG_ERROR("vkBindImageMemory (DXGI import %u) failed (vr=%d)", i, (int)vr);
            return false;
        }

        /* Create image view */
        VkImageViewCreateInfo ivci;
        memset(&ivci, 0, sizeof(ivci));
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = base->images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = base->format;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.baseMipLevel = 0;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.baseArrayLayer = 0;
        ivci.subresourceRange.layerCount = 1;

        vr = vkCreateImageView(sc->device, &ivci, NULL, &base->image_views[i]);
        if (vr != VK_SUCCESS) {
            NE_LOG_ERROR("vkCreateImageView (DXGI import %u) failed (vr=%d)", i, (int)vr);
            return false;
        }
    }

    return true;
}

static bool ne_dxgi_build(NESwapchainDXGI *sc) {
    NESwapchainI *base = &sc->base;
    HRESULT hr;

    HWND hwnd = (HWND)ne_window_get_native_handle(sc->window, NE_NATIVE_HANDLE_WIN32_HWND);
    if (!hwnd) {
        NE_LOG_ERROR("ne_swapchain_dxgi: failed to get HWND");
        return false;
    }

    int32_t fb_w = 0, fb_h = 0;
    if (!ne_window_get_framebuffer_size(sc->window, &fb_w, &fb_h)) return false;
    if (fb_w <= 0 || fb_h <= 0) return false;

    base->extent.width = (uint32_t)fb_w;
    base->extent.height = (uint32_t)fb_h;
    base->format = VK_FORMAT_B8G8R8A8_UNORM;

    /* ── Create D3D11 device (minimal, for DXGI ownership) ──────────── */

    if (!sc->d3d_device) {
        D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

        hr = D3D11CreateDevice(
            NULL,
            D3D_DRIVER_TYPE_HARDWARE,
            NULL,
            flags,
            &feature_level, 1,
            D3D11_SDK_VERSION,
            &sc->d3d_device,
            NULL, NULL);

        if (FAILED(hr) || !sc->d3d_device) {
            NE_LOG_ERROR("D3D11CreateDevice failed (hr=0x%08lx)", hr);
            return false;
        }
    }

    /* ── Create DXGI swapchain ──────────────────────────────────────── */

    if (!sc->dxgi_swapchain) {
        IDXGIDevice *dxgi_device = NULL;
        hr = ID3D11Device_QueryInterface(sc->d3d_device, &IID_IDXGIDevice_NE, (void **)&dxgi_device);
        if (FAILED(hr)) {
            NE_LOG_ERROR("QueryInterface(IDXGIDevice) failed (hr=0x%08lx)", hr);
            return false;
        }

        IDXGIAdapter *adapter = NULL;
        hr = IDXGIDevice_GetAdapter(dxgi_device, &adapter);
        IDXGIDevice_Release(dxgi_device);
        if (FAILED(hr)) {
            NE_LOG_ERROR("IDXGIDevice::GetAdapter failed (hr=0x%08lx)", hr);
            return false;
        }

        IDXGIFactory2 *factory = NULL;
        hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2_NE, (void **)&factory);
        IDXGIAdapter_Release(adapter);
        if (FAILED(hr)) {
            NE_LOG_ERROR("IDXGIAdapter::GetParent(IDXGIFactory2) failed (hr=0x%08lx)", hr);
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 scd;
        memset(&scd, 0, sizeof(scd));
        scd.Width = base->extent.width;
        scd.Height = base->extent.height;
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = NE_DXGI_BUFFER_COUNT;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        scd.Flags = 0;

        hr = IDXGIFactory2_CreateSwapChainForComposition(
            factory,
            (IUnknown *)sc->d3d_device,
            &scd,
            NULL,
            &sc->dxgi_swapchain);
        IDXGIFactory2_Release(factory);

        if (FAILED(hr) || !sc->dxgi_swapchain) {
            NE_LOG_ERROR("CreateSwapChainForComposition failed (hr=0x%08lx)", hr);
            return false;
        }
    }

    /* ── Setup DirectComposition (loaded dynamically, C interfaces) ─── */

    if (!sc->dcomp_device) {
        if (!sc->dcomp_lib) {
            sc->dcomp_lib = LoadLibraryA("dcomp.dll");
            if (!sc->dcomp_lib) {
                NE_LOG_ERROR("failed to load dcomp.dll");
                return false;
            }
        }

        PFN_DCompositionCreateDevice pfnCreate =
            (PFN_DCompositionCreateDevice)GetProcAddress(sc->dcomp_lib, "DCompositionCreateDevice");
        if (!pfnCreate) {
            NE_LOG_ERROR("failed to get DCompositionCreateDevice");
            return false;
        }

        hr = pfnCreate(NULL, &IID_IDCompositionDevice_NE, (void **)&sc->dcomp_device);
        if (FAILED(hr) || !sc->dcomp_device) {
            NE_LOG_ERROR("DCompositionCreateDevice failed (hr=0x%08lx)", hr);
            return false;
        }

        hr = sc->dcomp_device->lpVtbl->CreateTargetForHwnd(sc->dcomp_device, hwnd, TRUE, &sc->dcomp_target);
        if (FAILED(hr) || !sc->dcomp_target) {
            NE_LOG_ERROR("CreateTargetForHwnd failed (hr=0x%08lx)", hr);
            return false;
        }

        hr = sc->dcomp_device->lpVtbl->CreateVisual(sc->dcomp_device, &sc->dcomp_visual);
        if (FAILED(hr) || !sc->dcomp_visual) {
            NE_LOG_ERROR("CreateVisual failed (hr=0x%08lx)", hr);
            return false;
        }

        hr = sc->dcomp_visual->lpVtbl->SetContent(sc->dcomp_visual, (IUnknown *)sc->dxgi_swapchain);
        if (FAILED(hr)) {
            NE_LOG_ERROR("IDCompositionVisual::SetContent failed (hr=0x%08lx)", hr);
            return false;
        }

        hr = sc->dcomp_target->lpVtbl->SetRoot(sc->dcomp_target, sc->dcomp_visual);
        if (FAILED(hr)) {
            NE_LOG_ERROR("IDCompositionTarget::SetRoot failed (hr=0x%08lx)", hr);
            return false;
        }

        hr = sc->dcomp_device->lpVtbl->Commit(sc->dcomp_device);
        if (FAILED(hr)) {
            NE_LOG_ERROR("IDCompositionDevice::Commit failed (hr=0x%08lx)", hr);
            return false;
        }
    }

    /* ── Load VK_KHR_external_memory_win32 function ────────────────── */

    if (!s_vkGetMemoryWin32HandlePropertiesKHR) {
        PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr_local =
            (PFN_vkGetDeviceProcAddr)vkGetInstanceProcAddr(sc->instance, "vkGetDeviceProcAddr");
        if (vkGetDeviceProcAddr_local) {
            s_vkGetMemoryWin32HandlePropertiesKHR =
                (PFN_vkGetMemoryWin32HandlePropertiesKHR)vkGetDeviceProcAddr_local(
                    sc->device, "vkGetMemoryWin32HandlePropertiesKHR");
        }
        if (!s_vkGetMemoryWin32HandlePropertiesKHR) {
            NE_LOG_ERROR("failed to load vkGetMemoryWin32HandlePropertiesKHR "
                         "(VK_KHR_external_memory_win32 not available?)");
            return false;
        }
    }

    /* ── Import DXGI buffers into Vulkan ────────────────────────────── */

    if (!ne_dxgi_import_buffers(sc)) {
        return false;
    }

    /* ── Create present fence ──────────────────────────────────────── */

    if (sc->present_fence == VK_NULL_HANDLE) {
        VkFenceCreateInfo fci;
        memset(&fci, 0, sizeof(fci));
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        VkResult vr = vkCreateFence(sc->device, &fci, NULL, &sc->present_fence);
        if (vr != VK_SUCCESS) {
            NE_LOG_ERROR("vkCreateFence (present) failed (vr=%d)", (int)vr);
            return false;
        }
    }

    base->needs_recreate = false;
    return true;
}

/* ── Interface implementation ──────────────────────────────────────────── */

static void ne_swapchain_dxgi_destroy(NESwapchainI *iface) {
    NESwapchainDXGI *sc = (NESwapchainDXGI *)iface;

    vkDeviceWaitIdle(sc->device);

    ne_dxgi_release_vk_images(sc);

    if (sc->present_fence != VK_NULL_HANDLE) {
        vkDestroyFence(sc->device, sc->present_fence, NULL);
        sc->present_fence = VK_NULL_HANDLE;
    }

    if (sc->d3d_context) { ID3D11DeviceContext_Release(sc->d3d_context); sc->d3d_context = NULL; }
    if (sc->dcomp_visual) { sc->dcomp_visual->lpVtbl->Release(sc->dcomp_visual); sc->dcomp_visual = NULL; }
    if (sc->dcomp_target) { sc->dcomp_target->lpVtbl->Release(sc->dcomp_target); sc->dcomp_target = NULL; }
    if (sc->dcomp_device) { sc->dcomp_device->lpVtbl->Release(sc->dcomp_device); sc->dcomp_device = NULL; }
    if (sc->dcomp_lib) { FreeLibrary(sc->dcomp_lib); sc->dcomp_lib = NULL; }
    if (sc->dxgi_swapchain) { IDXGISwapChain1_Release(sc->dxgi_swapchain); sc->dxgi_swapchain = NULL; }
    if (sc->d3d_device) { ID3D11Device_Release(sc->d3d_device); sc->d3d_device = NULL; }

    free(sc);
}

static bool ne_swapchain_dxgi_recreate(NESwapchainI *iface) {
    NESwapchainDXGI *sc = (NESwapchainDXGI *)iface;

    vkDeviceWaitIdle(sc->device);

    ne_dxgi_release_vk_images(sc);

    int32_t fb_w = 0, fb_h = 0;
    if (!ne_window_get_framebuffer_size(sc->window, &fb_w, &fb_h)) return false;
    if (fb_w <= 0 || fb_h <= 0) return false;

    iface->extent.width = (uint32_t)fb_w;
    iface->extent.height = (uint32_t)fb_h;

    HRESULT hr = IDXGISwapChain1_ResizeBuffers(
        sc->dxgi_swapchain,
        NE_DXGI_BUFFER_COUNT,
        (UINT)fb_w, (UINT)fb_h,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        0);

    if (FAILED(hr)) {
        NE_LOG_ERROR("IDXGISwapChain::ResizeBuffers failed (hr=0x%08lx)", hr);
        return false;
    }

    if (!ne_dxgi_import_buffers(sc)) {
        return false;
    }

    iface->needs_recreate = false;
    return true;
}

static NESwapchainAcquireResult ne_swapchain_dxgi_acquire(NESwapchainI *iface, VkSemaphore signal_sem) {
    NESwapchainDXGI *sc = (NESwapchainDXGI *)iface;
    (void)signal_sem;

    /*
     * DXGI flip model: GetCurrentBackBufferIndex() returns the index of the
     * buffer that is not currently being presented or queued for presentation.
     * It's safe to render into immediately — DXGI guarantees this.
     *
     * Note: unlike Vulkan WSI, there's no GPU semaphore signaled here.
     * The image is ready for rendering as soon as this call returns.
     * The signal_sem is unused (the renderer's fence-wait at frame start
     * already ensures the previous frame's commands are done).
     */
    IDXGISwapChain3 *sc3 = NULL;
    HRESULT hr = sc->dxgi_swapchain->lpVtbl->QueryInterface(
        (IDXGISwapChain1 *)sc->dxgi_swapchain, &IID_IDXGISwapChain3_NE, (void **)&sc3);
    if (FAILED(hr) || !sc3) {
        NE_LOG_ERROR("QueryInterface(IDXGISwapChain3) failed (hr=0x%08lx)", hr);
        return NE_SWAPCHAIN_ACQUIRE_FAILED;
    }

    UINT index = sc3->lpVtbl->GetCurrentBackBufferIndex(sc3);
    sc3->lpVtbl->Release((IDXGISwapChain3 *)sc3);

    if (index >= iface->image_count) {
        return NE_SWAPCHAIN_ACQUIRE_FAILED;
    }

    iface->acquired_image_index = index;
    return NE_SWAPCHAIN_ACQUIRE_SUCCESS;
}

static NESwapchainPresentResult ne_swapchain_dxgi_present(NESwapchainI *iface, VkQueue queue, VkSemaphore wait_sem) {
    NESwapchainDXGI *sc = (NESwapchainDXGI *)iface;
    (void)queue;
    (void)wait_sem;

    /*
     * Option A sync: wait for Vulkan rendering to complete on CPU before
     * calling DXGI Present. The renderer submits with fences_in_flight[frame],
     * and begin_frame waits on it. But that's for the NEXT use of that frame
     * slot — we need the CURRENT frame to be done NOW.
     *
     * The simplest correct approach: vkDeviceWaitIdle. This is a sledgehammer
     * but it's correct and simple. We can refine to per-frame fence later.
     */
    vkDeviceWaitIdle(sc->device);

    /* Acquire keyed mutex on the shared texture, copy to swapchain, then release */
    uint32_t image_index = iface->acquired_image_index;

    IDXGIKeyedMutex *mutex = NULL;
    HRESULT hr = ID3D11Texture2D_QueryInterface(sc->shared_textures[image_index],
        &IID_IDXGIKeyedMutex_NE, (void **)&mutex);
    if (SUCCEEDED(hr) && mutex) {
        hr = IDXGIKeyedMutex_AcquireSync(mutex, 0, 5000);
        if (SUCCEEDED(hr)) {
            ID3D11Texture2D *back_buffer = NULL;
            hr = IDXGISwapChain1_GetBuffer(sc->dxgi_swapchain, image_index, &IID_ID3D11Texture2D_NE, (void **)&back_buffer);
            if (SUCCEEDED(hr) && back_buffer) {
                ID3D11DeviceContext_CopyResource(sc->d3d_context, (ID3D11Resource *)back_buffer, (ID3D11Resource *)sc->shared_textures[image_index]);
                ID3D11Texture2D_Release(back_buffer);
            }
            IDXGIKeyedMutex_ReleaseSync(mutex, 0);
        }
        IDXGIKeyedMutex_Release(mutex);
    }

    UINT sync_interval = sc->vsync ? 1 : 0;
    DXGI_PRESENT_PARAMETERS params;
    memset(&params, 0, sizeof(params));

    hr = IDXGISwapChain1_Present1(sc->dxgi_swapchain, sync_interval, 0, &params);

    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        NE_LOG_ERROR("DXGI device lost (hr=0x%08lx)", hr);
        return NE_SWAPCHAIN_PRESENT_FAILED;
    }

    if (FAILED(hr)) {
        NE_LOG_ERROR("IDXGISwapChain::Present1 failed (hr=0x%08lx)", hr);
        return NE_SWAPCHAIN_PRESENT_FAILED;
    }

    return NE_SWAPCHAIN_PRESENT_SUCCESS;
}

/* ── Vtable ────────────────────────────────────────────────────────────── */

static const NESwapchainOps g_swapchain_dxgi_ops = {
    .destroy  = ne_swapchain_dxgi_destroy,
    .recreate = ne_swapchain_dxgi_recreate,
    .acquire  = ne_swapchain_dxgi_acquire,
    .present  = ne_swapchain_dxgi_present,
};

/* ── Public constructor ────────────────────────────────────────────────── */

NESwapchainI *ne_swapchain_dxgi_create(const NESwapchainDXGIDesc *desc) {
    if (!desc || !desc->device || !desc->phys || !desc->instance || !desc->window) {
        return NULL;
    }

    NESwapchainDXGI *sc = (NESwapchainDXGI *)calloc(1, sizeof(NESwapchainDXGI));
    if (!sc) return NULL;

    sc->base.ops = &g_swapchain_dxgi_ops;
    sc->device = desc->device;
    sc->phys = desc->phys;
    sc->instance = desc->instance;
    sc->window = desc->window;
    sc->vsync = desc->vsync;

    vkGetPhysicalDeviceMemoryProperties(sc->phys, &sc->mem_props);

    if (!ne_dxgi_build(sc)) {
        ne_swapchain_dxgi_destroy(&sc->base);
        return NULL;
    }

    return &sc->base;
}
