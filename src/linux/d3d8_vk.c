/*
 * d3d8_vk.c - the Vulkan renderer of d3d8.c (OFP_RENDERER=vulkan).
 *
 * Vulkan 1.3 with dynamic rendering, extended dynamic state and push
 * descriptors; memory through VMA (vma.cpp). Direct3D 8's fixed function is
 * one pair of shaders (vk/d3d8.vert, vk/d3d8.frag, compiled into
 * vk_shaders.h by tools/make_vk_shaders.sh) driven by three uniform blocks:
 * transform, lighting, pixel stages. A pipeline exists per vertex layout,
 * blend state, primitive class, fill mode, sample count and depth format;
 * the rest (depth, stencil, culling, bias, viewport) is dynamic state.
 *
 * The matrices go in as the game sets them: Vulkan's clip space has D3D's
 * depth (0..w); its y points down, so the shader flips it; pixel centres
 * are shifted half a pixel.
 *
 * Frames: two in flight, each with a draw command buffer, an upload one
 * (texture levels and static buffers, submitted first) and an arena of
 * host-visible memory for the frame's uniform blocks, dynamic and client
 * vertices and indices, and upload staging. A texture or a static buffer
 * changed after this frame drew with it gets a new image or buffer (the old
 * one goes when the frame is done), so earlier draws keep what they saw.
 */
#define VK_NO_PROTOTYPES
#include <math.h>
#include <dlfcn.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include "d3d8_int.h"
#include "vk_shaders.h"

SDL_Window *hle_screen_sdl_window(void);                                   /* screen.c */
void hle_screen_gl_picture(int src_w, int src_h, int x, int y, int w, int h);

/* ------------------------------------------------------------------ the loader */
#define VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceVersion)
#define VK_INSTANCE(X) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceFeatures) X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkDestroySurfaceKHR) \
    X(vkEnumerateDeviceExtensionProperties)
#define VK_DEVICE(X) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit) \
    X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) \
    X(vkDestroySemaphore) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkCreateShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkCreatePipelineCache) \
    X(vkCmdPipelineBarrier) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers2) X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdSetViewport) \
    X(vkCmdSetScissor) X(vkCmdSetDepthBias) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetStencilReference) X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetPrimitiveTopology) \
    X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetStencilTestEnable) \
    X(vkCmdSetStencilOp) X(vkCmdSetDepthBiasEnable) X(vkCmdPushDescriptorSetKHR) X(vkCmdPushConstants) \
    X(vkCmdClearAttachments) X(vkCmdClearColorImage) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage) X(vkCmdResolveImage) X(vkCmdSetDepthBoundsTestEnable)
#define VK_PFN(n) static PFN_##n n;
VK_GLOBAL(VK_PFN) VK_INSTANCE(VK_PFN) VK_DEVICE(VK_PFN)
static PFN_vkGetInstanceProcAddr gipa;

static int load_global(void) {
    if (!gipa) {
        void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_GLOBAL);
        if (!lib) return 0;
        gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
        if (!gipa) return 0;
    }
#define VK_LOAD0(n) n = (PFN_##n)gipa(NULL, #n);
    VK_GLOBAL(VK_LOAD0)
    return vkCreateInstance != NULL;
}
static void load_instance(VkInstance in) {
#define VK_LOADI(n) n = (PFN_##n)gipa(in, #n);
    VK_INSTANCE(VK_LOADI)
}
static void load_device(VkDevice dv) {
#define VK_LOADD(n) n = (PFN_##n)vkGetDeviceProcAddr(dv, #n);
    VK_DEVICE(VK_LOADD)
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}
/* a device fit to draw with: Vulkan 1.3 (dynamic rendering, extended dynamic
 * state), push descriptors, a swapchain, a graphics queue, BC textures */
static int device_fits(VkPhysicalDevice pd, uint32_t *qf, VkSurfaceKHR surf) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(pd, &p);
    if (p.apiVersion < VK_API_VERSION_1_3) return 0;
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties *e = (VkExtensionProperties *)calloc(n ? n : 1, sizeof *e);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    int ok = has_ext(e, n, VK_KHR_SWAPCHAIN_EXTENSION_NAME) && has_ext(e, n, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    free(e);
    if (!ok) return 0;
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13 };
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (!f13.dynamicRendering || !f13.synchronization2 || !f2.features.textureCompressionBC) return 0;
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties *q = (VkQueueFamilyProperties *)calloc(nq ? nq : 1, sizeof *q);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, q);
    int found = -1;
    for (uint32_t i = 0; i < nq && found < 0; i++) {
        if (!(q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 pr = VK_TRUE;
        if (surf) vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surf, &pr);
        if (pr) found = (int)i;
    }
    free(q);
    if (found < 0) return 0;
    *qf = (uint32_t)found;
    return 1;
}
/* the device to use: a discrete GPU first, then an integrated one, then any (lavapipe) */
static VkPhysicalDevice pick_device(VkInstance in, VkSurfaceKHR surf, uint32_t *qf) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(in, &n, NULL);
    VkPhysicalDevice *pd = (VkPhysicalDevice *)calloc(n ? n : 1, sizeof *pd);
    vkEnumeratePhysicalDevices(in, &n, pd);
    VkPhysicalDevice best = VK_NULL_HANDLE;
    int best_rank = -1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t q;
        if (!device_fits(pd[i], &q, surf)) continue;
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        int rank = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 0 : 1;
        if (rank > best_rank) best_rank = rank, best = pd[i], *qf = q;
    }
    free(pd);
    return best;
}
static VkInstance make_instance(const char **ext, uint32_t n) {
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "Operation Flashpoint",
                              .pEngineName = "pcrecomp d3d8", .apiVersion = VK_API_VERSION_1_3 };
    const char *layers[1] = { "VK_LAYER_KHRONOS_validation" };
    int val = getenv("OFP_VK_VALIDATE") != NULL;
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                .enabledExtensionCount = n, .ppEnabledExtensionNames = ext,
                                .enabledLayerCount = val ? 1u : 0u, .ppEnabledLayerNames = layers };
    VkInstance in = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, NULL, &in) != VK_SUCCESS && val) {
        ci.enabledLayerCount = 0;                      /* no validation layer here */
        if (vkCreateInstance(&ci, NULL, &in) != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    return in;
}
/* before the window: is there a Vulkan 1.3 device to draw with? */
int d3d8_vk_available(void) {
    if (!load_global()) return 0;
    uint32_t ver = 0;
    if (vkEnumerateInstanceVersion && vkEnumerateInstanceVersion(&ver) == VK_SUCCESS && ver < VK_API_VERSION_1_3) return 0;
    VkInstance in = make_instance(NULL, 0);
    if (!in) return 0;
    load_instance(in);
    uint32_t qf;
    VkPhysicalDevice pd = pick_device(in, VK_NULL_HANDLE, &qf);
    if (pd) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd, &p);
        perf_note("Vulkan: %s, Vulkan %u.%u.%u", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
                  VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
    }
    vkDestroyInstance(in, NULL);
    return pd != VK_NULL_HANDLE;
}

/* ------------------------------------------------------------------ the device */
#define NFRAMES 2
#define ARENA_CHUNK (16u << 20)
typedef struct chunk_s { VkBuffer buf; VmaAllocation mem; uint8_t *ptr; VkDeviceSize size, used; struct chunk_s *next; } chunk_t;
typedef enum { DEAD_IMAGE, DEAD_BUFFER } dead_kind_t;
typedef struct { dead_kind_t kind; void *p; } dead_t;
typedef struct {
    VkCommandBuffer cmd, up;
    VkFence fence;
    VkSemaphore acquired;
    chunk_t *chunks;                   /* the arena: chunks[0] in use, the rest after */
    uint64_t serial;                   /* the serial this slot was begun with */
    dead_t *dead; int ndead, maxdead;  /* destroyed when this frame is done */
    int up_open;                       /* the upload command buffer has commands */
} frame_t;
/* an image: a texture, a render target, a depth surface, the back buffer */
typedef struct {
    VkImage img; VmaAllocation mem; VkImageView view;
    VkFormat fmt; int w, h, levels, samples;
    VkImageAspectFlags aspect;
    VkImageLayout layout;
    uint64_t used;                     /* the frame serial it was last drawn with or into */
} vkimg_t;
/* a buffer's copy: a static buffer's in device memory; a dynamic buffer's
 * mapped (ptr), with its earlier copies the GPU may still read (spare) */
typedef struct vkbuf { VkBuffer buf; VmaAllocation mem; VkDeviceSize size; uint64_t used; uint8_t *ptr; struct vkbuf *spare; } vkbuf_t;

typedef struct {                       /* a pipeline's identity */
    uint32_t fvf;
    uint8_t tci[4], topo, blend, src, dst, op, cw, fill, samples, depth, pad[3];
} pkey_t;
typedef struct { pkey_t k; VkPipeline p; } pent_t;
#define NPIPE 4096

/* the uniform blocks, as the shaders lay them out (std140) */
typedef struct { float wv[16], proj[16], nrm[16], screen[4]; int32_t vf[4], msrc[4], vf2[4]; } ub_xf_t;
typedef struct { float diffuse[4], specular[4], ambient[4], pos[4], dir[4], att[4], spot[4]; } ub_light_t;
typedef struct { float mdiff[4], mamb[4], mspec[4], memis[4], mpow[4], gamb[4]; int32_t nl[4]; ub_light_t L[8]; } ub_lt_t;
typedef struct { int32_t cop[4][4], aop[4][4]; float tfactor[4], fogcol[4], fogp[4]; int32_t pf[4]; float aref[4]; } ub_px_t;

