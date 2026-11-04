// Vulkan backend: GLSL is compiled to SPIR-V with glslang at runtime (one pipeline per scene
// structure, like the OpenGL programs). Frame uniforms and node data go into per-frame
// host-visible buffers; each render target is an image the UI samples (ImGui texture).
#ifdef FD_HAVE_VULKAN
#include <cstdio>
#include <cstring>
#include <mutex>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "renderer.hpp"
#include "vk_context.hpp"

namespace fd {

namespace {

bool compileGlsl(EShLanguage stage, const std::string& src, std::vector<uint32_t>& spv, std::string& err) {
  static std::once_flag once;
  std::call_once(once, [] { glslang::InitializeProcess(); });
  glslang::TShader sh(stage);
  const char* s = src.c_str();
  sh.setStrings(&s, 1);
  sh.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
  sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
  if (!sh.parse(GetDefaultResources(), 450, false, EShMsgDefault)) { err = sh.getInfoLog(); return false; }
  glslang::TProgram prog;
  prog.addShader(&sh);
  if (!prog.link(EShMsgDefault)) { err = prog.getInfoLog(); return false; }
  spv.clear();
  glslang::GlslangToSpv(*prog.getIntermediate(stage), spv);
  return true;
}

struct VKProgram {
  VkPipeline pipeline = VK_NULL_HANDLE;
};

// one size variant of a target (moving / idle resolution alternate, so two are kept)
struct VKImage {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFramebuffer fb = VK_NULL_HANDLE;
  VkDescriptorSet tex = VK_NULL_HANDLE;  // ImGui texture
  int w = 0, h = 0;
  uint64_t lastUse = 0;
};
struct VKTarget {
  VKImage v[2];
  int current = -1;
};

// per-frame upload memory (uniforms + node data), in chunks
struct Chunk {
  VkBuffer buf = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  uint8_t* ptr = nullptr;
  VkDeviceSize size = 0, used = 0;
};
struct FrameRes {
  std::vector<Chunk> chunks;
  VkDescriptorPool pool = VK_NULL_HANDLE;
};

class VKRenderer : public Renderer {
 public:
  explicit VKRenderer(VulkanContext& c) : ctx_(c) {}
  bool init(std::string& err) {
    VkDevice d = ctx_.device;
    device = ctx_.deviceName;
    // descriptor layout: frame uniforms + node data
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(d, &li, nullptr, &setLayout_) != VK_SUCCESS) { err = "descriptor set layout"; return false; }
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &setLayout_;
    if (vkCreatePipelineLayout(d, &pli, nullptr, &pipeLayout_) != VK_SUCCESS) { err = "pipeline layout"; return false; }
    // offscreen pass: the target is fully overwritten, then sampled by the UI
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // progressive sampling blends into what is there
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;  // images are created in this layout
    att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep[2]{};
    // earlier reads of the image (the UI of a previous frame) finish before we write it
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    // our writes are visible to the UI pass that samples the image
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 2;
    rpi.pDependencies = dep;
    if (vkCreateRenderPass(d, &rpi, nullptr, &pass_) != VK_SUCCESS) { err = "render pass"; return false; }
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 1;
    if (vkCreateSampler(d, &si, nullptr, &sampler_) != VK_SUCCESS) { err = "sampler"; return false; }
    std::vector<uint32_t> spv;
    if (!compileGlsl(EShLangVertex, vertexShader(ShaderTarget::Vulkan), spv, err)) return false;
    vert_ = module(spv);
    for (int i = 0; i <= VulkanContext::FRAMES; i++) {
      FrameRes& fr = i < VulkanContext::FRAMES ? frames_[i] : imm_;
      VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64}};
      VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      dpi.maxSets = 64;
      dpi.poolSizeCount = 2;
      dpi.pPoolSizes = ps;
      vkCreateDescriptorPool(d, &dpi, nullptr, &fr.pool);
    }
    ctx_.onFrameStart = [this](int slot) {
      FrameRes& f = frames_[slot];
      for (auto& c : f.chunks) c.used = 0;
      vkResetDescriptorPool(ctx_.device, f.pool, 0);
      frameCounter_++;
    };
    return true;
  }

  ~VKRenderer() override {
    vkDeviceWaitIdle(ctx_.device);
    clearPrograms();
    VkDevice d = ctx_.device;
    for (auto& t : targets_) for (auto& v : t.v) freeImage(v);
    auto freeRes = [&](FrameRes& f) {
      for (auto& c : f.chunks) { vkDestroyBuffer(d, c.buf, nullptr); vkFreeMemory(d, c.mem, nullptr); }
      vkDestroyDescriptorPool(d, f.pool, nullptr);
    };
    for (auto& f : frames_) freeRes(f);
    freeRes(imm_);
    vkDestroyShaderModule(d, vert_, nullptr);
    vkDestroySampler(d, sampler_, nullptr);
    vkDestroyRenderPass(d, pass_, nullptr);
    vkDestroyPipelineLayout(d, pipeLayout_, nullptr);
    vkDestroyDescriptorSetLayout(d, setLayout_, nullptr);
    ctx_.onFrameStart = nullptr;
  }

  uint64_t texture(const RenderTarget& rt) override {
    if (rt.id < 0 || rt.id >= (int)targets_.size()) return 0;
    const VKTarget& t = targets_[rt.id];
    return t.current >= 0 ? (uint64_t)(uintptr_t)t.v[t.current].tex : 0;
  }
  bool flipY() const override { return false; }
  const char* backendName() const override { return "Vulkan"; }
  void destroy(RenderTarget& rt) override {
    if (rt.id < 0 || rt.id >= (int)targets_.size()) return;
    vkDeviceWaitIdle(ctx_.device);
    for (auto& v : targets_[rt.id].v) freeImage(v);
    targets_[rt.id].current = -1;
    rt = RenderTarget{};
  }

 protected:
  ShaderTarget shaderTarget() const override { return ShaderTarget::Vulkan; }

  void* createProgram(const std::string& fs, std::string& err) override {
    std::vector<uint32_t> spv;
    if (!compileGlsl(EShLangFragment, fs, spv, err)) return nullptr;
    VkShaderModule frag = module(spv);
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vert_;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = frag;
    st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    // new sample = src * w + previous * (1 - w); w = 1 replaces (blend constant alpha, per draw)
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
    cba.dstColorBlendFactor = cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    cba.colorBlendOp = cba.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 3;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &ds;
    pi.layout = pipeLayout_;
    pi.renderPass = pass_;
    auto* p = new VKProgram;
    VkResult r = vkCreateGraphicsPipelines(ctx_.device, VK_NULL_HANDLE, 1, &pi, nullptr, &p->pipeline);
    vkDestroyShaderModule(ctx_.device, frag, nullptr);
    if (r != VK_SUCCESS) { err = "vkCreateGraphicsPipelines failed (" + std::to_string((int)r) + ")"; delete p; return nullptr; }
    return p;
  }
  void destroyProgram(void* h) override {
    auto* p = (VKProgram*)h;
    if (!p) return;
    vkDeviceWaitIdle(ctx_.device);  // a frame in flight may still use it
    vkDestroyPipeline(ctx_.device, p->pipeline, nullptr);
    delete p;
  }
  std::string compileCheck(const std::string& fs) override {
    std::vector<uint32_t> spv;
    std::string err;
    if (!compileGlsl(EShLangFragment, fs, spv, err) && err.empty()) err = "compile error";
    return err;
  }

  void draw(RenderTarget& rt, int w, int h, void* prog, const FrameUniforms& u, const std::vector<float>& nodes, int) override {
    if (immediate_) {  // offline renders: submitted and finished right away (with their own upload memory)
      VKImage& img = ensure(rt, w, h);
      ctx_.immediate([&](VkCommandBuffer cmd) { record(cmd, img, prog, u, nodes); });
      for (auto& c : imm_.chunks) c.used = 0;
      vkResetDescriptorPool(ctx_.device, imm_.pool, 0);
      return;
    }
    if (!ctx_.recording) return;  // only inside a frame
    VKImage& img = ensure(rt, w, h);
    record(ctx_.cmd(), img, prog, u, nodes);
  }

  void setImmediate(bool on) override { immediate_ = on; }

  bool readPixels(const RenderTarget& rt, Pixels& out) override {
    if (rt.id < 0 || rt.id >= (int)targets_.size() || targets_[rt.id].current < 0) return false;
    VKImage& img = targets_[rt.id].v[targets_[rt.id].current];
    VkDevice d = ctx_.device;
    VkDeviceSize bytes = (VkDeviceSize)img.w * img.h * 8;  // RGBA16F
    VkBuffer buf;
    VkDeviceMemory mem;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(d, &bci, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(d, buf, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = ctx_.memoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(d, &mai, nullptr, &mem) != VK_SUCCESS) { vkDestroyBuffer(d, buf, nullptr); return false; }
    vkBindBufferMemory(d, buf, mem, 0);
    ctx_.immediate([&](VkCommandBuffer cmd) {
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
      b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = img.image;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
      VkBufferImageCopy cp{};
      cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      cp.imageExtent = {(uint32_t)img.w, (uint32_t)img.h, 1};
      vkCmdCopyImageToBuffer(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &cp);
      b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    });
    void* p = nullptr;
    vkMapMemory(d, mem, 0, VK_WHOLE_SIZE, 0, &p);
    const uint16_t* h = (const uint16_t*)p;
    out.resize((size_t)img.w * img.h * 4);
    for (size_t i = 0; i < out.size(); i++) out[i] = (uint8_t)std::lround(std::clamp(halfToFloat(h[i]), 0.f, 1.f) * 255);  // rows top first
    vkUnmapMemory(d, mem);
    vkDestroyBuffer(d, buf, nullptr);
    vkFreeMemory(d, mem, nullptr);
    return true;
  }

  void record(VkCommandBuffer cmd, VKImage& img, void* prog, const FrameUniforms& u, const std::vector<float>& nodes) {
    VkClearValue clear{};
    clear.color = prog ? VkClearColorValue{{0, 0, 0, 1}} : VkClearColorValue{{0.3f, 0.05f, 0.05f, 1}};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = pass_;
    rb.framebuffer = img.fb;
    rb.renderArea.extent = {(uint32_t)img.w, (uint32_t)img.h};
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (prog) {
      // upload this draw's data (before the render pass begins)
      VkDeviceSize ua = ctx_.props.limits.minUniformBufferOffsetAlignment, sa = ctx_.props.limits.minStorageBufferOffsetAlignment;
      VkDeviceSize nodeBytes = std::max<VkDeviceSize>(16, nodes.size() * sizeof(float));
      VkBuffer ub, sb;
      VkDeviceSize uo, so;
      alloc(sizeof(FrameUniforms), ua, &u, sizeof u, ub, uo);
      alloc(nodeBytes, sa, nodes.data(), nodes.size() * sizeof(float), sb, so);
      VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      dai.descriptorPool = res().pool;
      dai.descriptorSetCount = 1;
      dai.pSetLayouts = &setLayout_;
      if (vkAllocateDescriptorSets(ctx_.device, &dai, &set) != VK_SUCCESS) set = VK_NULL_HANDLE;
      if (set) {
        VkDescriptorBufferInfo bi[2] = {{ub, uo, sizeof(FrameUniforms)}, {sb, so, nodeBytes}};
        VkWriteDescriptorSet wr[2]{};
        for (int i = 0; i < 2; i++) {
          wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          wr[i].dstSet = set;
          wr[i].dstBinding = (uint32_t)i;
          wr[i].descriptorCount = 1;
          wr[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
          wr[i].pBufferInfo = &bi[i];
        }
        vkUpdateDescriptorSets(ctx_.device, 2, wr, 0, nullptr);
      }
    }
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    if (!prog || !set) {  // shader failed: an error colour (the pass keeps old contents, so clear explicitly)
      VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0, clear};
      VkClearRect cr{{{0, 0}, {(uint32_t)img.w, (uint32_t)img.h}}, 0, 1};
      vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
    }
    if (prog && set) {
      float bc[4] = {0, 0, 0, u.e[2]};
      vkCmdSetBlendConstants(cmd, bc);
      VkViewport vpt{0, 0, (float)img.w, (float)img.h, 0, 1};
      VkRect2D sc{{0, 0}, {(uint32_t)img.w, (uint32_t)img.h}};
      vkCmdSetViewport(cmd, 0, 1, &vpt);
      vkCmdSetScissor(cmd, 0, 1, &sc);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ((VKProgram*)prog)->pipeline);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout_, 0, 1, &set, 0, nullptr);
      vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(cmd);
  }

 private:
  VulkanContext& ctx_;
  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
  VkRenderPass pass_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkShaderModule vert_ = VK_NULL_HANDLE;
  FrameRes frames_[VulkanContext::FRAMES];
  FrameRes imm_;  // immediate (offline) draws
  bool immediate_ = false;
  std::vector<VKTarget> targets_;
  uint64_t frameCounter_ = 0;

  VkShaderModule module(const std::vector<uint32_t>& spv) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spv.size() * 4;
    ci.pCode = spv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(ctx_.device, &ci, nullptr, &m);
    return m;
  }

  // linear allocation in this frame's upload chunks
  FrameRes& res() { return immediate_ ? imm_ : frames_[ctx_.frame]; }
  static float halfToFloat(uint16_t h) {
    uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 31, m = h & 1023, bits;
    if (e == 0) {
      if (m == 0) bits = s;
      else { e = 127 - 15 + 1; while (!(m & 1024)) { m <<= 1; e--; } m &= 1023; bits = s | (e << 23) | (m << 13); }
    } else if (e == 31) bits = s | 0x7F800000u | (m << 13);
    else bits = s | ((e + 127 - 15) << 23) | (m << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  }

  void alloc(VkDeviceSize size, VkDeviceSize align, const void* data, size_t bytes, VkBuffer& buf, VkDeviceSize& off) {
    FrameRes& f = res();
    align = std::max<VkDeviceSize>(align, 16);
    for (auto& c : f.chunks) {
      VkDeviceSize o = (c.used + align - 1) / align * align;
      if (o + size <= c.size) { put(c, o, size, data, bytes, buf, off); return; }
    }
    Chunk c;
    c.size = std::max<VkDeviceSize>(4u << 20, size * 2);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = c.size;
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    vkCreateBuffer(ctx_.device, &bci, nullptr, &c.buf);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(ctx_.device, c.buf, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = ctx_.memoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(ctx_.device, &mai, nullptr, &c.mem);
    vkBindBufferMemory(ctx_.device, c.buf, c.mem, 0);
    void* p = nullptr;
    vkMapMemory(ctx_.device, c.mem, 0, VK_WHOLE_SIZE, 0, &p);
    c.ptr = (uint8_t*)p;
    f.chunks.push_back(c);
    put(f.chunks.back(), 0, size, data, bytes, buf, off);
  }
  static void put(Chunk& c, VkDeviceSize o, VkDeviceSize size, const void* data, size_t bytes, VkBuffer& buf, VkDeviceSize& off) {
    if (bytes) std::memcpy(c.ptr + o, data, bytes);
    c.used = o + size;
    buf = c.buf;
    off = o;
  }

  void freeImage(VKImage& v) {
    VkDevice d = ctx_.device;
    if (v.tex) ImGui_ImplVulkan_RemoveTexture(v.tex);
    if (v.fb) vkDestroyFramebuffer(d, v.fb, nullptr);
    if (v.view) vkDestroyImageView(d, v.view, nullptr);
    if (v.image) vkDestroyImage(d, v.image, nullptr);
    if (v.mem) vkFreeMemory(d, v.mem, nullptr);
    v = VKImage{};
  }

  bool createImage(VKImage& v, int w, int h) {
    VkDevice d = ctx_.device;
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent = {(uint32_t)w, (uint32_t)h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(d, &ii, nullptr, &v.image) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(d, v.image, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = ctx_.memoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) mai.memoryTypeIndex = ctx_.memoryType(mr.memoryTypeBits, 0);
    if (vkAllocateMemory(d, &mai, nullptr, &v.mem) != VK_SUCCESS) return false;
    vkBindImageMemory(d, v.image, v.mem, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = v.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(d, &vi, nullptr, &v.view);
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = pass_;
    fi.attachmentCount = 1;
    fi.pAttachments = &v.view;
    fi.width = (uint32_t)w;
    fi.height = (uint32_t)h;
    fi.layers = 1;
    vkCreateFramebuffer(d, &fi, nullptr, &v.fb);
    // give it defined contents (the UI may show it before the first render finishes)
    ctx_.immediate([&](VkCommandBuffer cb) {
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = v.image;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    });
    v.tex = ImGui_ImplVulkan_AddTexture(sampler_, v.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    v.w = w;
    v.h = h;
    return true;
  }

  VKImage& ensure(RenderTarget& rt, int w, int h) {
    if (rt.id < 0) { rt.id = (int)targets_.size(); targets_.push_back({}); }
    VKTarget& t = targets_[rt.id];
    int pick = -1;
    for (int i = 0; i < 2; i++) if (t.v[i].image && t.v[i].w == w && t.v[i].h == h) pick = i;
    if (pick < 0) {
      // replace the variant used longest ago (wait: a frame in flight may still read it)
      pick = !t.v[0].image ? 0 : !t.v[1].image ? 1 : (t.v[0].lastUse <= t.v[1].lastUse ? 0 : 1);
      if (t.v[pick].image) { vkDeviceWaitIdle(ctx_.device); freeImage(t.v[pick]); }
      createImage(t.v[pick], w, h);
    }
    t.v[pick].lastUse = frameCounter_;
    t.current = pick;
    rt.w = w;
    rt.h = h;
    return t.v[pick];
  }
};

}  // namespace

std::unique_ptr<Renderer> makeVulkanRenderer(VulkanContext& ctx) {
  auto r = std::make_unique<VKRenderer>(ctx);
  std::string err;
  if (!r->init(err)) {
    std::fprintf(stderr, "Vulkan renderer: %s\n", err.c_str());
    return nullptr;
  }
  return r;
}

}  // namespace fd
#endif
