#include <windows.h>
#include <nvapi.h>

#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "nvapi64.lib")

namespace {

std::string NvError(NvAPI_Status status)
{
    NvAPI_ShortString text{};
    if (NvAPI_GetErrorMessage(status, text) == NVAPI_OK)
        return text;
    return "NVAPI error " + std::to_string(static_cast<int>(status));
}

const char* ScalingName(NV_SCALING scaling)
{
    switch (scaling) {
    case NV_SCALING_DEFAULT:
        return "default / no change";
    case NV_SCALING_GPU_SCALING_TO_CLOSEST:
        return "balanced full-screen";
    case NV_SCALING_GPU_SCALING_TO_NATIVE:
        return "FORCE GPU full-screen";
    case NV_SCALING_GPU_SCANOUT_TO_NATIVE:
        return "FORCE GPU centered / no scaling";
    case NV_SCALING_GPU_SCALING_TO_ASPECT_SCANOUT_TO_NATIVE:
        return "FORCE GPU aspect ratio";
    case NV_SCALING_GPU_SCALING_TO_ASPECT_SCANOUT_TO_CLOSEST:
        return "balanced aspect ratio";
    case NV_SCALING_GPU_SCANOUT_TO_CLOSEST:
        return "balanced centered / no scaling";
    case NV_SCALING_GPU_INTEGER_ASPECT_SCALING:
        return "FORCE GPU integer scaling";
    case NV_SCALING_CUSTOMIZED:
        return "customized";
    default:
        return "unknown";
    }
}

struct DisplayConfig {
    NvU32 pathCount = 0;
    std::vector<NV_DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<std::unique_ptr<NV_DISPLAYCONFIG_SOURCE_MODE_INFO>> sources;
    std::vector<std::unique_ptr<NV_DISPLAYCONFIG_PATH_TARGET_INFO[]>> targets;
    std::vector<std::vector<std::unique_ptr<NV_DISPLAYCONFIG_PATH_ADVANCED_TARGET_INFO>>> details;
};

NvAPI_Status LoadDisplayConfig(DisplayConfig& cfg)
{
    cfg = {};

    NvU32 pathCount = 0;
    NvAPI_Status st = NvAPI_DISP_GetDisplayConfig(&pathCount, nullptr);
    if (st != NVAPI_OK)
        return st;
    if (pathCount == 0)
        return NVAPI_NVIDIA_DEVICE_NOT_FOUND;

    cfg.paths.resize(pathCount);
    for (auto& path : cfg.paths) {
        std::memset(&path, 0, sizeof(path));
        path.version = NV_DISPLAYCONFIG_PATH_INFO_VER;
    }

    // First populated call gives us targetInfoCount for each path.
    NvU32 count = pathCount;
    st = NvAPI_DISP_GetDisplayConfig(&count, cfg.paths.data());
    if (st != NVAPI_OK)
        return st;

    // A topology change between the two calls is rare; fail cleanly instead of
    // risking undersized allocations.
    if (count != pathCount)
        return NVAPI_ERROR;

    cfg.pathCount = count;
    cfg.sources.resize(count);
    cfg.targets.resize(count);
    cfg.details.resize(count);

    for (NvU32 i = 0; i < count; ++i) {
        auto& path = cfg.paths[i];

        // Current public NVAPI uses PATH_INFO V2: one source-mode structure per path.
        cfg.sources[i] = std::make_unique<NV_DISPLAYCONFIG_SOURCE_MODE_INFO>();
        std::memset(cfg.sources[i].get(), 0, sizeof(NV_DISPLAYCONFIG_SOURCE_MODE_INFO));
        path.sourceModeInfo = cfg.sources[i].get();

        if (path.targetInfoCount == 0)
            continue;

        cfg.targets[i] = std::make_unique<NV_DISPLAYCONFIG_PATH_TARGET_INFO[]>(path.targetInfoCount);
        std::memset(cfg.targets[i].get(), 0,
                    sizeof(NV_DISPLAYCONFIG_PATH_TARGET_INFO) * path.targetInfoCount);
        path.targetInfo = cfg.targets[i].get();

        cfg.details[i].resize(path.targetInfoCount);
        for (NvU32 j = 0; j < path.targetInfoCount; ++j) {
            cfg.details[i][j] = std::make_unique<NV_DISPLAYCONFIG_PATH_ADVANCED_TARGET_INFO>();
            std::memset(cfg.details[i][j].get(), 0,
                        sizeof(NV_DISPLAYCONFIG_PATH_ADVANCED_TARGET_INFO));
            cfg.details[i][j]->version = NV_DISPLAYCONFIG_PATH_ADVANCED_TARGET_INFO_VER;
            path.targetInfo[j].details = cfg.details[i][j].get();
        }
    }

    // Second populated call fills source modes, display IDs and advanced target details.
    count = cfg.pathCount;
    st = NvAPI_DISP_GetDisplayConfig(&count, cfg.paths.data());
    if (st != NVAPI_OK)
        return st;
    if (count != cfg.pathCount)
        return NVAPI_ERROR;

    return NVAPI_OK;
}

NV_DISPLAYCONFIG_PATH_TARGET_INFO* FindTarget(DisplayConfig& cfg, NvU32 displayId)
{
    for (auto& path : cfg.paths) {
        for (NvU32 j = 0; j < path.targetInfoCount; ++j) {
            if (path.targetInfo[j].displayId == displayId)
                return &path.targetInfo[j];
        }
    }
    return nullptr;
}

const NV_DISPLAYCONFIG_PATH_TARGET_INFO* FindTarget(const DisplayConfig& cfg, NvU32 displayId)
{
    for (const auto& path : cfg.paths) {
        for (NvU32 j = 0; j < path.targetInfoCount; ++j) {
            if (path.targetInfo[j].displayId == displayId)
                return &path.targetInfo[j];
        }
    }
    return nullptr;
}

NvAPI_Status GetPrimaryDisplayId(NvU32& displayId)
{
    return NvAPI_DISP_GetGDIPrimaryDisplayId(&displayId);
}

void PrintTarget(NvU32 displayId,
                 const NV_DISPLAYCONFIG_PATH_ADVANCED_TARGET_INFO* d,
                 bool primary)
{
    std::cout << (primary ? "* " : "  ")
              << "displayId=0x" << std::hex << std::uppercase << displayId
              << std::dec;

    if (!d) {
        std::cout << "  details=<unavailable>\n";
        return;
    }

    std::cout << "  scaling=" << static_cast<int>(d->scaling)
              << " (" << ScalingName(d->scaling) << ")"
              << "  refresh=" << (d->refreshRate1K / 1000.0) << " Hz\n";
}

void ListDisplays(const DisplayConfig& cfg, NvU32 primaryId)
{
    for (const auto& path : cfg.paths) {
        for (NvU32 j = 0; j < path.targetInfoCount; ++j) {
            const auto& t = path.targetInfo[j];
            PrintTarget(t.displayId, t.details, t.displayId == primaryId);
        }
    }
}

bool ParseDisplayId(const char* text, NvU32& out)
{
    if (!text || !*text)
        return false;

    char* end = nullptr;
    unsigned long value = std::strtoul(text, &end, 0); // accepts decimal or 0xHEX
    if (end == text || *end != '\0')
        return false;

    out = static_cast<NvU32>(value);
    return true;
}

bool ParseScaling(const std::string& command, NV_SCALING& scaling)
{
    if (command == "fullscreen") {
        scaling = NV_SCALING_GPU_SCALING_TO_NATIVE;
        return true;
    }
    if (command == "aspect") {
        scaling = NV_SCALING_GPU_SCALING_TO_ASPECT_SCANOUT_TO_NATIVE;
        return true;
    }
    if (command == "centered") {
        scaling = NV_SCALING_GPU_SCANOUT_TO_NATIVE;
        return true;
    }
    if (command == "integer") {
        scaling = NV_SCALING_GPU_INTEGER_ASPECT_SCALING;
        return true;
    }
    if (command == "balanced-fullscreen") {
        scaling = NV_SCALING_GPU_SCALING_TO_CLOSEST;
        return true;
    }
    if (command == "balanced-aspect") {
        scaling = NV_SCALING_GPU_SCALING_TO_ASPECT_SCANOUT_TO_CLOSEST;
        return true;
    }
    return false;
}

void Usage()
{
    std::cout <<
        "nvscale - NVIDIA scaling control without NVCP\n\n"
        "Usage:\n"
        "  nvscale list\n"
        "  nvscale status [displayId]\n"
        "  nvscale fullscreen [displayId]\n"
        "  nvscale aspect [displayId]\n"
        "  nvscale centered [displayId]\n"
        "  nvscale integer [displayId]\n"
        "  nvscale balanced-fullscreen [displayId]\n"
        "  nvscale balanced-aspect [displayId]\n\n"
        "If displayId is omitted, the Windows GDI primary display is used.\n"
        "displayId may be decimal or 0xHEX.\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        Usage();
        return 2;
    }