typedef struct {
    VkInstance inst;
    VkPhysicalDevice pd;
    VkPhysicalDeviceProperties props;
    VkDevice dev;
    VkQueue queue;
    uint32_t qf;
    VmaAllocator vma;
    VkSurfaceKHR surf;
    VkSwapchainKHR swap;
    VkFormat swap_fmt;
    VkImage swap_img[16]; uint32_t nswap; int swap_w, swap_h;
    VkSemaphore swap_done[16];
    int vsync, want_vsync, swap_dirty;
    VkCommandPool pool;
    frame_t fr[NFRAMES];
    int cur;                           /* the frame slot being recorded */
    uint64_t serial;                   /* counts submissions: what "drawn with this frame" compares to */
    uint64_t done;                     /* the serial the GPU has finished (and all before) */
    VkFormat depth_fmt;
    int fill_nonsolid;
    float max_aniso;
    /* the back buffer: back (single-sampled, what is shown), ms (multisampled, drawn into when antialiased) */
    vkimg_t *back, *back_ms, *depth, *depth_ms;
    /* drawing */
    int rendering;
    vkimg_t *rcol, *rdep;              /* the attachments of the rendering under way */
    VkDescriptorSetLayout dsl, odsl;
    VkPipelineLayout pl, opl;
    VkShaderModule vs, fs, ovs, ofs;
    VkPipelineCache pcache;
    pent_t *pipes;
    VkPipeline opipe;                  /* the overlay's */
    VkPipeline bound;
    vkimg_t *white;
    VkBuffer defaults; VmaAllocation defaults_mem;
    struct { uint32_t key; VkSampler s; } samp[64]; int nsamp;
    VkSampler lin;                     /* the overlay's */
    /* what the command buffer was last given */
    struct {
        int valid;
        VkBuffer ubuf[3]; VkDeviceSize uoff[3];
        VkImageView view[4]; VkSampler samp[4];
        VkCullModeFlags cull; int depth_test, depth_write, stencil, bias_on; VkCompareOp zfunc;
        VkPrimitiveTopology topo;
        uint32_t sref, smask, swmask; VkStencilOp sop[3]; VkCompareOp sfunc;
        float bias;
        VkViewport vp; VkRect2D sc;
    } cs;
    /* the uniform blocks last written: the bytes, where they went */
    ub_xf_t xf; ub_lt_t lt; ub_px_t px;
    VkBuffer ubuf[3]; VkDeviceSize uoff[3]; int uvalid[3];
    vkimg_t *ov[2]; unsigned ov_ver[2];
} vkr_t;
static vkr_t V;

static frame_t *F(void) { return &V.fr[V.cur]; }
static int ok(VkResult r, const char *what) {
    if (r == VK_SUCCESS) return 1;
    fprintf(stderr, "[vulkan] %s: error %d\n", what, (int)r);
    perf_note("Vulkan: %s failed (%d)", what, (int)r);
    return 0;
}

/* ------------------------------------------------------------------ memory */
static chunk_t *chunk_new(VkDeviceSize size) {
    chunk_t *c = (chunk_t *)calloc(1, sizeof *c);
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
               | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VmaAllocationCreateInfo ai = { .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
                                   .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST };
    VmaAllocationInfo info;
    if (!ok(vmaCreateBuffer(V.vma, &bi, &ai, &c->buf, &c->mem, &info), "arena")) { free(c); return NULL; }
    c->ptr = (uint8_t *)info.pMappedData;
    c->size = size;
    return c;
}
/* frame memory: size bytes aligned; the buffer and offset, and where to write */
static uint8_t *arena(VkDeviceSize size, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off) {
    frame_t *f = F();
    chunk_t *c = f->chunks;
    VkDeviceSize o = c ? (c->used + align - 1) / align * align : 0;
    if (!c || o + size > c->size) {
        /* a chunk with room: a spare one from earlier frames, or a new one */
        chunk_t **pp = &f->chunks, *got = NULL;
        if (c) for (pp = &c->next; *pp; pp = &(*pp)->next) if ((*pp)->size >= size && (*pp)->used == 0) { got = *pp; *pp = got->next; break; }
        if (!got) got = chunk_new(size > ARENA_CHUNK ? size : ARENA_CHUNK);
        if (!got) return NULL;
        got->used = 0;
        got->next = f->chunks;                         /* in use goes first; the old first is full */
        f->chunks = got;
        c = got, o = 0;
    }
    c->used = o + size;
    *buf = c->buf, *off = o;
    return c->ptr + o;
}
static void dead(dead_kind_t k, void *p) {
    frame_t *f = F();
    if (f->ndead == f->maxdead) f->maxdead = f->maxdead ? 2 * f->maxdead : 64, f->dead = (dead_t *)realloc(f->dead, (size_t)f->maxdead * sizeof *f->dead);
    f->dead[f->ndead].kind = k, f->dead[f->ndead++].p = p;
}
static void img_destroy_now(vkimg_t *m) {
    if (!m) return;
    if (m->view) vkDestroyImageView(V.dev, m->view, NULL);
    if (m->img) vmaDestroyImage(V.vma, m->img, m->mem);
    free(m);
}
static void img_free(vkimg_t *m) { if (m) dead(DEAD_IMAGE, m); }
static void buf_destroy_now(vkbuf_t *b) { if (b) vmaDestroyBuffer(V.vma, b->buf, b->mem), free(b); }

