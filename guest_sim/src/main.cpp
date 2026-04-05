// Guest simulator: builds Venus command streams and sends them to Host over TCP.
// This simulates what a Guest-side DXVK + Venus encoder would do.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "../../common/venus/vn_encoder.h"
#include "../../common/transport/tcp_transport.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

static constexpr uint32_t WINDOW_WIDTH = 800;
static constexpr uint32_t WINDOW_HEIGHT = 600;

// Handle IDs (must match what host decoder expects)
enum HandleIds : uint64_t {
    H_DEVICE       = 1,
    H_QUEUE        = 2,
    H_SWAPCHAIN    = 3,
    H_RENDER_PASS  = 10,
    H_VERT_SHADER  = 11,
    H_FRAG_SHADER  = 12,
    H_PIPE_LAYOUT  = 13,
    H_PIPELINE     = 14,
    H_CMD_POOL     = 20,
    H_CMD_BUF      = 21,
    H_SEM_IMAGE    = 30,
    H_SEM_RENDER   = 31,
    H_FENCE        = 32,
    H_FB_BASE      = 100,
};

static std::vector<char> readShaderFile(const std::string& path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f.is_open()) {
        fprintf(stderr, "Cannot open shader: %s\n", path.c_str());
        return {};
    }
    size_t sz = (size_t)f.tellg();
    std::vector<char> buf(sz);
    f.seekg(0);
    f.read(buf.data(), sz);
    return buf;
}

static std::vector<uint8_t> buildSetupStream(const char* shaderDir) {
    VnEncoder enc;

    auto vertSpv = readShaderFile(std::string(shaderDir) + "/triangle.vert.spv");
    auto fragSpv = readShaderFile(std::string(shaderDir) + "/triangle.frag.spv");
    if (vertSpv.empty() || fragSpv.empty()) return {};

    enc.cmdBridgeCreateSwapchain(H_DEVICE, H_SWAPCHAIN, WINDOW_WIDTH, WINDOW_HEIGHT, 3);

    uint32_t format = 50; // VK_FORMAT_B8G8R8A8_SRGB
    uint32_t loadOp = 1;  // VK_ATTACHMENT_LOAD_OP_CLEAR
    uint32_t storeOp = 0; // VK_ATTACHMENT_STORE_OP_STORE
    uint32_t initialLayout = 0; // VK_IMAGE_LAYOUT_UNDEFINED
    uint32_t finalLayout = 2;   // VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
    enc.cmdCreateRenderPass(H_DEVICE, H_RENDER_PASS, 1,
                            &format, &loadOp, &storeOp, &initialLayout, &finalLayout);

    enc.cmdCreateShaderModule(H_DEVICE, H_VERT_SHADER,
                              reinterpret_cast<const uint32_t*>(vertSpv.data()), vertSpv.size());
    enc.cmdCreateShaderModule(H_DEVICE, H_FRAG_SHADER,
                              reinterpret_cast<const uint32_t*>(fragSpv.data()), fragSpv.size());

    enc.cmdCreatePipelineLayout(H_DEVICE, H_PIPE_LAYOUT, 0, nullptr, 0, nullptr);
    enc.cmdCreateGraphicsPipeline(H_DEVICE, H_PIPELINE, H_RENDER_PASS, H_PIPE_LAYOUT,
                                  H_VERT_SHADER, H_FRAG_SHADER, WINDOW_WIDTH, WINDOW_HEIGHT,
                                  0); // 0 = use renderPass (not dynamic rendering)

    for (uint32_t i = 0; i < 3; i++) {
        uint64_t imageViewId = H_SWAPCHAIN * 100 + i + 1;
        enc.cmdCreateFramebuffer(H_DEVICE, H_FB_BASE + i, H_RENDER_PASS,
                                 imageViewId, WINDOW_WIDTH, WINDOW_HEIGHT);
    }

    enc.cmdCreateCommandPool(H_DEVICE, H_CMD_POOL, 0);
    enc.cmdAllocateCommandBuffers(H_DEVICE, H_CMD_POOL, H_CMD_BUF);
    enc.cmdCreateSemaphore(H_DEVICE, H_SEM_IMAGE);
    enc.cmdCreateSemaphore(H_DEVICE, H_SEM_RENDER);
    enc.cmdCreateFence(H_DEVICE, H_FENCE, 1); // VK_FENCE_CREATE_SIGNALED_BIT

    enc.cmdEndOfStream();
    auto* d = enc.data();
    return std::vector<uint8_t>(d, d + enc.size());
}