    NvAPI_Status st = NvAPI_Initialize();
    if (st != NVAPI_OK) {
        std::cerr << "NvAPI_Initialize failed: " << NvError(st) << "\n";
        return 1;
    }

    struct NvApiGuard {
        ~NvApiGuard() { NvAPI_Unload(); }
    } guard;

    DisplayConfig cfg;
    st = LoadDisplayConfig(cfg);
    if (st != NVAPI_OK) {
        std::cerr << "NvAPI_DISP_GetDisplayConfig failed: " << NvError(st) << "\n";
        return 1;
    }

    NvU32 primaryId = 0;
    st = GetPrimaryDisplayId(primaryId);
    if (st != NVAPI_OK) {
        std::cerr << "NvAPI_DISP_GetGDIPrimaryDisplayId failed: " << NvError(st)
                  << "\nThe Windows primary display may not be attached to an NVIDIA GPU.\n";
        return 1;
    }

    const std::string command = argv[1];

    if (command == "list") {
        ListDisplays(cfg, primaryId);
        return 0;
    }

    NvU32 displayId = primaryId;
    if (argc >= 3 && !ParseDisplayId(argv[2], displayId)) {
        std::cerr << "Invalid displayId: " << argv[2] << "\n";
        return 2;
    }

    auto* target = FindTarget(cfg, displayId);
    if (!target || !target->details) {
        std::cerr << "Display 0x" << std::hex << std::uppercase << displayId
                  << std::dec << " was not found in the NVIDIA display configuration.\n";
        return 1;
    }