/* ------------------------------------------------------------------ command buffers and barriers */
static VkCommandBuffer up_cmd(void) {                  /* the frame's upload commands, opened on first use */
    frame_t *f = F();
    if (!f->up_open) {
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkBeginCommandBuffer(f->up, &bi);
        /* the frames before have finished reading what this one overwrites */
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        vkCmdPipelineBarrier(f->up, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        f->up_open = 1;
    }
    return f->up;
}
static void layout(VkCommandBuffer cb, vkimg_t *m, VkImageLayout to) {
    if (m->layout == to) return;
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = m->layout, .newLayout = to,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = m->img,
        .subresourceRange = { m->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    m->layout = to;
}
static void begin_frame(void) {
    frame_t *f = F();
    vkWaitForFences(V.dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
    if (f->serial > V.done) V.done = f->serial;
    for (int i = 0; i < f->ndead; i++)
        if (f->dead[i].kind == DEAD_IMAGE) img_destroy_now((vkimg_t *)f->dead[i].p);
        else buf_destroy_now((vkbuf_t *)f->dead[i].p);
    f->ndead = 0;
    for (chunk_t *c = f->chunks; c; c = c->next) c->used = 0;
    vkResetCommandBuffer(f->cmd, 0);
    vkResetCommandBuffer(f->up, 0);
    f->up_open = 0;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(f->cmd, &bi);
    f->serial = ++V.serial;
    V.cs.valid = 0, V.bound = VK_NULL_HANDLE;
    V.uvalid[0] = V.uvalid[1] = V.uvalid[2] = 0;       /* the blocks' memory went with the arena */
}
static void end_rendering(void) {
    if (V.rendering) vkCmdEndRendering(F()->cmd), V.rendering = 0;
}
/* the frame's commands to the queue (the uploads first); wait and acquired
 * semaphores when presenting */
static void submit(VkSemaphore wait, VkSemaphore signal) {
    frame_t *f = F();
    end_rendering();
    VkCommandBuffer cbs[2];
    uint32_t n = 0;
    if (f->up_open) {
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT };
        vkCmdPipelineBarrier(f->up, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        vkEndCommandBuffer(f->up);
        cbs[n++] = f->up;
    }
    vkEndCommandBuffer(f->cmd);
    cbs[n++] = f->cmd;
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = n, .pCommandBuffers = cbs,
                        .waitSemaphoreCount = wait ? 1u : 0u, .pWaitSemaphores = &wait, .pWaitDstStageMask = &ws,
                        .signalSemaphoreCount = signal ? 1u : 0u, .pSignalSemaphores = &signal };
    vkResetFences(V.dev, 1, &f->fence);
    ok(vkQueueSubmit(V.queue, 1, &si, f->fence), "submit");
}

/* ------------------------------------------------------------------ images */
typedef struct { VkFormat f; VkComponentMapping sw; int convert24; } vkfmt_t;
static vkfmt_t vk_fmt(uint32_t f) {
    const VkComponentSwizzle I = VK_COMPONENT_SWIZZLE_IDENTITY, R = VK_COMPONENT_SWIZZLE_R, G = VK_COMPONENT_SWIZZLE_G,
                             B = VK_COMPONENT_SWIZZLE_B, A = VK_COMPONENT_SWIZZLE_A, ONE = VK_COMPONENT_SWIZZLE_ONE,
                             ZERO = VK_COMPONENT_SWIZZLE_ZERO;
    vkfmt_t v = { VK_FORMAT_B8G8R8A8_UNORM, { I, I, I, I }, 0 };
    switch (f) {
    case FMT_A8R8G8B8: break;
    case FMT_X8R8G8B8: v.sw.a = ONE; break;
    case FMT_R8G8B8: v.sw.a = ONE, v.convert24 = 1; break;
    case FMT_R5G6B5: v.f = VK_FORMAT_R5G6B5_UNORM_PACK16; break;
    case FMT_X1R5G5B5: v.f = VK_FORMAT_A1R5G5B5_UNORM_PACK16, v.sw.a = ONE; break;
    case FMT_A1R5G5B5: v.f = VK_FORMAT_A1R5G5B5_UNORM_PACK16; break;
    /* B4G4R4A4 holds blue in the top nibble: D3D's A4R4G4B4 read through it is (G R A B) */
    case FMT_A4R4G4B4: v.f = VK_FORMAT_B4G4R4A4_UNORM_PACK16, v.sw = (VkComponentMapping){ G, R, A, B }; break;
    case FMT_X4R4G4B4: v.f = VK_FORMAT_B4G4R4A4_UNORM_PACK16, v.sw = (VkComponentMapping){ G, R, A, ONE }; break;
    case FMT_A8: v.f = VK_FORMAT_R8_UNORM, v.sw = (VkComponentMapping){ ZERO, ZERO, ZERO, R }; break;
    case FMT_L8: v.f = VK_FORMAT_R8_UNORM, v.sw = (VkComponentMapping){ R, R, R, ONE }; break;
    case FMT_A8L8: v.f = VK_FORMAT_R8G8_UNORM, v.sw = (VkComponentMapping){ R, R, R, G }; break;
    case FMT_DXT1: v.f = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; break;
    case FMT_DXT2: case FMT_DXT3: v.f = VK_FORMAT_BC2_UNORM_BLOCK; break;
    case FMT_DXT4: case FMT_DXT5: v.f = VK_FORMAT_BC3_UNORM_BLOCK; break;
    }
    return v;
}
static vkimg_t *img_new(int w, int h, int levels, VkFormat fmt, VkComponentMapping sw, VkImageUsageFlags usage, int samples) {
    vkimg_t *m = (vkimg_t *)calloc(1, sizeof *m);
    int depth = fmt == V.depth_fmt;
    m->fmt = fmt, m->w = w, m->h = h, m->levels = levels, m->samples = samples ? samples : 1;
    m->aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    m->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
        .extent = { (uint32_t)w, (uint32_t)h, 1 }, .mipLevels = (uint32_t)levels, .arrayLayers = 1,
        .samples = (VkSampleCountFlagBits)m->samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VmaAllocationCreateInfo ai = { .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE };
    if (!ok(vmaCreateImage(V.vma, &ci, &ai, &m->img, &m->mem, NULL), "image")) { free(m); return NULL; }
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt, .components = sw,
        .subresourceRange = { depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                              0, (uint32_t)levels, 0, 1 } };
    if (!ok(vkCreateImageView(V.dev, &vi, NULL, &m->view), "image view")) { vmaDestroyImage(V.vma, m->img, m->mem); free(m); return NULL; }
    return m;
}
static const VkComponentMapping SW_ID = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                          VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
static vkimg_t *color_target(int w, int h, int samples) {
    return img_new(w, h, 1, VK_FORMAT_B8G8R8A8_UNORM, SW_ID, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                   | (samples > 1 ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                      : VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT), samples);
}
static vkimg_t *depth_target(int w, int h, int samples) {
    return img_new(w, h, 1, V.depth_fmt, SW_ID, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, samples);
}
/* bytes of a level into the image (in the upload commands) */
static void img_upload(vkimg_t *m, int level, const uint8_t *src, uint32_t size, int w, int h, int convert24) {
    VkBuffer b; VkDeviceSize off;
    uint32_t n = convert24 ? (uint32_t)w * (uint32_t)h * 4 : size;
    uint8_t *p = arena(n, 16, &b, &off);
    if (!p) return;
    if (convert24) for (int i = 0; i < w * h; i++) p[4 * i] = src[3 * i], p[4 * i + 1] = src[3 * i + 1], p[4 * i + 2] = src[3 * i + 2], p[4 * i + 3] = 255;
    else memcpy(p, src, size);
    VkCommandBuffer cb = up_cmd();
    layout(cb, m, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy r = { .bufferOffset = off, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, 0, 1 },
                            .imageExtent = { (uint32_t)w, (uint32_t)h, 1 } };
    vkCmdCopyBufferToImage(cb, b, m->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
}

/* the texture's image, up to date: made at first use; a level changed after
 * this frame drew with it gives a new image */
static vkimg_t *tex_image(tex_t *t) {
    vkimg_t *m = (vkimg_t *)t->vk;
    if (m && !t->dirty) return m;
    if (t->usage & 1) return m;                        /* a render target: drawn into, not uploaded */
    vkfmt_t vf = vk_fmt(t->fmt);
    int all = 0;
    if (!m || m->used == V.serial) {
        vkimg_t *n = img_new(t->w, t->h, t->levels, vf.f, vf.sw, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1);
        if (!n) return m ? m : V.white;
        img_free(m);
        t->vk = m = n, all = 1;
    }
    for (int l = 0; l < t->levels; l++) {
        surf_t *s = t->lvl[l];
        if (!all && !(t->dirty & (1u << l))) continue;
        if (!s || !s->mem) continue;
        img_upload(m, l, s->mem, s->size, s->w, s->h, vf.convert24);
        perf_texture(s->size);
    }
    layout(up_cmd(), m, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    t->dirty = 0;
    return m;
}
/* a buffer copy: device memory (ai PREFER_DEVICE), or mapped (ptr) */
static vkbuf_t *vkbuf_new(VkDeviceSize size, VkBufferUsageFlags usage, const VmaAllocationCreateInfo *ai) {
    vkbuf_t *n = (vkbuf_t *)calloc(1, sizeof *n);
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size ? size : 4,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | usage };
    VmaAllocationInfo info;
    if (!ok(vmaCreateBuffer(V.vma, &bi, ai, &n->buf, &n->mem, &info), "buffer")) { free(n); return NULL; }
    n->ptr = (uint8_t *)info.pMappedData, n->size = bi.size;
    perf_rc[RC_NEWBUF]++;
    return n;
}
/* a buffer copy and the spares after it, freed once the GPU is done */
static void spares_release(vkbuf_t *v) {
    while (v) { vkbuf_t *s = v->spare; v->spare = NULL; dead(DEAD_BUFFER, v); v = s; }
}
/* a static buffer's device copy, up to date */
static vkbuf_t *buf_copy(buf_t *b) {
    vkbuf_t *v = (vkbuf_t *)b->vk;
    if (v && b->dhi <= b->dlo) return v;
    uint32_t lo = b->dlo, hi = b->dhi;
    if (!v || v->used == V.serial) {                   /* new, or drawn with this frame: a new copy, all of it */
        static const VmaAllocationCreateInfo ai = { .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE };
        vkbuf_t *n = vkbuf_new(b->size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &ai);
        if (!n) return v;
        if (v) dead(DEAD_BUFFER, v);
        b->vk = v = n, lo = 0, hi = b->size;
    }
    if (hi > lo) {
        VkBuffer sb; VkDeviceSize so;
        uint8_t *p = arena(hi - lo, 16, &sb, &so);
        if (p) {
            memcpy(p, b->mem + lo, hi - lo);
            perf_rc[RC_COPY] += hi - lo;
            VkBufferCopy r = { so, lo, hi - lo };
            vkCmdCopyBuffer(up_cmd(), sb, v->buf, 1, &r);
        }
    }
    buf_synced(b);
    return v;
}
/* a dynamic buffer's copy, up to date: the changed bytes written into it in
 * place (NOOVERWRITE: the game does not touch what is being drawn), or into a
 * free copy when the GPU may still read this one and the lock was DISCARD
 * (only the new bytes count) or neither flag (all of it) */
#define NSPARE 16
static vkbuf_t *dyn_copy(buf_t *b) {
    vkbuf_t *v = (vkbuf_t *)b->vk;
    if (v && b->dhi <= b->dlo) return v;
    uint32_t lo = b->dlo, hi = b->dhi;
    if (!v || (v->used > V.done && (b->discard || b->sync))) {
        vkbuf_t *n = NULL;
        if (v) for (vkbuf_t **pp = &v->spare; *pp; pp = &(*pp)->spare)
            if ((*pp)->used <= V.done) { n = *pp, *pp = n->spare; break; }
        if (!n) {
            static const VmaAllocationCreateInfo ai = { .usage = VMA_MEMORY_USAGE_AUTO,
                .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT };
            if (!(n = vkbuf_new(b->size, 0, &ai))) return v;
        }
        if (v) {                                       /* the old one goes first of the spares; too many: freed */
            n->spare = v;
            vkbuf_t **pp = &n->spare;
            for (int k = 0; k < NSPARE && *pp; k++) pp = &(*pp)->spare;
            spares_release(*pp), *pp = NULL;
        }
        if (!v || !b->discard) lo = 0, hi = b->size;   /* DISCARD: what was written since is all that counts */
        b->vk = v = n;
    }
    if (hi > lo) {
        memcpy(v->ptr + lo, b->mem + lo, hi - lo);
        vmaFlushAllocation(V.vma, v->mem, lo, hi - lo);
        perf_rc[RC_COPY] += hi - lo;
    }
    buf_synced(b);
    return v;
}
/* a buffer's copy for this frame's draw (D3DUSAGE_DYNAMIC: mapped, else device) */
static vkbuf_t *buf_vk(buf_t *b) {
    vkbuf_t *c = (b->usage & 0x200u) ? dyn_copy(b) : buf_copy(b);
    if (c) c->used = V.serial;
    return c;
}

/* ------------------------------------------------------------------ samplers */
static VkSamplerAddressMode vk_wrap(uint32_t a) {
    switch (a) { case 2: case 5: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; case 3: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                 case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; default: return VK_SAMPLER_ADDRESS_MODE_REPEAT; }
}
static VkSampler sampler(const uint32_t *ts, int levels) {
    uint32_t mag = ts[16], min = ts[17], mip = levels > 1 ? ts[18] : 0, an = (min == 3 || mag == 3) ? (ts[21] ? ts[21] : 1) : 1;
    if (an > 16) an = 16;
    uint32_t key = (mag & 3) | (min & 3) << 2 | (mip & 3) << 4 | (ts[13] & 7) << 6 | (ts[14] & 7) << 9 | an << 12;
    for (int i = 0; i < V.nsamp; i++) if (V.samp[i].key == key) return V.samp[i].s;
    VkSamplerCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = mag == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR, .minFilter = min == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
        .mipmapMode = mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = vk_wrap(ts[13]), .addressModeV = vk_wrap(ts[14]), .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .anisotropyEnable = an > 1 && V.max_aniso > 1, .maxAnisotropy = (float)an < V.max_aniso ? (float)an : V.max_aniso,
        .minLod = 0, .maxLod = mip ? VK_LOD_CLAMP_NONE : 0.25f, .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK };
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(V.dev, &ci, NULL, &s);
    if (V.nsamp < 64) V.samp[V.nsamp].key = key, V.samp[V.nsamp++].s = s;
    return s;
}

/* ------------------------------------------------------------------ pipelines */
static VkShaderModule shader(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code };
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(V.dev, &ci, NULL, &m);
    return m;
}
static VkBlendFactor vk_blend(uint32_t b) {
    switch (b) {
    case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE; case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR; case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA; case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp vk_blendop(uint32_t o) {
    switch (o) { case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
                 case 4: return VK_BLEND_OP_MIN; case 5: return VK_BLEND_OP_MAX; default: return VK_BLEND_OP_ADD; }
}
static VkFormat tc_fmt(int n) {
    return n == 1 ? VK_FORMAT_R32_SFLOAT : n == 3 ? VK_FORMAT_R32G32B32_SFLOAT : n == 4 ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R32G32_SFLOAT;
}
static VkPipeline make_pipeline(const pkey_t *k) {
    fvf_t f;
    fvf_decode(k->fvf, &f);
    VkVertexInputBindingDescription bd[2] = { { 0, 0, VK_VERTEX_INPUT_RATE_VERTEX }, { 1, 0, VK_VERTEX_INPUT_RATE_VERTEX } };
    VkVertexInputAttributeDescription ad[8];
    /* binding 1 is the defaults (stride 0): normal 0, diffuse white, specular 0, texcoord (0 0 0 1) */
    ad[0] = (VkVertexInputAttributeDescription){ 0, 0, f.rhw ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R32G32B32_SFLOAT, 0 };
    ad[1] = f.normal ? (VkVertexInputAttributeDescription){ 1, 0, VK_FORMAT_R32G32B32_SFLOAT, (uint32_t)f.o_normal }
                     : (VkVertexInputAttributeDescription){ 1, 1, VK_FORMAT_R32G32B32_SFLOAT, 0 };
    ad[2] = f.diffuse ? (VkVertexInputAttributeDescription){ 2, 0, VK_FORMAT_B8G8R8A8_UNORM, (uint32_t)f.o_diffuse }
                      : (VkVertexInputAttributeDescription){ 2, 1, VK_FORMAT_B8G8R8A8_UNORM, 16 };
    ad[3] = f.specular ? (VkVertexInputAttributeDescription){ 3, 0, VK_FORMAT_B8G8R8A8_UNORM, (uint32_t)f.o_specular }
                       : (VkVertexInputAttributeDescription){ 3, 1, VK_FORMAT_B8G8R8A8_UNORM, 20 };
    for (int i = 0; i < 4; i++) {
        int c = k->tci[i];
        ad[4 + i] = c < f.ntex ? (VkVertexInputAttributeDescription){ (uint32_t)(4 + i), 0, tc_fmt(f.tex_n[c]), (uint32_t)f.o_tex[c] }
                               : (VkVertexInputAttributeDescription){ (uint32_t)(4 + i), 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32 };
    }
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 2, .pVertexBindingDescriptions = bd, .vertexAttributeDescriptionCount = 8,
        .pVertexAttributeDescriptions = ad };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = k->topo == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : k->topo == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                                           : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = k->fill == 1 ? VK_POLYGON_MODE_POINT : k->fill == 2 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = (VkSampleCountFlagBits)(k->samples ? k->samples : 1) };
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba = { .blendEnable = k->blend, .srcColorBlendFactor = vk_blend(k->src),
        .dstColorBlendFactor = vk_blend(k->dst), .colorBlendOp = vk_blendop(k->op), .srcAlphaBlendFactor = vk_blend(k->src),
        .dstAlphaBlendFactor = vk_blend(k->dst), .alphaBlendOp = vk_blendop(k->op), .colorWriteMask = k->cw };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &ba };
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
        VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE };
    VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = sizeof dyn / sizeof dyn[0], .pDynamicStates = dyn };
    VkFormat cf = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &cf, .depthAttachmentFormat = k->depth ? V.depth_fmt : VK_FORMAT_UNDEFINED,
        .stencilAttachmentFormat = k->depth ? V.depth_fmt : VK_FORMAT_UNDEFINED };
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = V.vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = V.fs, .pName = "main" } };
    VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
        .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb, .pDynamicState = &dy, .layout = V.pl };
    VkPipeline p = VK_NULL_HANDLE;
    ok(vkCreateGraphicsPipelines(V.dev, V.pcache, 1, &ci, NULL, &p), "pipeline");
    return p;
}
static VkPipeline pipeline(const pkey_t *k) {
    uint32_t h = 2166136261u;
    const uint8_t *b = (const uint8_t *)k;
    for (size_t i = 0; i < sizeof *k; i++) h = (h ^ b[i]) * 16777619u;
    for (uint32_t i = 0; i < NPIPE; i++) {
        pent_t *e = &V.pipes[(h + i) & (NPIPE - 1)];
        if (!e->p) {                                   /* a new one: timed (a stall the first time it is drawn) */
            uint64_t t0 = hle_mono_ns();
            e->k = *k, e->p = make_pipeline(k);
            perf_rc[RC_NEWPIPE]++, perf_rc[RC_NEWPIPE_NS] += hle_mono_ns() - t0;
            return e->p;
        }
        if (!memcmp(&e->k, k, sizeof *k)) return e->p;
    }
    return make_pipeline(k);                           /* full (it is not): made each time */
}

