#ifdef FD_HAVE_VULKAN
#include "vk_context.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "renderer.hpp"

namespace fd {

#define VKCHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { err = std::string(#x) + " failed (" + std::to_string((int)r_) + ")"; return false; } } while (0)

uint32_t VulkanContext::memoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(phys, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
  return UINT32_MAX;
}

bool VulkanContext::init(GLFWwindow* win, std::string& err) {
  win_ = win;
  if (volkInitialize() != VK_SUCCESS) { err = "no Vulkan loader (vulkan-1)"; return false; }
  if (!glfwVulkanSupported()) { err = "GLFW: Vulkan not supported"; return false; }
  uint32_t n = 0;
  const char** ext = glfwGetRequiredInstanceExtensions(&n);
  if (!ext) { err = "no Vulkan surface extensions"; return false; }
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "4DCAD";
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = n;
  ici.ppEnabledExtensionNames = ext;
  VKCHECK(vkCreateInstance(&ici, nullptr, &instance));
  volkLoadInstance(instance);
  // GLFW creates the surface with its own loader; give it ours
  VKCHECK(glfwCreateWindowSurface(instance, win, nullptr, &surface));

  // device: prefer a discrete GPU with a graphics queue that can present
  uint32_t pc = 0;
  vkEnumeratePhysicalDevices(instance, &pc, nullptr);
  std::vector<VkPhysicalDevice> pds(pc);
  vkEnumeratePhysicalDevices(instance, &pc, pds.data());
  int best = -1;
  for (auto pd : pds) {
    uint32_t qc = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qc);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, qf.data());
    for (uint32_t i = 0; i < qc; i++) {
      VkBool32 present = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &present);
      if (!(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(pd, &p);
      int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
      if (score > best) { best = score; phys = pd; queueFamily = i; props = p; }
      break;
    }
  }
  if (!phys) { err = "no Vulkan device can draw to this window"; return false; }
  deviceName = std::string("Vulkan ") + std::to_string(VK_VERSION_MAJOR(props.apiVersion)) + "." + std::to_string(VK_VERSION_MINOR(props.apiVersion)) +
               " | " + props.deviceName;
  float prio = 1;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = queueFamily;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  const char* dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = dext;
  VKCHECK(vkCreateDevice(phys, &dci, nullptr, &device));
  {  // the pipeline cache (the driver checks the saved data and ignores it if it is from another device / version)
    std::vector<char> data;
    if (!pipelineCachePath.empty()) {
      std::ifstream in(pipelineCachePath, std::ios::binary);
      if (in) data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    pci.initialDataSize = data.size();
    pci.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(device, &pci, nullptr, &pipelineCache) != VK_SUCCESS) {
      pci.initialDataSize = 0;
      pci.pInitialData = nullptr;
      vkCreatePipelineCache(device, &pci, nullptr, &pipelineCache);
    }
  }
  volkLoadDevice(device);
  vkGetDeviceQueue(device, queueFamily, 0, &queue);

  // surface format: plain UNORM (the shaders apply gamma themselves, as with OpenGL)
  uint32_t fc = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fc, nullptr);
  std::vector<VkSurfaceFormatKHR> fmts(fc);
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fc, fmts.data());
  swapFormat = fmts.empty() ? VK_FORMAT_B8G8R8A8_UNORM : fmts[0].format;
  for (auto& f : fmts) if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { swapFormat = f.format; break; }

  // the ImGui pass draws over a cleared swapchain image
  VkAttachmentDescription att{};
  att.format = swapFormat;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpi.attachmentCount = 1;
  rpi.pAttachments = &att;
  rpi.subpassCount = 1;
  rpi.pSubpasses = &sub;
  rpi.dependencyCount = 1;
  rpi.pDependencies = &dep;
  VKCHECK(vkCreateRenderPass(device, &rpi, nullptr, &uiPass));

  for (auto& f : frames) {
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = queueFamily;
    VKCHECK(vkCreateCommandPool(device, &cpi, nullptr, &f.pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = f.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VKCHECK(vkAllocateCommandBuffers(device, &cai, &f.cmd));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VKCHECK(vkCreateFence(device, &fci, nullptr, &f.fence));
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VKCHECK(vkCreateSemaphore(device, &sci, nullptr, &f.acquired));
    VKCHECK(vkCreateSemaphore(device, &sci, nullptr, &f.rendered));
  }
  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  dpi.maxSets = 64;
  dpi.poolSizeCount = 1;
  dpi.pPoolSizes = &ps;
  VKCHECK(vkCreateDescriptorPool(device, &dpi, nullptr, &imguiPool));
  if (!createSwapchain()) { err = "cannot create the swapchain"; return false; }
  return true;
}

bool VulkanContext::createSwapchain() {
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
  int w = 0, h = 0;
  glfwGetFramebufferSize(win_, &w, &h);
  if (w <= 0 || h <= 0) return false;  // minimised
  extent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent
                                                   : VkExtent2D{std::clamp((uint32_t)w, caps.minImageExtent.width, caps.maxImageExtent.width),
                                                                std::clamp((uint32_t)h, caps.minImageExtent.height, caps.maxImageExtent.height)};
  if (extent.width == 0 || extent.height == 0) return false;
  minImageCount = std::max(2u, caps.minImageCount);
  uint32_t count = caps.minImageCount + 1;
  if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
  readbackSupported = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
  VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sci.surface = surface;
  sci.minImageCount = count;
  sci.imageFormat = swapFormat;
  sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  sci.imageExtent = extent;
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (readbackSupported ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) sci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // vsync, always available
  sci.clipped = VK_TRUE;
  sci.oldSwapchain = VK_NULL_HANDLE;
  if (vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain) != VK_SUCCESS) return false;
  uint32_t ic = 0;
  vkGetSwapchainImagesKHR(device, swapchain, &ic, nullptr);
  images.resize(ic);
  vkGetSwapchainImagesKHR(device, swapchain, &ic, images.data());
  views.resize(ic);
  framebuffers.resize(ic);
  for (uint32_t i = 0; i < ic; i++) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = images[i];
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = swapFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(device, &vi, nullptr, &views[i]);
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = uiPass;
    fi.attachmentCount = 1;
    fi.pAttachments = &views[i];
    fi.width = extent.width;
    fi.height = extent.height;
    fi.layers = 1;
    vkCreateFramebuffer(device, &fi, nullptr, &framebuffers[i]);
  }
  swapchainDirty = false;
  return true;
}