    if (command == "status") {
        PrintTarget(displayId, target->details, displayId == primaryId);
        return 0;
    }

    NV_SCALING requested{};
    if (!ParseScaling(command, requested)) {
        Usage();
        return 2;
    }

    const NV_SCALING oldScaling = target->details->scaling;
    target->details->scaling = requested;

    // Validate the complete topology first. We only changed one scaling field.
    st = NvAPI_DISP_SetDisplayConfig(
        cfg.pathCount,
        cfg.paths.data(),
        NV_DISPLAYCONFIG_VALIDATE_ONLY);

    if (st != NVAPI_OK) {
        std::cerr << "Validation failed: " << NvError(st) << "\n";
        return 1;
    }

    // Apply and persist. DRIVER_RELOAD_ALLOWED lets NVAPI reload the display driver
    // if that is required to commit the requested configuration.
    const NvU32 flags =
        NV_DISPLAYCONFIG_SAVE_TO_PERSISTENCE |
        NV_DISPLAYCONFIG_DRIVER_RELOAD_ALLOWED;

    st = NvAPI_DISP_SetDisplayConfig(cfg.pathCount, cfg.paths.data(), flags);
    if (st != NVAPI_OK) {
        std::cerr << "Apply failed: " << NvError(st) << "\n";
        return 1;
    }

    std::cout << "Requested display 0x" << std::hex << std::uppercase << displayId
              << std::dec << ": " << ScalingName(oldScaling)
              << " -> " << ScalingName(requested) << "\n";

    // Re-read from the driver and verify the value it currently reports.
    DisplayConfig verify;
    st = LoadDisplayConfig(verify);
    if (st != NVAPI_OK) {
        std::cerr << "Applied, but verification read failed: " << NvError(st) << "\n";
        return 1;
    }

    const auto* verified = FindTarget(verify, displayId);
    if (!verified || !verified->details) {
        std::cerr << "Applied, but the display disappeared during verification.\n";
        return 1;
    }

    std::cout << "Driver reports: scaling="
              << static_cast<int>(verified->details->scaling)
              << " (" << ScalingName(verified->details->scaling) << ")\n";

    if (verified->details->scaling != requested) {
        std::cerr << "Warning: driver did not report the requested scaling mode after apply.\n";
        return 3;
    }

    return 0;
}