/* ------------------------------------------------------------------ swapchain */
static VkPresentModeKHR present_mode(void) {
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(V.pd, V.surf, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(V.pd, V.surf, &n, m);
    VkPresentModeKHR want[3];
    int nw = 0;
    if (V.want_vsync < 0) want[nw++] = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    else if (!V.want_vsync) want[nw++] = VK_PRESENT_MODE_IMMEDIATE_KHR, want[nw++] = VK_PRESENT_MODE_MAILBOX_KHR;
    for (int w = 0; w < nw; w++) for (uint32_t i = 0; i < n; i++) if (m[i] == want[w]) return want[w];
    return VK_PRESENT_MODE_FIFO_KHR;
}
static int make_swapchain(void) {
    SDL_Window *w = hle_screen_sdl_window();
    int ww = 0, wh = 0;
    SDL_Vulkan_GetDrawableSize(w, &ww, &wh);
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.pd, V.surf, &caps);
    if (caps.currentExtent.width != 0xFFFFFFFFu) ww = (int)caps.currentExtent.width, wh = (int)caps.currentExtent.height;
    if (ww <= 0 || wh <= 0) return 0;                  /* minimised */
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(V.pd, V.surf, &nf, NULL);
    VkSurfaceFormatKHR fm[32];
    if (nf > 32) nf = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(V.pd, V.surf, &nf, fm);
    V.swap_fmt = fm[0].format;
    VkColorSpaceKHR cs = fm[0].colorSpace;
    for (uint32_t i = 0; i < nf; i++)
        if (fm[i].format == VK_FORMAT_B8G8R8A8_UNORM || fm[i].format == VK_FORMAT_R8G8B8A8_UNORM) { V.swap_fmt = fm[i].format, cs = fm[i].colorSpace; break; }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkSwapchainKHR old = V.swap;
    VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = V.surf, .minImageCount = count,
        .imageFormat = V.swap_fmt, .imageColorSpace = cs, .imageExtent = { (uint32_t)ww, (uint32_t)wh }, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform = caps.currentTransform, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = present_mode(), .clipped = VK_TRUE, .oldSwapchain = old };
    vkDeviceWaitIdle(V.dev);
    if (!ok(vkCreateSwapchainKHR(V.dev, &ci, NULL, &V.swap), "swapchain")) return 0;
    if (old) vkDestroySwapchainKHR(V.dev, old, NULL);
    V.nswap = 16;
    vkGetSwapchainImagesKHR(V.dev, V.swap, &V.nswap, V.swap_img);
    for (uint32_t i = 0; i < 16; i++) {
        if (V.swap_done[i]) vkDestroySemaphore(V.dev, V.swap_done[i], NULL), V.swap_done[i] = VK_NULL_HANDLE;
        if (i < V.nswap) {
            VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            vkCreateSemaphore(V.dev, &si, NULL, &V.swap_done[i]);
        }
    }
    V.swap_w = ww, V.swap_h = wh, V.vsync = V.want_vsync, V.swap_dirty = 0;
    perf_note("Vulkan swapchain: %dx%d, %u images, present mode %d", ww, wh, V.nswap, (int)ci.presentMode);
    return 1;
}

