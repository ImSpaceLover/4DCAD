// Vulkan device, window swapchain and frame loop (used when the Vulkan backend is chosen).
// Functions are loaded at runtime through volk, so the program starts without a Vulkan
// driver (it then falls back to OpenGL).
#pragma once
#ifdef FD_HAVE_VULKAN
#include <volk.h>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct GLFWwindow;
struct ImDrawData;

namespace fd {

struct VulkanContext {
  static constexpr int FRAMES = 2;  // frames in flight

  VkInstance instance = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties props{};
  VkDevice device = VK_NULL_HANDLE;
  uint32_t queueFamily = 0;
  VkQueue queue = VK_NULL_HANDLE;
  std::string deviceName;

  // window swapchain + the render pass ImGui draws with
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkFormat swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
  VkExtent2D extent{0, 0};
  std::vector<VkImage> images;
  std::vector<VkImageView> views;
  std::vector<VkFramebuffer> framebuffers;
  VkRenderPass uiPass = VK_NULL_HANDLE;
  uint32_t minImageCount = 2;
  bool swapchainDirty = false;
  bool readbackSupported = false;

  struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
  };
  Frame frames[FRAMES];
  int frame = 0;          // current frame slot
  uint32_t imageIndex = 0;
  bool recording = false;
  VkDescriptorPool imguiPool = VK_NULL_HANDLE;
  // compiled pipelines kept between runs (set the path before init; saved by shutdown)
  std::string pipelineCachePath;
  VkPipelineCache pipelineCache = VK_NULL_HANDLE;
  /** Called at the start of each frame once its previous use finished (renderer resets per-frame allocators). */
  std::function<void(int slot)> onFrameStart;

  /** Creates everything for the window; false + err if Vulkan is not usable. */
  bool init(GLFWwindow* win, std::string& err);
  /** ImGui Vulkan backend setup (after ImGui_ImplGlfw_InitForVulkan). */
  bool initImGui();
  /** Waits for the frame slot, acquires a swapchain image and starts recording. False: skip this frame. */
  bool beginFrame(GLFWwindow* win);
  VkCommandBuffer cmd() const { return frames[frame].cmd; }
  /** Records the ImGui pass, submits and presents. If `rgba` is given, the presented image is read back. */
  void endFrame(ImDrawData* dd, std::vector<uint8_t>* rgba = nullptr);
  void shutdown();
  /** One-off commands (uploads, render threads' draws), executed synchronously. Any thread. */
  void immediate(const std::function<void(VkCommandBuffer)>& fn);
  /** vkDeviceWaitIdle with the queue lock held (any thread). */
  void waitIdle();
  // the one queue is used by the UI thread and the render threads: every submit / present /
  // wait-idle holds queueMutex; ImGui's descriptor pool (image textures) is guarded by poolMutex
  std::mutex queueMutex;
  std::recursive_mutex poolMutex;
  uint32_t memoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) const;

 private:
  GLFWwindow* win_ = nullptr;
  uint64_t slotFrame_[FRAMES] = {0, 0};  // UI frame number last submitted in each slot (see uiFrames())
  std::mutex poolsMutex_;
  std::map<std::thread::id, VkCommandPool> pools_;  // immediate(): one command pool per thread
  VkCommandPool threadPool();
  bool createSwapchain();
  void destroySwapchain();
};

}  // namespace fd
#endif