static std::vector<uint8_t> buildFrameStream(uint64_t fbId) {
    VnEncoder enc;

    enc.cmdBridgeAcquireNextImage(H_SWAPCHAIN, H_SEM_IMAGE);
    enc.cmdWaitForFences(H_DEVICE, H_FENCE);
    enc.cmdResetFences(H_DEVICE, H_FENCE);

    enc.cmdBeginCommandBuffer(H_CMD_BUF);
    enc.cmdBeginRenderPass(H_CMD_BUF, H_RENDER_PASS, fbId,
                           WINDOW_WIDTH, WINDOW_HEIGHT,
                           0.0f, 0.0f, 0.0f, 1.0f);
    enc.cmdBindPipeline(H_CMD_BUF, H_PIPELINE);
    enc.cmdDraw(H_CMD_BUF, 3, 1, 0, 0);
    enc.cmdEndRenderPass(H_CMD_BUF);
    enc.cmdEndCommandBuffer(H_CMD_BUF);

    enc.cmdQueueSubmit(H_QUEUE, H_CMD_BUF, H_SEM_IMAGE, H_SEM_RENDER, H_FENCE);
    enc.cmdBridgeQueuePresent(H_QUEUE, H_SWAPCHAIN, H_SEM_RENDER);

    enc.cmdEndOfStream();
    auto* d = enc.data();
    return std::vector<uint8_t>(d, d + enc.size());
}

int main(int argc, char* argv[]) {
    const char* host = "127.0.0.1";
    uint16_t port = DEFAULT_PORT;
    const char* shaderDir = "shaders";

    // Parse args: guest_sim [host] [port] [shader_dir]
    if (argc > 1) host = argv[1];
    if (argc > 2) port = static_cast<uint16_t>(atoi(argv[2]));
    if (argc > 3) shaderDir = argv[3];

    fprintf(stderr, "[Guest] Connecting to %s:%u...\n", host, port);

    TcpSender sender;
    if (!sender.connect(host, port)) {
        fprintf(stderr, "[Guest] Connection failed.\n");
        return 1;
    }

    // --- Send setup stream ---
    auto setupStream = buildSetupStream(shaderDir);
    if (setupStream.empty()) {
        fprintf(stderr, "[Guest] Failed to build setup stream.\n");
        return 1;
    }

    if (!sender.send(setupStream.data(), setupStream.size())) {
        fprintf(stderr, "[Guest] Failed to send setup stream.\n");
        return 1;
    }
    fprintf(stderr, "[Guest] Setup stream sent (%zu bytes).\n", setupStream.size());

    // Receive initial response (image index)
    uint32_t imageIndex = 0;
    uint8_t respBuf[64];
    size_t respSize = sender.recv(respBuf, sizeof(respBuf));
    if (respSize >= 4) {
        memcpy(&imageIndex, respBuf, 4);
    }

    // --- Frame loop ---
    fprintf(stderr, "[Guest] Entering frame loop. Press Ctrl+C to stop.\n");
    uint32_t frameCount = 0;

    while (true) {
        uint64_t fbId = H_FB_BASE + imageIndex;
        auto frameStream = buildFrameStream(fbId);

        if (!sender.send(frameStream.data(), frameStream.size())) {
            fprintf(stderr, "[Guest] Send failed at frame %u.\n", frameCount);
            break;
        }

        // Wait for host response: updated image index
        respSize = sender.recv(respBuf, sizeof(respBuf));
        if (respSize < 4) {
            fprintf(stderr, "[Guest] Host disconnected at frame %u.\n", frameCount);
            break;
        }
        memcpy(&imageIndex, respBuf, 4);

        frameCount++;
        if (frameCount % 300 == 0) {
            fprintf(stderr, "[Guest] %u frames sent.\n", frameCount);
        }
    }

    sender.close();
    fprintf(stderr, "[Guest] Done. Total frames: %u\n", frameCount);
    return 0;
}