/* ------------------------------------------------------------------ rendering */
static void back_images(d3ddev_t *d) {
    if (V.dev) vkDeviceWaitIdle(V.dev);
    end_rendering();
    vkimg_t **m[4] = { &V.back, &V.back_ms, &V.depth, &V.depth_ms };
    for (int i = 0; i < 4; i++) if (*m[i]) img_free(*m[i]), *m[i] = NULL;
    int s = d->msaa;
    VkSampleCountFlags have = V.props.limits.framebufferColorSampleCounts & V.props.limits.framebufferDepthSampleCounts;
    while (s > 1 && !(have & (VkSampleCountFlags)s)) s /= 2;
    d->msaa = s > 1 ? s : 0;
    V.back = color_target(d->bw, d->bh, 1);
    V.depth = depth_target(d->bw, d->bh, 1);
    if (d->msaa) V.back_ms = color_target(d->bw, d->bh, d->msaa), V.depth_ms = depth_target(d->bw, d->bh, d->msaa);
    perf_note("antialiasing: %s%d samples (Vulkan)", d->msaa ? "" : "off, ", d->msaa);
    if (d->back) d->back->vk = V.back;
    if (d->depth) d->depth->vk = V.depth;
    V.rcol = V.rdep = NULL;
}
/* the attachments: the render target, else the back buffer (multisampled when
 * antialiased: then its own depth, as a game depth surface is single-sampled) */
static void targets_now(d3ddev_t *d, vkimg_t **col, vkimg_t **dep) {
    if (d->rt) {
        tex_t *t = d->rt->parent;
        *col = t ? (vkimg_t *)t->vk : (vkimg_t *)d->rt->vk;
        *dep = d->zs ? (vkimg_t *)d->zs->vk : NULL;
        if (*dep && ((*dep)->w < (*col)->w || (*dep)->h < (*col)->h)) *dep = NULL;
    } else if (d->msaa) {
        *col = V.back_ms;
        *dep = d->zs ? V.depth_ms : NULL;
    } else {
        *col = V.back;
        *dep = d->zs ? (vkimg_t *)d->zs->vk : NULL;
        if (*dep && (*dep)->samples != 1) *dep = V.depth;
    }
}
static void begin_rendering(d3ddev_t *d) {
    vkimg_t *col, *dep;
    targets_now(d, &col, &dep);
    if (V.rendering && col == V.rcol && dep == V.rdep) return;
    end_rendering();
    VkCommandBuffer cb = F()->cmd;
    layout(cb, col, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (dep) layout(cb, dep, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = col->view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingAttachmentInfo da = ca;
    if (dep) da.imageView = dep->view, da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { (uint32_t)col->w, (uint32_t)col->h } },
        .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca,
        .pDepthAttachment = dep ? &da : NULL, .pStencilAttachment = dep ? &da : NULL };
    vkCmdBeginRendering(cb, &ri);
    V.rendering = 1, V.rcol = col, V.rdep = dep;
    col->used = V.serial;
    if (dep) dep->used = V.serial;
    V.cs.valid = 0, V.bound = VK_NULL_HANDLE;
}

static VkCompareOp vk_cmp(uint32_t c) {
    static const VkCompareOp m[] = { VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL,
        VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS };
    return c < 9 ? m[c] : VK_COMPARE_OP_ALWAYS;
}
static VkStencilOp vk_sop(uint32_t o) {
    switch (o) { case 2: return VK_STENCIL_OP_ZERO; case 3: return VK_STENCIL_OP_REPLACE; case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
                 case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP; case 6: return VK_STENCIL_OP_INVERT;
                 case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP; case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP; default: return VK_STENCIL_OP_KEEP; }
}
static void mat_mul(float *r, const float *a, const float *b) {     /* r = a * b, D3D row vectors */
    float t[16];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        float s = 0;
        for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
        t[i * 4 + j] = s;
    }
    memcpy(r, t, 64);
}
/* the normals' matrix: the inverse transpose of wv's 3x3 (D3D layout in, the same out) */
static void normal_matrix(float *n, const float *m) {
    float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    float det = a * A + b * B + c * C;
    float s = fabsf(det) > 1e-20f ? 1.0f / det : 0.0f;
    memset(n, 0, 64);
    /* inverse transpose = cofactor / det */
    n[0] = A * s, n[1] = B * s, n[2] = C * s;
    n[4] = -(b * i - c * h) * s, n[5] = (a * i - c * g) * s, n[6] = -(a * h - b * g) * s;
    n[8] = (b * f - c * e) * s, n[9] = -(a * f - c * d) * s, n[10] = (a * e - b * d) * s;
    n[15] = 1;
}
/* a uniform block to the arena when its bytes changed */
static void ublock(int i, const void *data, void *last, size_t size) {
    if (V.uvalid[i] && !memcmp(data, last, size)) return;
    VkBuffer b; VkDeviceSize off;
    uint8_t *p = arena(size, V.props.limits.minUniformBufferOffsetAlignment, &b, &off);
    if (!p) return;
    memcpy(p, data, size);
    memcpy(last, data, size);
    perf_rc[RC_UBO]++;
    V.ubuf[i] = b, V.uoff[i] = off, V.uvalid[i] = 1;
}
static void build_uniforms(d3ddev_t *d, const fvf_t *f, int nstages) {
    const uint32_t *rs = d->s.rs;
    int vw = rt_w(d), vh = rt_h(d);
    int fog = rs[28] != 0;
    uint32_t fmode = rs[35] ? rs[35] : rs[140];
    int fsrc = 0, fpix = 0;
    if (fog) {
        if (!fmode) { if (f->specular) fsrc = 1, fpix = 3; }
        else fsrc = f->rhw ? 3 : 2, fpix = fmode == 1 ? 1 : fmode == 2 ? 2 : 3;
    }
    int lit = !f->rhw && rs[137];
    {
        ub_xf_t x;
        memset(&x, 0, sizeof x);
        if (!f->rhw) {
            mat_mul(x.wv, d->s.world, d->s.view);
            memcpy(x.proj, d->s.proj, 64);
            if (lit) normal_matrix(x.nrm, x.wv);
            int pw = d->s.vp[2] ? (int)d->s.vp[2] : vw, ph = d->s.vp[3] ? (int)d->s.vp[3] : vh;
            x.screen[2] = 1.0f / (float)pw, x.screen[3] = 1.0f / (float)ph;
        } else x.screen[0] = 2.0f / (float)vw, x.screen[1] = 2.0f / (float)vh;
        x.vf[0] = f->rhw, x.vf[1] = lit, x.vf[2] = fsrc, x.vf[3] = rs[143] != 0;
        int cv = rs[141] != 0;
        x.msrc[0] = cv ? (int)rs[145] : 0, x.msrc[1] = cv ? (int)rs[147] : 0, x.msrc[2] = cv ? (int)rs[146] : 0, x.msrc[3] = cv ? (int)rs[148] : 0;
        x.vf2[0] = f->diffuse, x.vf2[1] = f->specular, x.vf2[2] = rs[142] != 0, x.vf2[3] = rs[29] != 0;
        ublock(0, &x, &V.xf, sizeof x);
    }
    if (lit) {
        ub_lt_t l;
        memset(&l, 0, sizeof l);
        memcpy(l.mdiff, d->s.mat, 16), memcpy(l.mamb, d->s.mat + 4, 16), memcpy(l.mspec, d->s.mat + 8, 16), memcpy(l.memis, d->s.mat + 12, 16);
        l.mpow[0] = d->s.mat[16];
        color4(rs[139], l.gamb);
        const float *v = d->s.view;
        int n = 0;
        for (int i = 0; i < NLIGHT && n < 8; i++) {
            if (!d->s.light_on[i]) continue;
            const light_t *s = &d->s.light[i];
            ub_light_t *o = &l.L[n++];
            memcpy(o->diffuse, s->diffuse, 16), memcpy(o->specular, s->specular, 16), memcpy(o->ambient, s->ambient, 16);
            /* to view space: points by the view matrix, directions by its 3x3 */
            for (int k = 0; k < 3; k++) {
                o->pos[k] = s->pos[0] * v[k] + s->pos[1] * v[4 + k] + s->pos[2] * v[8 + k] + v[12 + k];
                o->dir[k] = s->dir[0] * v[k] + s->dir[1] * v[4 + k] + s->dir[2] * v[8 + k];
            }
            o->pos[3] = (float)s->type;
            o->att[0] = s->att0, o->att[1] = s->att1, o->att[2] = s->att2, o->att[3] = s->range > 0 ? s->range : 1e30f;
            o->spot[0] = cosf(s->theta * 0.5f), o->spot[1] = cosf(s->phi * 0.5f), o->spot[2] = s->falloff > 0 ? s->falloff : 1.0f;
        }
        l.nl[0] = n;
        ublock(1, &l, &V.lt, sizeof l);
    } else if (!V.uvalid[1]) {
        ub_lt_t l;
        memset(&l, 0, sizeof l);
        ublock(1, &l, &V.lt, sizeof l);
    }
    {
        ub_px_t p;
        memset(&p, 0, sizeof p);
        for (int i = 0; i < nstages; i++) {
            const uint32_t *ts = d->s.tss[i];
            p.cop[i][0] = (int)ts[1], p.cop[i][1] = (int)ts[2], p.cop[i][2] = (int)ts[3], p.cop[i][3] = (int)ts[26];
            p.aop[i][0] = (int)ts[4], p.aop[i][1] = (int)ts[5], p.aop[i][2] = (int)ts[6], p.aop[i][3] = (int)ts[27];
        }
        color4(rs[60], p.tfactor);
        color4(rs[34], p.fogcol);
        if (fsrc == 1) p.fogp[0] = 0, p.fogp[1] = 1;
        else p.fogp[0] = fl(rs[36]), p.fogp[1] = fl(rs[37]), p.fogp[2] = fl(rs[38]);
        p.pf[0] = nstages, p.pf[1] = rs[15] ? (int)rs[25] : 0, p.pf[2] = rs[29] && f->specular && !lit ? 1 : rs[29] && lit, p.pf[3] = fpix;
        p.aref[0] = (float)(rs[24] & 255) / 255.0f;
        ublock(2, &p, &V.px, sizeof p);
    }
}
/* stream vertices or indices of this draw into the arena */
static int stream(const void *src, VkDeviceSize n, VkBuffer *b, VkDeviceSize *off) {
    uint8_t *p = arena(n ? n : 4, 16, b, off);
    if (!p) return 0;
    memcpy(p, src, n);
    perf_rc[RC_STREAM] += n;
    return 1;
}