void VulkanContext::destroySwapchain() {
  for (auto f : framebuffers) vkDestroyFramebuffer(device, f, nullptr);
  for (auto v : views) vkDestroyImageView(device, v, nullptr);
  framebuffers.clear();
  views.clear();
  images.clear();
  if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
  swapchain = VK_NULL_HANDLE;
}

static PFN_vkVoidFunction imguiLoader(const char* name, void* user) {
  auto* ctx = (VulkanContext*)user;
  PFN_vkVoidFunction f = vkGetDeviceProcAddr && ctx->device ? vkGetDeviceProcAddr(ctx->device, name) : nullptr;
  return f ? f : vkGetInstanceProcAddr(ctx->instance, name);
}

bool VulkanContext::initImGui() {
  if (!ImGui_ImplVulkan_LoadFunctions(imguiLoader, this)) return false;
  ImGui_ImplVulkan_InitInfo ii{};
  ii.Instance = instance;
  ii.PhysicalDevice = phys;
  ii.Device = device;
  ii.QueueFamily = queueFamily;
  ii.Queue = queue;
  ii.DescriptorPool = imguiPool;
  ii.RenderPass = uiPass;
  ii.MinImageCount = minImageCount;
  ii.ImageCount = (uint32_t)std::max<size_t>(images.size(), minImageCount);
  ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  return ImGui_ImplVulkan_Init(&ii);
}

bool VulkanContext::beginFrame(GLFWwindow* win) {
  win_ = win;
  if (swapchainDirty || !swapchain) {
    waitIdle();
    destroySwapchain();
    if (!createSwapchain()) return false;
    ImGui_ImplVulkan_SetMinImageCount(minImageCount);
  }
  Frame& f = frames[frame];
  vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX);
  if (slotFrame_[frame] > uiFrames().completed) uiFrames().completed = slotFrame_[frame];  // that UI frame is done
  VkResult r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &imageIndex);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) { swapchainDirty = true; return false; }
  if (r == VK_SUBOPTIMAL_KHR) swapchainDirty = true;
  else if (r != VK_SUCCESS) return false;
  vkResetFences(device, 1, &f.fence);
  if (onFrameStart) onFrameStart(frame);
  vkResetCommandBuffer(f.cmd, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(f.cmd, &bi);
  recording = true;
  return true;
}

