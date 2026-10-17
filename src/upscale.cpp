// lsl-upscale: headless scaler benchmark. Loads (or generates) an image,
// upscales it on a chosen GPU with each requested scaler, reports GPU
// timestamps and quality metrics against a CPU reference, and optionally
// writes the result and a CSV row.

#include "image_io.hpp"
#include "lsl_vk.hpp"
#include "reference.hpp"
#include "spv_blob.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

namespace {

struct Args {
    std::string in, out, csv;
    uint32_t dstW = 0, dstH = 0;
    uint32_t genW = 0, genH = 0;
    uint32_t renderW = 0, renderH = 0;  // 0 = off; see --render
    std::vector<uint32_t> modes;
    std::optional<int> device;
    int bench = 200;
    float sharpness = lsl::kDefaultSharpness;  // RCAS strength for fsr1rcas
    bool quiet = false;
    // Deliberately drop the EASU->RCAS barrier, as a probe for whether
    // synchronization validation can see a shader storage-buffer hazard at
    // all.  It exists because a clean syncval run proves nothing unless a
    // known hazard would have been reported -- and, measured, this VVL cannot
    // see one (issue 6061).  See the README.
    bool hazard = false;
};

[[noreturn]] void usage(const char* why) {
    std::fprintf(stderr,
        "%s\nusage: lsl-upscale [options]\n"
        "  --in FILE            input P5/P6 image (else --make-test is required)\n"
        "  --make-test WxH      generate a synthetic test pattern\n"
        "  --out FILE.ppm       write the upscaled image\n"
        "  --dst WxH            destination size (required)\n"
        "  --modes LIST         nearest,bilinear,cubic,lanczos2,fsr1,fsr1rcas\n"
        "                       (default: all)\n"
        "  --sharpness N        RCAS strength in stops for fsr1rcas: 0 is the\n"
        "                       strongest lobe AMD's RCAS applies (default 0)\n"
        "  --device N           physical device index (default: see lsl-probe)\n"
        "  --bench N            timed iterations per mode (default 200)\n"
        "  --csv FILE           append a CSV header (once) and rows\n"
        "  --quiet              only print the CSV/stdout table\n"
        "  --hazard             TEST ONLY: omit the EASU->RCAS barrier, to\n"
        "                       check whether sync validation reports it.\n"
        "                       Upstream VVL does not yet flag shader buffer\n"
        "                       hazards (issue 6061) - see README.\n",
        why);
    std::exit(2);
}

bool parseWH(const std::string& s, uint32_t& w, uint32_t& h) {
    const size_t x = s.find('x');
    if (x == std::string::npos) return false;
    try {
        w = uint32_t(std::stoul(s.substr(0, x)));
        h = uint32_t(std::stoul(s.substr(x + 1)));
    } catch (...) {
        return false;
    }
    return w > 0 && h > 0;
}

uint32_t parseMode(const std::string& n) {
    if (n == "nearest")  return lsl::MODE_NEAREST;
    if (n == "bilinear") return lsl::MODE_BILINEAR;
    if (n == "cubic")    return lsl::MODE_CUBIC;
    if (n == "lanczos2") return lsl::MODE_LANCZOS2;
    if (n == "fsr1")     return lsl::MODE_FSR1;
    if (n == "fsr1rcas") return lsl::MODE_FSR1RCAS;
    usage(("unknown mode: " + n).c_str());
}

std::vector<uint32_t> allModes() {
    // Derived from MODE_COUNT, not spelled out: a hand-written list silently
    // dropped fsr1rcas the moment a sixth mode was added.
    std::vector<uint32_t> modes;
    modes.reserve(lsl::MODE_COUNT);
    for (uint32_t m = 0; m < lsl::MODE_COUNT; ++m) modes.push_back(m);
    return modes;
}

Args parse(int argc, char** argv) {
    Args a;
    a.modes = allModes();
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        auto need = [&](const char* what) -> std::string {
            if (i + 1 >= argc) usage((std::string("missing value for ") + what).c_str());
            return argv[++i];
        };
        if (k == "--in")             a.in = need("--in");
        else if (k == "--out")        a.out = need("--out");
        else if (k == "--csv")        a.csv = need("--csv");
        else if (k == "--bench")      a.bench = std::stoi(need("--bench"));
        else if (k == "--device")     a.device = std::stoi(need("--device"));
        else if (k == "--sharpness")  a.sharpness = std::stof(need("--sharpness"));
        else if (k == "--quiet")      a.quiet = true;
        else if (k == "--hazard")     a.hazard = true;
        else if (k == "--dst") {
            if (!parseWH(need("--dst"), a.dstW, a.dstH)) usage("--dst wants WxH");
        } else if (k == "--make-test") {
            if (!parseWH(need("--make-test"), a.genW, a.genH)) usage("--make-test wants WxH");
        } else if (k == "--render") {
            if (!parseWH(need("--render"), a.renderW, a.renderH)) usage("--render wants WxH");
        } else if (k == "--modes") {
            a.modes.clear();
            std::string list = need("--modes");
            size_t p = 0;
            while (p <= list.size()) {
                size_t c = list.find(',', p);
                if (c == std::string::npos) c = list.size();
                if (c > p) a.modes.push_back(parseMode(list.substr(p, c - p)));
                p = c + 1;
            }
            if (a.modes.empty()) usage("--modes was empty");
        } else {
            usage(("unknown argument: " + k).c_str());
        }
    }
    if (a.dstW == 0 && a.renderW == 0)
        usage("--dst WxH (or --render WxH) is required");
    if (a.dstW != 0 && a.renderW != 0)
        usage("--dst and --render are mutually exclusive: with --render the\n"
              "       target size is the input image's size");
    if (a.in.empty() && a.genW == 0) usage("need --in FILE or --make-test WxH");
    if (a.bench < 1) usage("--bench must be >= 1");
    return a;
}

double percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double pos = q * double(v.size() - 1);
    const size_t lo = size_t(pos);
    const size_t hi = std::min(lo + 1, v.size() - 1);
    const double f = pos - double(lo);
    return v[lo] * (1.0 - f) + v[hi] * f;
}

void writeHeaderIfNeeded(const std::string& csv) {
    if (csv.empty()) return;
    FILE* f = std::fopen(csv.c_str(), "r");
    const bool exists = f != nullptr;
    if (f) std::fclose(f);
    if (exists) return;
    FILE* o = std::fopen(csv.c_str(), "w");
    if (!o) { std::perror("csv"); std::exit(1); }
    std::fprintf(o, "gpu,vendor,mode,src,dst,dispatch_ms_avg,dispatch_ms_p99,"
                    "psnr_db,ssim,edge,iterations\n");
    std::fclose(o);
}

}  // namespace

int main(int argc, char** argv) {
    const Args a = parse(argc, argv);
    if (a.hazard)
        std::fprintf(stderr,
                     "[lsl] --hazard: the EASU->RCAS barrier will be omitted; "
                     "this is a test hook only\n");

    lsl::Image truth;
    try {
        truth = a.in.empty() ? lsl::makeTestPattern(a.genW, a.genH)
                             : lsl::loadImage(a.in);
    } catch (const std::exception& e) {
        std::cerr << "input error: " << e.what() << "\n";
        return 1;
    }

    // Without --render the metric is fidelity to the CPU reference, which says
    // nothing about how good the picture looks. With --render the loaded image
    // is ground truth, a stand-in for the low-resolution render is produced by
    // box downsampling, and the metric becomes image quality against the
    // original.
    const bool quality = a.renderW != 0;
    lsl::Image src;
    uint32_t dstW = a.dstW, dstH = a.dstH;
    if (quality) {
        // A render size larger than the ground truth would turn the "downsample
        // to a render proxy" step into an upsample, and every number after it
        // would be measuring the wrong thing.
        if (a.renderW > truth.w || a.renderH > truth.h) {
            std::cerr << "render error: --render " << a.renderW << "x" << a.renderH
                      << " is larger than the ground truth " << truth.w << "x"
                      << truth.h << "\n";
            return 2;
        }
        try {
            src = lsl::downscaleBox(truth, a.renderW, a.renderH);
        } catch (const std::exception& e) {
            std::cerr << "input error: " << e.what() << "\n";
            return 1;
        }
        dstW = truth.w;
        dstH = truth.h;
    } else {
        src = truth;
    }

    if (!a.quiet) {
        const std::string origin = a.in.empty() ? " (synthetic)" : " " + a.in;
        if (quality)
            std::cout << "truth : " << truth.w << "x" << truth.h << origin << "\n"
                      << "render: " << src.w << "x" << src.h << "\n"
                      << "target: " << dstW << "x" << dstH
                      << "   metric: vs ground truth (image quality)\n";
        else
            std::cout << "source: " << src.w << "x" << src.h << origin << "\n"
                      << "target: " << dstW << "x" << dstH
                      << "   metric: vs CPU reference (fidelity)\n";
    }

    std::unique_ptr<lsl::Context> ctx;
    try {
        ctx = std::make_unique<lsl::Context>(a.device);
    } catch (const std::exception& e) {
        std::cerr << "device error: " << e.what() << "\n"
                  << "hint: on this host run  env -u VK_LOADER_DRIVERS_DISABLE "
                     "./build/lsl-probe\n";
        return 1;
    }

    const lsl::DeviceInfo& di = ctx->info();
    const std::string gpuName = di.props.deviceName;
    const std::string gpuVendor = [&] {
        switch (di.props.vendorID) {
            case 0x10de: return "nvidia";
            case 0x1002: return "amd";
            case 0x8086: return "intel";
            default:     return "other";
        }
    }();

    if (!a.quiet)
        std::cout << "GPU   : " << gpuName << " [" << gpuVendor << "], driver "
                  << di.props.driverVersion << "\n"
                  << "heap  : " << (di.deviceLocalBytes / (1024 * 1024))
                  << " MB device-local, compute qf " << di.queueFamily << "\n"
                  << "timing: "
                  << (ctx->timestampsUsable()
                          ? std::string("GPU timestamps, period ") +
                                std::to_string(unsigned(di.props.limits.timestampPeriod)) + " ns"
                          : std::string("timestamps unavailable"))
                  << "\n\n";

    // ---- resources -------------------------------------------------------
    // The timed dispatch reads/writes DEVICE_LOCAL storage; host-visible
    // staging buffers are used once to upload the source and once to download
    // the result. Without this the GPU would touch system memory over PCIe on
    // every iteration and the numbers would measure the bus, not the scaler
    // (on the GTX 1650 every mode collapsed to a flat ~3.3 ms).
    const VkDeviceSize srcBytes = VkDeviceSize(src.rgba.size());
    const VkDeviceSize dstBytes = VkDeviceSize(dstW) * dstH * 4;

    const VkBufferUsageFlags kStore =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    lsl::Context::Buffer srcStage =
        ctx->createBufferEx(srcBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            src.rgba.data());
    lsl::Context::Buffer dstStage =
        ctx->createBufferEx(dstBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    lsl::Context::Buffer srcBuf =
        ctx->createBufferEx(srcBytes, kStore, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    lsl::Context::Buffer dstBuf =
        ctx->createBufferEx(dstBytes, kStore, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    // Holds the unsharpened EASU result between the two dispatches of the RCAS
    // chain. Only that one mode uses it.
    lsl::Context::Buffer scratchBuf =
        ctx->createBufferEx(dstBytes, kStore, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!ctx->info().deviceLocalBytes) {
        // No DEVICE_LOCAL heap (should not happen on a real GPU): fall back so
        // the tool still runs instead of throwing from findMemoryType.
        std::cerr << "warning: no device-local heap reported\n";
    }

    VkShaderModule scaleShader = ctx->createShader(lsl_spv_scale_comp, lsl_spv_scale_comp_words);
    VkShaderModule rcasShader  = ctx->createShader(lsl_spv_rcas_comp,  lsl_spv_rcas_comp_words);
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(lsl::Push);
    VkDescriptorSetLayout setLayout = ctx->createSetLayout();
    VkPipelineLayout layout = ctx->createLayout(pcr, setLayout);
    VkPipeline scalePipe = ctx->createCompute(layout, scaleShader);
    VkPipeline rcasPipe  = ctx->createCompute(layout, rcasShader);
    VkDescriptorPool pool = ctx->createPool();
    VkCommandPool cmdPool = ctx->createCmdPool();
    VkQueryPool queries = ctx->createQueries(2);

    // One descriptor set per pass: every mode except fsr1rcas scales straight
    // into the output; the RCAS chain reads the source through one set and the
    // EASU result through another.
    VkDescriptorSet setDirect = ctx->allocSet(pool, setLayout, srcBuf, dstBuf);
    VkDescriptorSet setScale  = ctx->allocSet(pool, setLayout, srcBuf, scratchBuf);
    VkDescriptorSet setRcas   = ctx->allocSet(pool, setLayout, scratchBuf, dstBuf);

    VkCommandBuffer cmd = ctx->allocCmd(cmdPool);

    // Upload the source into device-local memory exactly once.
    {
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        VkBufferCopy region{0, 0, srcBytes};
        vkCmdCopyBuffer(cmd, srcStage.buf, srcBuf.buf, 1, &region);
        VkMemoryBarrier2 mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        mb.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
        vkEndCommandBuffer(cmd);
        ctx->submitAndWait(cmd);
    }

    auto record = [&](uint32_t mode) {
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = 0;  // reused across all bench iterations: NOT one-shot
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &bi);

        vkCmdResetQueryPool(cmd, queries, 0, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, queries, 0);

        // fsr1rcas is the only mode that is not one dispatch: EASU lands in the
        // scratch buffer, then RCAS sharpens it into the output. Both dispatches
        // sit inside the same timestamp pair, so the reported time is the whole
        // chain - which is what a real FSR1 upscale costs.
        const bool rcasChain = (mode == lsl::MODE_FSR1RCAS);
        const uint32_t groupsX = (dstW + 15) / 16, groupsY = (dstH + 15) / 16;

        lsl::Push p1{src.w, src.h, dstW, dstH,
                     rcasChain ? uint32_t(lsl::MODE_FSR1) : mode, 0.0f, 0, 0};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, scalePipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1,
                                rcasChain ? &setScale : &setDirect, 0, nullptr);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p1), &p1);
        vkCmdDispatch(cmd, groupsX, groupsY, 1);

        if (rcasChain) {
            // RCAS reads what the EASU dispatch just wrote.
            VkMemoryBarrier2 eb{};
            eb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            eb.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            eb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            eb.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            eb.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
            VkDependencyInfo ed{};
            ed.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            ed.memoryBarrierCount = 1;
            ed.pMemoryBarriers = &eb;
            if (!a.hazard) vkCmdPipelineBarrier2(cmd, &ed);

            lsl::Push p2{dstW, dstH, dstW, dstH, 0u, a.sharpness, 0u, 0u};
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rcasPipe);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1,
                                    &setRcas, 0, nullptr);
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p2), &p2);
            vkCmdDispatch(cmd, groupsX, groupsY, 1);
        }

        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, queries, 1);

        // hand the result over to the transfer stage for the final download
        VkMemoryBarrier2 mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        mb.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        mb.dstStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);

        vkEndCommandBuffer(cmd);
    };

    writeHeaderIfNeeded(a.csv);
    if (!a.quiet) {
        std::printf("%-9s %-9s %14s %14s %10s %10s %9s %8s\n",
                    "mode", "gpu", "dispatch_avg", "dispatch_p99",
                    quality ? "psnr_vs_gt" : "psnr_db", "ssim", "edge", "iters");
        if (quality)
            std::printf("(edge = mean |Laplacian| of the output, 0-255 scale; "
                        "ground truth is %.2f)\n", lsl::lapAbsRGB(truth));
    }

    int failures = 0;
    for (uint32_t mode : a.modes) {
        record(mode);

        // warm-up (shader upload, clock ramp) then timed loop
        double t = ctx->submitTimed(cmd, cmdPool, queries);
        (void)t;

        std::vector<double> times;
        times.reserve(size_t(a.bench));
        for (int i = 0; i < a.bench; ++i)
            times.push_back(ctx->submitTimed(cmd, cmdPool, queries));

        // download the result: dstLocal -> staging, then memcpy to the host
        {
            VkCommandBufferBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkResetCommandBuffer(cmd, 0);
            vkBeginCommandBuffer(cmd, &bi);
            VkBufferCopy region{0, 0, dstBytes};
            vkCmdCopyBuffer(cmd, dstBuf.buf, dstStage.buf, 1, &region);
            VkMemoryBarrier2 db{};
            db.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            db.srcStageMask  = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            db.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            db.dstStageMask  = VK_PIPELINE_STAGE_2_HOST_BIT;
            db.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dd{};
            dd.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dd.memoryBarrierCount = 1;
            dd.pMemoryBarriers = &db;
            vkCmdPipelineBarrier2(cmd, &dd);
            vkEndCommandBuffer(cmd);
            ctx->submitAndWait(cmd);
        }

        void* m = ctx->map(dstStage);
        lsl::Image got;
        got.w = dstW;
        got.h = dstH;
        got.rgba.resize(dstBytes);
        std::memcpy(got.rgba.data(), m, size_t(dstBytes));
        ctx->unmap(dstStage);

        // In quality mode the reference is the original image, so the (much
        // more expensive) CPU upscale is skipped entirely.
        lsl::Image ref;
        if (!quality) ref = lsl::upscaleCPU(src, dstW, dstH, mode, a.sharpness);
        const lsl::Image& cmp = quality ? truth : ref;
        const double psnr = lsl::psnrRGB(cmp, got);
        const double ssim = lsl::ssimRGB(cmp, got);
        const double edge = lsl::lapAbsRGB(got);

        const double avg = std::accumulate(times.begin(), times.end(), 0.0) /
                           double(times.size());
        const double p99 = percentile(times, 0.99);

        if (!quality) {
            if (mode == lsl::MODE_NEAREST && psnr < 90.0) ++failures;  // exact
            if (mode != lsl::MODE_NEAREST && psnr < 45.0) ++failures;
        } else if (psnr < 15.0) {
            ++failures;  // below this the output is not an upscale at all
        }

        std::printf("%-9s %-9s %11.4f ms %11.4f ms %10.2f %8.4f %9.2f %8d\n",
                    lsl::modeName(mode), gpuVendor.c_str(), avg, p99, psnr, ssim,
                    edge, a.bench);

        if (!a.csv.empty()) {
            FILE* f = std::fopen(a.csv.c_str(), "a");
            if (f) {
                std::fprintf(f, "\"%s\",%s,%s,%ux%u,%ux%u,%.6f,%.6f,%.3f,%.6f,%.3f,%d\n",
                             gpuName.c_str(), gpuVendor.c_str(), lsl::modeName(mode),
                             src.w, src.h, dstW, dstH, avg, p99, psnr, ssim, edge,
                             a.bench);
                std::fclose(f);
            }
        }

        if (!a.out.empty() && mode == a.modes.back())
            lsl::savePPM(a.out, got);
    }

    // teardown
    vkDestroyQueryPool(ctx->device(), queries, nullptr);
    vkDestroyCommandPool(ctx->device(), cmdPool, nullptr);
    vkDestroyDescriptorPool(ctx->device(), pool, nullptr);
    vkDestroyPipeline(ctx->device(), scalePipe, nullptr);
    vkDestroyPipeline(ctx->device(), rcasPipe, nullptr);
    vkDestroyPipelineLayout(ctx->device(), layout, nullptr);
    vkDestroyDescriptorSetLayout(ctx->device(), setLayout, nullptr);
    vkDestroyShaderModule(ctx->device(), scaleShader, nullptr);
    vkDestroyShaderModule(ctx->device(), rcasShader, nullptr);
    ctx->destroyBuffer(srcBuf);
    ctx->destroyBuffer(dstBuf);
    ctx->destroyBuffer(scratchBuf);
    ctx->destroyBuffer(srcStage);
    ctx->destroyBuffer(dstStage);

    if (failures) {
        std::cerr << "\nFAILED: " << failures
                  << " mode(s) fell below the quality gate "
                     "(nearest must be exact, others >= 45 dB)\n";
        return 1;
    }
    if (!a.quiet) std::cout << "\nall modes passed the quality gate\n";
    return 0;
}