static void vk_draw(d3ddev_t *d, const draw_t *dr) {
    fvf_t f;
    fvf_decode(dr->fvf, &f);
    const uint32_t *rs = d->s.rs;
    /* the stages on: up to the first disabled colour op, four at most */
    int nst = 0;
    while (nst < 4 && d->s.tss[nst][1] != 1) nst++;
    vkimg_t *img[4]; VkSampler smp[4];
    for (int i = 0; i < 4; i++) {
        tex_t *t = i < nst && d->s.tex[i] ? (tex_t *)HOST(d->s.tex[i]) : NULL;
        vkimg_t *m = t && !t->cube ? tex_image(t) : NULL;
        if (!m || m->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            /* a render target drawn into earlier: readable now */
            if (m && m->layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && m != V.rcol) end_rendering(), layout(F()->cmd, m, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            else if (m && m->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) m = NULL;
        }
        img[i] = m ? m : V.white;
        smp[i] = sampler(d->s.tss[i], m ? m->levels : 1);
        img[i]->used = V.serial;
    }
    begin_rendering(d);
    VkCommandBuffer cb = F()->cmd;

    /* vertices: a static buffer's device copy, else streamed */
    VkBuffer vb; VkDeviceSize voff;
    int32_t vbase = 0;
    int vfirst = dr->first, vn = dr->nv;
    if (dr->idx) vfirst = dr->rmin, vn = dr->rmax - dr->rmin + 1;
    if (dr->vb) {
        vkbuf_t *c = buf_vk(dr->vb);
        if (!c) return;
        vb = c->buf, voff = (VkDeviceSize)dr->vertex0 * (VkDeviceSize)dr->stride;
        if (dr->idx) vbase = 0; else vbase = vfirst;
    } else {
        if (!stream(dr->base + (size_t)vfirst * (size_t)dr->stride, (VkDeviceSize)vn * (VkDeviceSize)dr->stride, &vb, &voff)) return;
        vbase = -vfirst;                               /* index i is streamed vertex i - vfirst */
        if (!dr->idx) vbase = 0;
    }
    VkBuffer ib = VK_NULL_HANDLE; VkDeviceSize ioff = 0;
    if (dr->idx) {
        VkDeviceSize isz = dr->is32 ? 4 : 2;
        if (dr->ib) {
            vkbuf_t *c = buf_vk(dr->ib);
            if (!c) return;
            ib = c->buf, ioff = dr->ioff;
        } else if (!stream(dr->idx, (VkDeviceSize)dr->count * isz, &ib, &ioff)) return;
    }

    /* the pipeline */
    pkey_t k;
    memset(&k, 0, sizeof k);
    k.fvf = dr->fvf & ~1u;
    for (int i = 0; i < 4; i++) k.tci[i] = (uint8_t)(i < nst ? (d->s.tss[i][11] & 7) : 7);
    k.topo = dr->prim == 1 ? 0 : dr->prim <= 3 ? 1 : 2;
    if (rs[27]) {
        uint32_t sb = rs[19], db = rs[20];
        if (sb == 12) sb = 5, db = 6;
        if (sb == 13) sb = 6, db = 5;
        k.blend = 1, k.src = (uint8_t)sb, k.dst = (uint8_t)db, k.op = (uint8_t)rs[171];
    }
    k.cw = (uint8_t)(rs[168] & 0xF);
    k.fill = rs[8] == 1 ? 1 : rs[8] == 2 ? 2 : 0;
    if (!V.fill_nonsolid) k.fill = 0;
    k.samples = (uint8_t)V.rcol->samples;
    k.depth = V.rdep != NULL;
    VkPipeline p = pipeline(&k);
    if (!p) return;
    if (p != V.bound) vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p), V.bound = p, perf_rc[RC_PIPE]++;

    /* dynamic state, set when it changes */
    int first = !V.cs.valid;
    VkViewport vp; VkRect2D sc;
    if (f.rhw) vp = (VkViewport){ 0, 0, (float)V.rcol->w, (float)V.rcol->h, 0, 1 };
    else vp = (VkViewport){ (float)d->s.vp[0], (float)d->s.vp[1], (float)d->s.vp[2], (float)d->s.vp[3], d->s.vpz[0], d->s.vpz[1] };
    if (vp.width <= 0 || vp.height <= 0) vp.width = (float)V.rcol->w, vp.height = (float)V.rcol->h;
    sc = (VkRect2D){ { (int32_t)d->s.vp[0], (int32_t)d->s.vp[1] }, { d->s.vp[2], d->s.vp[3] } };
    if (sc.offset.x < 0) sc.offset.x = 0;
    if (sc.offset.y < 0) sc.offset.y = 0;
    if (!sc.extent.width || sc.offset.x + (int)sc.extent.width > V.rcol->w) sc.extent.width = (uint32_t)(V.rcol->w - sc.offset.x);
    if (!sc.extent.height || sc.offset.y + (int)sc.extent.height > V.rcol->h) sc.extent.height = (uint32_t)(V.rcol->h - sc.offset.y);
    if (first || memcmp(&vp, &V.cs.vp, sizeof vp)) vkCmdSetViewport(cb, 0, 1, &vp), V.cs.vp = vp;
    if (first || memcmp(&sc, &V.cs.sc, sizeof sc)) vkCmdSetScissor(cb, 0, 1, &sc), V.cs.sc = sc;
    VkCullModeFlags cull = rs[22] == 1 ? VK_CULL_MODE_NONE : rs[22] == 3 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_BIT;
    if (first || cull != V.cs.cull) vkCmdSetCullMode(cb, cull), V.cs.cull = cull;
    if (first) vkCmdSetFrontFace(cb, VK_FRONT_FACE_CLOCKWISE), vkCmdSetDepthBoundsTestEnable(cb, VK_FALSE);
    static const VkPrimitiveTopology topo[] = { VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN };
    VkPrimitiveTopology tp = topo[dr->prim < 7 ? dr->prim : 0];
    if (first || tp != V.cs.topo) vkCmdSetPrimitiveTopology(cb, tp), V.cs.topo = tp;
    int dt = V.rdep && rs[7], dw = V.rdep && rs[14];
    VkCompareOp zf = vk_cmp(rs[23]);
    if (first || dt != V.cs.depth_test) vkCmdSetDepthTestEnable(cb, (VkBool32)dt), V.cs.depth_test = dt;
    if (first || dw != V.cs.depth_write) vkCmdSetDepthWriteEnable(cb, (VkBool32)dw), V.cs.depth_write = dw;
    if (first || zf != V.cs.zfunc) vkCmdSetDepthCompareOp(cb, zf), V.cs.zfunc = zf;
    int st = V.rdep && rs[52];
    if (first || st != V.cs.stencil) vkCmdSetStencilTestEnable(cb, (VkBool32)st), V.cs.stencil = st;
    if (st) {
        VkStencilOp so[3] = { vk_sop(rs[53]), vk_sop(rs[54]), vk_sop(rs[55]) };
        VkCompareOp sf = vk_cmp(rs[56]);
        if (first || memcmp(so, V.cs.sop, sizeof so) || sf != V.cs.sfunc)
            vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, so[0], so[2], so[1], sf), memcpy(V.cs.sop, so, sizeof so), V.cs.sfunc = sf;
        if (first || rs[57] != V.cs.sref) vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, rs[57]), V.cs.sref = rs[57];
        if (first || rs[58] != V.cs.smask) vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, rs[58]), V.cs.smask = rs[58];
        if (first || rs[59] != V.cs.swmask) vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, rs[59]), V.cs.swmask = rs[59];
    } else if (first) {
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
        vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, ~0u);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, ~0u);
        V.cs.sref = 0, V.cs.smask = V.cs.swmask = ~0u, V.cs.sfunc = VK_COMPARE_OP_ALWAYS;
        V.cs.sop[0] = V.cs.sop[1] = V.cs.sop[2] = VK_STENCIL_OP_KEEP;
    }
    int bias_on = rs[47] != 0;
    float bias = -(float)rs[47];
    if (first || bias_on != V.cs.bias_on) vkCmdSetDepthBiasEnable(cb, (VkBool32)bias_on), V.cs.bias_on = bias_on;
    if (first || bias != V.cs.bias) vkCmdSetDepthBias(cb, bias, 0.0f, bias_on ? -1.0f : 0.0f), V.cs.bias = bias;

    /* uniform blocks and textures */
    build_uniforms(d, &f, nst);
    int push = first;
    for (int i = 0; i < 3; i++) if (V.ubuf[i] != V.cs.ubuf[i] || V.uoff[i] != V.cs.uoff[i]) push = 1;
    for (int i = 0; i < 4; i++) if (img[i]->view != V.cs.view[i] || smp[i] != V.cs.samp[i]) push = 1;
    if (push) {
        VkDescriptorBufferInfo bi[3];
        VkDescriptorImageInfo ii[4];
        VkWriteDescriptorSet w[7];
        for (int i = 0; i < 3; i++) {
            bi[i] = (VkDescriptorBufferInfo){ V.ubuf[i], V.uoff[i], i == 0 ? sizeof(ub_xf_t) : i == 1 ? sizeof(ub_lt_t) : sizeof(ub_px_t) };
            w[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i, .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[i] };
            V.cs.ubuf[i] = V.ubuf[i], V.cs.uoff[i] = V.uoff[i];
        }
        for (int i = 0; i < 4; i++) {
            ii[i] = (VkDescriptorImageInfo){ smp[i], img[i]->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            w[3 + i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)(3 + i),
                .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii[i] };
            V.cs.view[i] = img[i]->view, V.cs.samp[i] = smp[i];
        }
        vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.pl, 0, 7, w);
        perf_rc[RC_PUSH]++;
    }
    V.cs.valid = 1;

    VkBuffer vbs[2] = { vb, V.defaults };
    VkDeviceSize vofs[2] = { voff, 0 }, strides[2] = { (VkDeviceSize)dr->stride, 0 };
    vkCmdBindVertexBuffers2(cb, 0, 2, vbs, vofs, NULL, strides);
    if (dr->idx) {
        vkCmdBindIndexBuffer(cb, ib, ioff, dr->is32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cb, (uint32_t)dr->count, 1, 0, vbase, 0);
    } else vkCmdDraw(cb, (uint32_t)dr->count, 1, (uint32_t)vbase, 0);
}