void VulkanContext::endFrame(ImDrawData* dd, std::vector<uint8_t>* rgba) {
  Frame& f = frames[frame];
  VkClearValue clear{};
  clear.color = {{0.09f, 0.1f, 0.12f, 1.0f}};
  VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rb.renderPass = uiPass;
  rb.framebuffer = framebuffers[imageIndex];
  rb.renderArea.extent = extent;
  rb.clearValueCount = 1;
  rb.pClearValues = &clear;
  vkCmdBeginRenderPass(f.cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
  if (dd) {
    std::lock_guard<std::recursive_mutex> lk(poolMutex);
    ImGui_ImplVulkan_RenderDrawData(dd, f.cmd);
  }
  vkCmdEndRenderPass(f.cmd);

  // optional read back of the finished image (screenshots / tests)
  VkBuffer rbBuf = VK_NULL_HANDLE;
  VkDeviceMemory rbMem = VK_NULL_HANDLE;
  bool doRead = rgba && readbackSupported;
  if (doRead) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = (VkDeviceSize)extent.width * extent.height * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkCreateBuffer(device, &bci, nullptr, &rbBuf);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(device, rbBuf, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device, &mai, nullptr, &rbMem);
    vkBindBufferMemory(device, rbBuf, rbMem, 0);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = images[imageIndex];
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy cp{};
    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(f.cmd, images[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbBuf, 1, &cp);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(f.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  }
  vkEndCommandBuffer(f.cmd);
  recording = false;
  VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.waitSemaphoreCount = 1;
  si.pWaitSemaphores = &f.acquired;
  si.pWaitDstStageMask = &wait;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &f.cmd;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &f.rendered;
  {
    std::lock_guard<std::mutex> lk(queueMutex);
    vkQueueSubmit(queue, 1, &si, f.fence);
  }
  slotFrame_[frame] = uiFrames().current;
  uiFrames().current++;
  if (doRead) {
    vkWaitForFences(device, 1, &f.fence, VK_TRUE, UINT64_MAX);
    void* p = nullptr;
    vkMapMemory(device, rbMem, 0, VK_WHOLE_SIZE, 0, &p);
    rgba->resize((size_t)extent.width * extent.height * 4);
    std::memcpy(rgba->data(), p, rgba->size());
    vkUnmapMemory(device, rbMem);
    if (swapFormat == VK_FORMAT_B8G8R8A8_UNORM || swapFormat == VK_FORMAT_B8G8R8A8_SRGB)
      for (size_t i = 0; i < rgba->size(); i += 4) std::swap((*rgba)[i], (*rgba)[i + 2]);
    vkDestroyBuffer(device, rbBuf, nullptr);
    vkFreeMemory(device, rbMem, nullptr);
  }
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &f.rendered;
  pi.swapchainCount = 1;
  pi.pSwapchains = &swapchain;
  pi.pImageIndices = &imageIndex;
  VkResult r;
  {
    std::lock_guard<std::mutex> lk(queueMutex);
    r = vkQueuePresentKHR(queue, &pi);
  }
  if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapchainDirty = true;
  frame = (frame + 1) % FRAMES;
}

VkCommandPool VulkanContext::threadPool() {
  std::lock_guard<std::mutex> lk(poolsMutex_);
  VkCommandPool& p = pools_[std::this_thread::get_id()];
  if (!p) {
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cpi.queueFamilyIndex = queueFamily;
    vkCreateCommandPool(device, &cpi, nullptr, &p);
  }
  return p;
}

void VulkanContext::waitIdle() {
  std::lock_guard<std::mutex> lk(queueMutex);
  vkDeviceWaitIdle(device);
}

void VulkanContext::immediate(const std::function<void(VkCommandBuffer)>& fn) {
  VkCommandPool pool = threadPool();
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkCommandBuffer cb;
  vkAllocateCommandBuffers(device, &cai, &cb);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &bi);
  fn(cb);
  vkEndCommandBuffer(cb);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  vkCreateFence(device, &fci, nullptr, &fence);
  {
    std::lock_guard<std::mutex> lk(queueMutex);
    vkQueueSubmit(queue, 1, &si, fence);
  }
  vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);  // (only this work: other threads keep going)
  vkDestroyFence(device, fence, nullptr);
  vkFreeCommandBuffers(device, pool, 1, &cb);
}

void VulkanContext::shutdown() {
  if (!device) {
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
    surface = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
    return;
  }
  vkDeviceWaitIdle(device);
  destroySwapchain();
  if (pipelineCache) {
    size_t n = 0;
    if (!pipelineCachePath.empty() && vkGetPipelineCacheData(device, pipelineCache, &n, nullptr) == VK_SUCCESS && n > 0) {
      std::vector<char> data(n);
      if (vkGetPipelineCacheData(device, pipelineCache, &n, data.data()) == VK_SUCCESS) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(pipelineCachePath).parent_path(), ec);
        std::ofstream out(pipelineCachePath, std::ios::binary);
        out.write(data.data(), (std::streamsize)n);
      }
    }
    vkDestroyPipelineCache(device, pipelineCache, nullptr);
    pipelineCache = VK_NULL_HANDLE;
  }
  for (auto& [t, p] : pools_) vkDestroyCommandPool(device, p, nullptr);
  pools_.clear();
  for (auto& f : frames) {
    vkDestroyFence(device, f.fence, nullptr);
    vkDestroySemaphore(device, f.acquired, nullptr);
    vkDestroySemaphore(device, f.rendered, nullptr);
    vkDestroyCommandPool(device, f.pool, nullptr);
  }
  vkDestroyDescriptorPool(device, imguiPool, nullptr);
  vkDestroyRenderPass(device, uiPass, nullptr);
  vkDestroyDevice(device, nullptr);
  vkDestroySurfaceKHR(instance, surface, nullptr);
  vkDestroyInstance(instance, nullptr);
  device = VK_NULL_HANDLE;
  instance = VK_NULL_HANDLE;
}

}  // namespace fd
#endif