static void vk_clear(d3ddev_t *d, uint32_t n, const int32_t *r, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    begin_rendering(d);
    VkClearAttachment a[2];
    uint32_t na = 0;
    if (flags & 1) {
        a[na] = (VkClearAttachment){ VK_IMAGE_ASPECT_COLOR_BIT, 0, { .color = { .float32 = { 0 } } } };
        color4(color, a[na].clearValue.color.float32);
        na++;
    }
    VkImageAspectFlags za = ((flags & 2) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((flags & 4) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    if (za && V.rdep) a[na++] = (VkClearAttachment){ za, 0, { .depthStencil = { z, stencil } } };
    if (!na) return;
    VkClearRect cr[16];
    uint32_t nr = 0;
    int W = V.rcol->w, H = V.rcol->h;
    for (uint32_t i = 0; i < (n ? n : 1) && nr < 16; i++) {
        int x1, y1, x2, y2;
        if (n) x1 = r[4 * i], y1 = r[4 * i + 1], x2 = r[4 * i + 2], y2 = r[4 * i + 3];
        else x1 = (int)d->s.vp[0], y1 = (int)d->s.vp[1], x2 = x1 + (int)d->s.vp[2], y2 = y1 + (int)d->s.vp[3];
        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 > W) x2 = W;
        if (y2 > H) y2 = H;
        if (x2 <= x1 || y2 <= y1) continue;
        cr[nr++] = (VkClearRect){ { { x1, y1 }, { (uint32_t)(x2 - x1), (uint32_t)(y2 - y1) } }, 0, 1 };
    }
    if (nr) vkCmdClearAttachments(F()->cmd, na, a, nr, cr);
}

/* ------------------------------------------------------------------ present */
/* the overlay's pictures into the back buffer (after the resolve) */
static void draw_overlay(d3ddev_t *d) {
    overlay_layer_t ly[2];
    int n = overlay_layers(d->bw, d->bh, ly);
    if (!n) return;
    for (int i = 0; i < n; i++) {
        int id = ly[i].id;
        vkimg_t *m = V.ov[id];
        if (!m || m->w != ly[i].w || m->h != ly[i].h || V.ov_ver[id] != ly[i].ver) {
            if (!m || m->w != ly[i].w || m->h != ly[i].h || m->used == V.serial) {
                img_free(m);
                m = V.ov[id] = img_new(ly[i].w, ly[i].h, 1, VK_FORMAT_R8G8B8A8_UNORM, SW_ID,
                                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1);
                if (!m) return;
            }
            uint8_t *tight = (uint8_t *)malloc((size_t)ly[i].w * ly[i].h * 4);
            for (int y = 0; y < ly[i].h; y++) memcpy(tight + (size_t)y * ly[i].w * 4, (const uint8_t *)ly[i].rgba + (size_t)y * ly[i].pitch, (size_t)ly[i].w * 4);
            img_upload(m, 0, tight, (uint32_t)(ly[i].w * ly[i].h * 4), ly[i].w, ly[i].h, 0);
            free(tight);
            layout(up_cmd(), m, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            V.ov_ver[id] = ly[i].ver;
        }
    }
    VkCommandBuffer cb = F()->cmd;
    layout(cb, V.back, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = V.back->view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { (uint32_t)d->bw, (uint32_t)d->bh } },
        .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca };
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.opipe);
    VkViewport vp = { 0, 0, (float)d->bw, (float)d->bh, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { (uint32_t)d->bw, (uint32_t)d->bh } };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    for (int i = 0; i < n; i++) {
        vkimg_t *m = V.ov[ly[i].id];
        m->used = V.serial;
        VkDescriptorImageInfo ii = { V.lin, m->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii };
        vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.opl, 0, 1, &w);
        float q[4] = { 2.0f * ly[i].x / d->bw - 1, 2.0f * ly[i].y / d->bh - 1,
                       2.0f * (ly[i].x + ly[i].w) / d->bw - 1, 2.0f * (ly[i].y + ly[i].h) / d->bh - 1 };
        vkCmdPushConstants(cb, V.opl, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof q, q);
        vkCmdDraw(cb, 4, 1, 0, 0);
    }
    vkCmdEndRendering(cb);
    V.cs.valid = 0, V.bound = VK_NULL_HANDLE;
}
/* the shown picture (V.back) into host memory, BGRA rows top first: the
 * frame so far runs to the end first */
static uint8_t *read_back_image(d3ddev_t *d, vkimg_t *src) {
    VkCommandBuffer cb = F()->cmd;
    end_rendering();
    VkBuffer b; VkDeviceSize off;
    size_t n = (size_t)src->w * src->h * 4;
    uint8_t *p = arena(n, 16, &b, &off);
    if (!p) return NULL;
    VkImageLayout was = src->layout;
    layout(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy r = { .bufferOffset = off, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                            .imageExtent = { (uint32_t)src->w, (uint32_t)src->h, 1 } };
    vkCmdCopyImageToBuffer(cb, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &r);
    if (was != VK_IMAGE_LAYOUT_UNDEFINED) layout(cb, src, was);
    submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    vkDeviceWaitIdle(V.dev);                           /* the other frame too: begin_frame frees what this one let go */
    uint8_t *out = (uint8_t *)malloc(n);
    if (out) memcpy(out, p, n);
    begin_frame();
    (void)d;
    return out;
}
static void resolve_back(d3ddev_t *d) {
    if (!d->msaa) return;
    VkCommandBuffer cb = F()->cmd;
    end_rendering();
    layout(cb, V.back_ms, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    layout(cb, V.back, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageResolve r = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                         .extent = { (uint32_t)d->bw, (uint32_t)d->bh, 1 } };
    vkCmdResolveImage(cb, V.back_ms->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.back->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
}
static void vk_present(d3ddev_t *d) {
    SDL_Window *w = hle_screen_sdl_window();
    end_rendering();
    resolve_back(d);
    draw_overlay(d);
    const char *shot = d3d8_shot_due(d);
    if (shot) {
        uint8_t *px = read_back_image(d, V.back);
        if (px) {
            for (int i = 0; i < d->bw * d->bh; i++) { uint8_t b = px[4 * i], g = px[4 * i + 1], r = px[4 * i + 2]; px[3 * i] = r, px[3 * i + 1] = g, px[3 * i + 2] = b; }
            d3d8_shot_write(shot, d->bw, d->bh, px);
            free(px);
        }
    }
    int ww = 0, wh = 0;
    SDL_Vulkan_GetDrawableSize(w, &ww, &wh);
    if (V.swap_dirty || ww != V.swap_w || wh != V.swap_h || V.want_vsync != V.vsync) {
        /* the frame's commands first: the swapchain is remade with the device idle */
        submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
        if (!make_swapchain()) { begin_frame(); return; }
        begin_frame();
    }
    uint32_t idx = 0;
    frame_t *f = F();
    VkResult r = vkAcquireNextImageKHR(V.dev, V.swap, UINT64_MAX, f->acquired, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { V.swap_dirty = 1; return; }   /* this frame is not shown; recording goes on */
    VkCommandBuffer cb = f->cmd;
    layout(cb, V.back, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = V.swap_img[idx],
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkClearColorValue black = { .float32 = { 0, 0, 0, 1 } };
    VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, V.swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    /* the picture scaled into the window, its aspect kept */
    int sw = V.swap_w, sh = V.swap_h, dw = sw, dh = sh;
    if ((long long)sw * d->bh > (long long)sh * d->bw) dw = (int)((long long)sh * d->bw / d->bh);
    else dh = (int)((long long)sw * d->bh / d->bw);
    int dx = (sw - dw) / 2, dy = (sh - dh) / 2;
    VkImageBlit bl = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .srcOffsets = { { 0, 0, 0 }, { d->bw, d->bh, 1 } },
                       .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstOffsets = { { dx, dy, 0 }, { dx + dw, dy + dh, 1 } } };
    vkCmdBlitImage(cb, V.back->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                   dw == d->bw && dh == d->bh ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, b.dstAccessMask = 0;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    submit(f->acquired, V.swap_done[idx]);
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &V.swap_done[idx],
                            .swapchainCount = 1, .pSwapchains = &V.swap, .pImageIndices = &idx };
    r = vkQueuePresentKHR(V.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) V.swap_dirty = 1;
    hle_screen_gl_picture(d->bw, d->bh, dx, dy, dw, dh);
    V.cur = (V.cur + 1) % NFRAMES;
    begin_frame();
}

/* ------------------------------------------------------------------ the renderer's entry points */
static int vk_create(d3ddev_t *d) {
    SDL_Window *w = hle_screen_sdl_window();
    gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!gipa || !load_global()) { fprintf(stderr, "[vulkan] no loader\n"); return 0; }
    unsigned int ne = 0;
    SDL_Vulkan_GetInstanceExtensions(w, &ne, NULL);
    const char **ext = (const char **)calloc(ne + 1, sizeof *ext);
    SDL_Vulkan_GetInstanceExtensions(w, &ne, ext);
    V.inst = make_instance(ext, ne);
    free(ext);
    if (!V.inst) { fprintf(stderr, "[vulkan] no instance\n"); return 0; }
    load_instance(V.inst);
    if (!SDL_Vulkan_CreateSurface(w, V.inst, &V.surf)) { fprintf(stderr, "[vulkan] surface: %s\n", SDL_GetError()); return 0; }
    V.pd = pick_device(V.inst, V.surf, &V.qf);
    if (!V.pd) { fprintf(stderr, "[vulkan] no device\n"); return 0; }
    vkGetPhysicalDeviceProperties(V.pd, &V.props);
    VkPhysicalDeviceFeatures have;
    vkGetPhysicalDeviceFeatures(V.pd, &have);
    VkPhysicalDeviceFeatures want = { .textureCompressionBC = VK_TRUE, .samplerAnisotropy = have.samplerAnisotropy,
                                      .fillModeNonSolid = have.fillModeNonSolid };
    V.fill_nonsolid = have.fillModeNonSolid;
    V.max_aniso = have.samplerAnisotropy ? V.props.limits.maxSamplerAnisotropy : 1.0f;
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                             .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = V.qf, .queueCount = 1,
                                   .pQueuePriorities = &prio };
    const char *dext[2] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME };
    VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qi, .enabledExtensionCount = 2, .ppEnabledExtensionNames = dext,
                              .pEnabledFeatures = &want };
    if (!ok(vkCreateDevice(V.pd, &di, NULL, &V.dev), "device")) return 0;
    load_device(V.dev);
    vkGetDeviceQueue(V.dev, V.qf, 0, &V.queue);
    fprintf(stderr, "[vulkan] %s, Vulkan %u.%u\n", V.props.deviceName, VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion));
    perf_note("Vulkan device: %s (driver %u.%u.%u)", V.props.deviceName, VK_API_VERSION_MAJOR(V.props.driverVersion),
              VK_API_VERSION_MINOR(V.props.driverVersion), VK_API_VERSION_PATCH(V.props.driverVersion));
    if (V.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) perf_note("WARNING: software rendering (%s): the graphics card is not used", V.props.deviceName);

    VmaVulkanFunctions vf = { .vkGetInstanceProcAddr = gipa, .vkGetDeviceProcAddr = vkGetDeviceProcAddr };
    VmaAllocatorCreateInfo ai = { .physicalDevice = V.pd, .device = V.dev, .instance = V.inst, .pVulkanFunctions = &vf,
                                  .vulkanApiVersion = VK_API_VERSION_1_3 };
    if (!ok(vmaCreateAllocator(&ai, &V.vma), "allocator")) return 0;

    /* a depth format with stencil: D24S8 if the device draws into it, else D32S8 */
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(V.pd, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    V.depth_fmt = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? VK_FORMAT_D24_UNORM_S8_UINT
                                                                                              : VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = V.qf };
    vkCreateCommandPool(V.dev, &pci, NULL, &V.pool);
    for (int i = 0; i < NFRAMES; i++) {
        VkCommandBuffer cbs[2];
        VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = V.pool,
                                            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2 };
        vkAllocateCommandBuffers(V.dev, &cai, cbs);
        V.fr[i].cmd = cbs[0], V.fr[i].up = cbs[1];
        VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        vkCreateFence(V.dev, &fi, NULL, &V.fr[i].fence);
        VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(V.dev, &si, NULL, &V.fr[i].acquired);
    }
    /* the layouts: three uniform blocks and four textures, pushed; the overlay's one texture */
    VkDescriptorSetLayoutBinding lb[7];
    for (int i = 0; i < 7; i++)
        lb[i] = (VkDescriptorSetLayoutBinding){ (uint32_t)i, i < 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                i < 3 ? VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 7, .pBindings = lb };
    vkCreateDescriptorSetLayout(V.dev, &lci, NULL, &V.dsl);
    VkPipelineLayoutCreateInfo plc = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &V.dsl };
    vkCreatePipelineLayout(V.dev, &plc, NULL, &V.pl);
    lb[0] = (VkDescriptorSetLayoutBinding){ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    lci.bindingCount = 1;
    vkCreateDescriptorSetLayout(V.dev, &lci, NULL, &V.odsl);
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 16 };
    plc.pSetLayouts = &V.odsl, plc.pushConstantRangeCount = 1, plc.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(V.dev, &plc, NULL, &V.opl);
    V.vs = shader(spv_d3d8_vert, sizeof spv_d3d8_vert), V.fs = shader(spv_d3d8_frag, sizeof spv_d3d8_frag);
    V.ovs = shader(spv_overlay_vert, sizeof spv_overlay_vert), V.ofs = shader(spv_overlay_frag, sizeof spv_overlay_frag);
    VkPipelineCacheCreateInfo cci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    vkCreatePipelineCache(V.dev, &cci, NULL, &V.pcache);
    V.pipes = (pent_t *)calloc(NPIPE, sizeof *V.pipes);
    {   /* the overlay's pipeline: a blended strip, no vertices */
        VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
        VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
        VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .lineWidth = 1 };
        VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = 1 };
        VkPipelineColorBlendAttachmentState ba = { .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xF };
        VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ba };
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
        VkFormat cf = VK_FORMAT_B8G8R8A8_UNORM;
        VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &cf };
        VkPipelineShaderStageCreateInfo st[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = V.ovs, .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = V.ofs, .pName = "main" } };
        VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = st,
            .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
            .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &dy, .layout = V.opl };
        if (!ok(vkCreateGraphicsPipelines(V.dev, V.pcache, 1, &ci, NULL, &V.opipe), "overlay pipeline")) return 0;
    }
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f };
    vkCreateSampler(V.dev, &sci, NULL, &V.lin);

    V.want_vsync = overlay_vsync();
    if (!make_swapchain()) return 0;
    begin_frame();
    /* the defaults (binding 1, stride 0) and the white texture */
    {
        uint8_t def[48] = { 0 };
        memset(def + 16, 0xFF, 4);                     /* diffuse white; specular 0 */
        float one = 1.0f;
        memcpy(def + 44, &one, 4);                     /* texcoord (0 0 0 1) */
        VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizeof def,
                                  .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
        VmaAllocationCreateInfo vai = { .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE };
        if (!ok(vmaCreateBuffer(V.vma, &bi, &vai, &V.defaults, &V.defaults_mem, NULL), "defaults")) return 0;
        VkBuffer sb; VkDeviceSize so;
        uint8_t *p = arena(sizeof def, 16, &sb, &so);
        memcpy(p, def, sizeof def);
        VkBufferCopy r = { so, 0, sizeof def };
        vkCmdCopyBuffer(up_cmd(), sb, V.defaults, 1, &r);
        uint32_t px = 0xFFFFFFFFu;
        V.white = img_new(1, 1, 1, VK_FORMAT_B8G8R8A8_UNORM, SW_ID, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1);
        if (!V.white) return 0;
        img_upload(V.white, 0, (const uint8_t *)&px, 4, 1, 1, 0);
        layout(up_cmd(), V.white, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    back_images(d);
    /* the back buffer black at first */
    layout(F()->cmd, V.back, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black = { .float32 = { 0, 0, 0, 1 } };
    VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(F()->cmd, V.back->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
    d->vk = &V;
    return 1;
}
static void vk_resize(d3ddev_t *d) {
    submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    vkDeviceWaitIdle(V.dev);
    begin_frame();
    back_images(d);
}
static void vk_targets(d3ddev_t *d) { (void)d; end_rendering(); }
static int vk_tex_init(tex_t *t) {
    if (!(t->usage & 1)) return 1;                     /* a texture: its image at first use */
    vkimg_t *m = color_target(t->w, t->h, 1);
    if (!m) return 0;
    t->vk = m;
    return 1;
}
static void vk_tex_free(tex_t *t) {
    if (t->vk) {
        if (V.rcol == t->vk) end_rendering(), V.rcol = NULL;
        img_free((vkimg_t *)t->vk), t->vk = NULL;
    }
}
static int vk_rt_init(surf_t *s) { s->vk = color_target(s->w, s->h, 1); return s->vk != NULL; }
static int vk_depth_init(surf_t *s) { s->vk = depth_target(s->w, s->h, 1); return s->vk != NULL; }
static void vk_surf_free(surf_t *s) {
    if (!s->vk || s->vk == V.back || s->vk == V.depth) return;
    if (V.rcol == s->vk || V.rdep == s->vk) end_rendering(), V.rcol = V.rdep = NULL;
    img_free((vkimg_t *)s->vk), s->vk = NULL;
}
static void vk_buf_free(buf_t *b) { spares_release((vkbuf_t *)b->vk), b->vk = NULL; }
static void vk_read_back(d3ddev_t *d, surf_t *s) {
    resolve_back(d);
    uint8_t *px = read_back_image(d, V.back);
    if (px) {
        uint32_t n = (uint32_t)d->bw * (uint32_t)d->bh * 4;
        memcpy(s->mem, px, n < s->size ? n : s->size);
        free(px);
    }
}
static void vk_vsync(int on) { V.want_vsync = on; }

const renderer_t d3d8_vk_renderer = {
    "Vulkan", vk_create, vk_resize, vk_present, vk_clear, vk_targets, vk_draw,
    vk_tex_init, vk_tex_free, vk_rt_init, vk_depth_init, vk_surf_free, vk_buf_free, vk_read_back, vk_vsync,
};
