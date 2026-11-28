/*
 * Project Tourline 5 — NVIDIA DLSS / DLAA D3D12 Plug-in (pt5_upscale_dlss.dll)
 *
 * Implements the pure-C ABI in rsx_upscale_plugin.h on top of the NVIDIA NGX
 * DLSS SDK. Built as a standalone DLL so gt5.exe has zero compile-time or
 * link-time dependency on proprietary NVIDIA libraries.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#ifndef _NTDEF_
typedef LONG NTSTATUS;
#endif
#include <d3dkmthk.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_defs.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_dlssg_d3d.h"
#include "rsx_upscale_plugin.h"

#define PT5_EXPORT extern "C" __declspec(dllexport)

/* Custom Project ID GUID for Project Tourline 5 (NGX_ENGINE_TYPE_CUSTOM). */
static const char* const kProjectId    = "8F3A4D61-7B2C-4E19-9A5F-0D4C1E72B501";
static const char* const kEngineVer    = "0.3.3";

static bool                 s_inited   = false;
static ID3D12Device*        s_device   = nullptr;
static wchar_t              s_dll_dir[MAX_PATH] = L"";
static NVSDK_NGX_Parameter* s_cap_pars = nullptr;
static NVSDK_NGX_Parameter* s_eval_pars = nullptr;
static NVSDK_NGX_Handle*    s_feature  = nullptr;

/* Frame Generation (DLSS-G) state. */
static NVSDK_NGX_Parameter* s_fg_pars    = nullptr;
static NVSDK_NGX_Handle*    s_fg_feature = nullptr;
static uint32_t             s_fg_out_w   = 0, s_fg_out_h = 0;
static uint32_t             s_fg_ren_w   = 0, s_fg_ren_h = 0;
static uint32_t             s_fg_max_gen = 1;
static int32_t              s_fg_hdr     = 0;
static int32_t              s_fg_dinv    = 0;

static void set_reason(char* out, size_t cap, const char* msg)
{
    if (!out || cap == 0) return;
    snprintf(out, cap, "%s", msg ? msg : "");
}

static const char* ngx_result_name(NVSDK_NGX_Result r)
{
    switch (r) {
    case NVSDK_NGX_Result_Success:                          return "Success";
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported:         return "FeatureNotSupported";
    case NVSDK_NGX_Result_FAIL_PlatformError:               return "PlatformError";
    case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists:        return "FeatureAlreadyExists";
    case NVSDK_NGX_Result_FAIL_FeatureNotFound:             return "FeatureNotFound";
    case NVSDK_NGX_Result_FAIL_InvalidParameter:            return "InvalidParameter";
    case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall:       return "ScratchBufferTooSmall";
    case NVSDK_NGX_Result_FAIL_NotInitialized:              return "NotInitialized";
    case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat:      return "UnsupportedInputFormat";
    case NVSDK_NGX_Result_FAIL_RWFlagMissing:               return "RWFlagMissing";
    case NVSDK_NGX_Result_FAIL_MissingInput:                return "MissingInput";
    case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature:   return "UnableToInitializeFeature";
    case NVSDK_NGX_Result_FAIL_OutOfDate:                   return "OutOfDate";
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:              return "OutOfGPUMemory";
    case NVSDK_NGX_Result_FAIL_UnsupportedFormat:           return "UnsupportedFormat";
    case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath:  return "UnableToWriteToAppDataPath";
    case NVSDK_NGX_Result_FAIL_UnsupportedParameter:        return "UnsupportedParameter";
    case NVSDK_NGX_Result_FAIL_Denied:                      return "Denied";
    case NVSDK_NGX_Result_FAIL_NotImplemented:              return "NotImplemented";
    default:                                                return "Fail";
    }
}

static NVSDK_NGX_PerfQuality_Value map_quality(int32_t mode)
{
    switch (mode) {
    case PT5_UPSCALE_DLAA:       return NVSDK_NGX_PerfQuality_Value_DLAA;
    case PT5_UPSCALE_QUALITY:    return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case PT5_UPSCALE_BALANCED:   return NVSDK_NGX_PerfQuality_Value_Balanced;
    case PT5_UPSCALE_PERF:       return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case PT5_UPSCALE_ULTRA_PERF: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    case PT5_UPSCALE_CUSTOM:     return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    default:                     return NVSDK_NGX_PerfQuality_Value_DLAA;
    }
}

static const char* preset_param_for_mode(int32_t mode)
{
    switch (mode) {
    case PT5_UPSCALE_DLAA:       return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA;
    case PT5_UPSCALE_QUALITY:    return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality;
    case PT5_UPSCALE_BALANCED:   return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced;
    case PT5_UPSCALE_PERF:       return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance;
    case PT5_UPSCALE_ULTRA_PERF: return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance;
    case PT5_UPSCALE_CUSTOM:     return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality;
    default:                     return NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA;
    }
}

PT5_EXPORT uint32_t pt5_upscale_abi_version(void)
{
    return PT5_UPSCALE_ABI_VERSION;
}

PT5_EXPORT int pt5_upscale_init(void* d3d12_device,
                                const wchar_t* data_dir,
                                const wchar_t* dll_dir,
                                char* reason_out, size_t reason_cap)
{
    if (s_inited) return 1;
    if (!d3d12_device) {
        set_reason(reason_out, reason_cap, "Null D3D12 device");
        return 0;
    }
    ID3D12Device* dev = static_cast<ID3D12Device*>(d3d12_device);

    const wchar_t* app_path = (data_dir && data_dir[0]) ? data_dir : L".";

    NVSDK_NGX_FeatureCommonInfo common = {};
    const wchar_t* paths[2] = { nullptr, nullptr };
    unsigned int npaths = 0;
    if (dll_dir && dll_dir[0]) paths[npaths++] = dll_dir;
    if (npaths > 0) {
        common.PathListInfo.Path   = paths;
        common.PathListInfo.Length = npaths;
    }
    common.LoggingInfo.LoggingCallback          = nullptr;
    common.LoggingInfo.MinimumLoggingLevel      = NVSDK_NGX_LOGGING_LEVEL_OFF;
    common.LoggingInfo.DisableOtherLoggingSinks = false;

    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(
        kProjectId,
        NVSDK_NGX_ENGINE_TYPE_CUSTOM,
        kEngineVer,
        app_path,
        dev,
        &common,
        NVSDK_NGX_Version_API);

    if (NVSDK_NGX_FAILED(r)) {
        char buf[192];
        if (r == NVSDK_NGX_Result_FAIL_FeatureNotSupported ||
            r == NVSDK_NGX_Result_FAIL_PlatformError) {
            snprintf(buf, sizeof buf, "NVIDIA RTX GPU or NGX driver not available (%s 0x%08X)",
                     ngx_result_name(r), (unsigned)r);
        } else {
            snprintf(buf, sizeof buf, "NGX init failed (%s 0x%08X)",
                     ngx_result_name(r), (unsigned)r);
        }
        set_reason(reason_out, reason_cap, buf);
        return 0;
    }

    r = NVSDK_NGX_D3D12_GetCapabilityParameters(&s_cap_pars);
    if (NVSDK_NGX_FAILED(r) || !s_cap_pars) {
        char buf[160];
        snprintf(buf, sizeof buf, "NGX GetCapabilityParameters failed (%s 0x%08X)",
                 ngx_result_name(r), (unsigned)r);
        set_reason(reason_out, reason_cap, buf);
        NVSDK_NGX_D3D12_Shutdown1(dev);
        return 0;
    }

    int needs_driver = 0;
    unsigned int min_maj = 0, min_min = 0;
    NVSDK_NGX_Result r_drv = s_cap_pars->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    if (NVSDK_NGX_SUCCEED(r_drv) && needs_driver) {
        s_cap_pars->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &min_maj);
        s_cap_pars->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &min_min);
        char buf[160];
        snprintf(buf, sizeof buf, "NVIDIA driver too old for DLSS (need >= %u.%u)", min_maj, min_min);
        set_reason(reason_out, reason_cap, buf);
        NVSDK_NGX_D3D12_DestroyParameters(s_cap_pars);
        s_cap_pars = nullptr;
        NVSDK_NGX_D3D12_Shutdown1(dev);
        return 0;
    }

    int dlss_avail = 0;
    r = s_cap_pars->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &dlss_avail);
    if (NVSDK_NGX_FAILED(r) || !dlss_avail) {
        NVSDK_NGX_Result init_res = NVSDK_NGX_Result_Fail;
        s_cap_pars->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, (int*)&init_res);
        char buf[192];
        snprintf(buf, sizeof buf, "DLSS not available on this GPU/install (nvngx_dlss.dll missing or non-RTX; 0x%08X)",
                 (unsigned)init_res);
        set_reason(reason_out, reason_cap, buf);
        NVSDK_NGX_D3D12_DestroyParameters(s_cap_pars);
        s_cap_pars = nullptr;
        NVSDK_NGX_D3D12_Shutdown1(dev);
        return 0;
    }

    r = NVSDK_NGX_D3D12_AllocateParameters(&s_eval_pars);
    if (NVSDK_NGX_FAILED(r) || !s_eval_pars) {
        char buf[160];
        snprintf(buf, sizeof buf, "NGX AllocateParameters failed (%s 0x%08X)",
                 ngx_result_name(r), (unsigned)r);
        set_reason(reason_out, reason_cap, buf);
        NVSDK_NGX_D3D12_DestroyParameters(s_cap_pars);
        s_cap_pars = nullptr;
        NVSDK_NGX_D3D12_Shutdown1(dev);
        return 0;
    }

    s_inited = true;
    s_device = dev;
    if (dll_dir && dll_dir[0]) {
        wcsncpy(s_dll_dir, dll_dir, MAX_PATH - 1);
        s_dll_dir[MAX_PATH - 1] = L'\0';
    }
    set_reason(reason_out, reason_cap, "OK");
    return 1;
}

PT5_EXPORT int pt5_upscale_optimal(uint32_t out_w, uint32_t out_h, int32_t mode,
                                   uint32_t* render_w, uint32_t* render_h)
{
    if (!s_inited || !s_cap_pars || !render_w || !render_h || !out_w || !out_h) return 0;
    if (mode == PT5_UPSCALE_DLAA) {
        *render_w = out_w;
        *render_h = out_h;
        return 1;
    }
    unsigned int opt_w = 0, opt_h = 0, max_w = 0, max_h = 0, min_w = 0, min_h = 0;
    float sharpness = 0.0f;
    NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(
        s_cap_pars, out_w, out_h, map_quality(mode),
        &opt_w, &opt_h, &max_w, &max_h, &min_w, &min_h, &sharpness);
    if (NVSDK_NGX_FAILED(r) || !opt_w || !opt_h) return 0;
    *render_w = opt_w;
    *render_h = opt_h;
    return 1;
}

PT5_EXPORT void pt5_upscale_destroy_feature(void)
{
    if (s_feature) {
        NVSDK_NGX_D3D12_ReleaseFeature(s_feature);
        s_feature = nullptr;
    }
}

PT5_EXPORT int pt5_upscale_create(void* d3d12_cmd_list,
                                  const Pt5UpscaleCreate* cfg,
                                  char* reason_out, size_t reason_cap)
{
    if (!s_inited || !s_eval_pars || !d3d12_cmd_list || !cfg) {
        set_reason(reason_out, reason_cap, "Not initialized or invalid arguments");
        return 0;
    }
    if (cfg->struct_size < sizeof(Pt5UpscaleCreate)) {
        set_reason(reason_out, reason_cap, "ABI struct_size mismatch");
        return 0;
    }
    pt5_upscale_destroy_feature();

    ID3D12GraphicsCommandList* cl = static_cast<ID3D12GraphicsCommandList*>(d3d12_cmd_list);

    int flags = NVSDK_NGX_DLSS_Feature_Flags_None;
    if (cfg->is_hdr)         flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (cfg->mv_low_res)     flags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (cfg->depth_inverted) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (cfg->auto_exposure)  flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

    if (cfg->preset > 0) {
        s_eval_pars->Set(preset_param_for_mode(cfg->mode), (unsigned int)cfg->preset);
    } else {
        s_eval_pars->Set(preset_param_for_mode(cfg->mode),
                         (unsigned int)NVSDK_NGX_DLSS_Hint_Render_Preset_Default);
    }
    s_eval_pars->Set(NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);

    NVSDK_NGX_DLSS_Create_Params cp = {};
    cp.Feature.InWidth            = cfg->in_w;
    cp.Feature.InHeight           = cfg->in_h;
    cp.Feature.InTargetWidth      = cfg->out_w;
    cp.Feature.InTargetHeight     = cfg->out_h;
    cp.Feature.InPerfQualityValue = map_quality(cfg->mode);
    cp.InFeatureCreateFlags       = flags;
    cp.InEnableOutputSubrects     = false;

    NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(cl, 1, 1, &s_feature, s_eval_pars, &cp);
    if (NVSDK_NGX_FAILED(r) || !s_feature) {
        char buf[192];
        snprintf(buf, sizeof buf, "NGX_D3D12_CREATE_DLSS_EXT %ux%u->%ux%u failed (%s 0x%08X)",
                 cfg->in_w, cfg->in_h, cfg->out_w, cfg->out_h,
                 ngx_result_name(r), (unsigned)r);
        set_reason(reason_out, reason_cap, buf);
        s_feature = nullptr;
        return 0;
    }
    set_reason(reason_out, reason_cap, "OK");
    return 1;
}

PT5_EXPORT int pt5_upscale_eval(void* d3d12_cmd_list,
                                const Pt5UpscaleEval* ev,
                                char* reason_out, size_t reason_cap)
{
    if (!s_inited || !s_feature || !s_eval_pars || !d3d12_cmd_list || !ev) {
        set_reason(reason_out, reason_cap, "DLSS feature not created");
        return 0;
    }
    ID3D12GraphicsCommandList* cl = static_cast<ID3D12GraphicsCommandList*>(d3d12_cmd_list);

    NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor               = static_cast<ID3D12Resource*>(ev->color);
    ep.Feature.pInOutput              = static_cast<ID3D12Resource*>(ev->output);
    ep.pInDepth                       = static_cast<ID3D12Resource*>(ev->depth);
    ep.pInMotionVectors               = static_cast<ID3D12Resource*>(ev->mv);
    ep.InJitterOffsetX                = ev->jitter_x;
    ep.InJitterOffsetY                = ev->jitter_y;
    ep.InRenderSubrectDimensions.Width  = ev->render_w;
    ep.InRenderSubrectDimensions.Height = ev->render_h;
    ep.InReset                        = ev->reset ? 1 : 0;
    ep.InMVScaleX                     = ev->mv_scale_x != 0.0f ? ev->mv_scale_x : 1.0f;
    ep.InMVScaleY                     = ev->mv_scale_y != 0.0f ? ev->mv_scale_y : 1.0f;
    ep.InFrameTimeDeltaInMsec         = ev->frame_dt_ms;

    NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(cl, s_feature, s_eval_pars, &ep);
    if (NVSDK_NGX_FAILED(r)) {
        char buf[160];
        snprintf(buf, sizeof buf, "NGX_D3D12_EVALUATE_DLSS_EXT failed (%s 0x%08X)",
                 ngx_result_name(r), (unsigned)r);
        set_reason(reason_out, reason_cap, buf);
        return 0;
    }
    return 1;
}

static int fg_hags_enabled(void)
{
    if (!s_device) return -1;
    D3DKMT_OPENADAPTERFROMLUID oa = {};
    oa.AdapterLuid = s_device->GetAdapterLuid();
    if (D3DKMTOpenAdapterFromLuid(&oa) != 0) return -1;
    D3DKMT_WDDM_2_7_CAPS wc = {};
    D3DKMT_QUERYADAPTERINFO qa = {};
    qa.hAdapter = oa.hAdapter;
    qa.Type = KMTQAITYPE_WDDM_2_7_CAPS;
    qa.pPrivateDriverData = &wc;
    qa.PrivateDriverDataSize = sizeof wc;
    const NTSTATUS ks = D3DKMTQueryAdapterInfo(&qa);
    D3DKMT_CLOSEADAPTER ca = {};
    ca.hAdapter = oa.hAdapter;
    D3DKMTCloseAdapter(&ca);
    return ks == 0 ? (wc.HwSchEnabled ? 1 : 0) : -1;
}

static bool fg_has_dlssg_dll(void)
{
    wchar_t path[MAX_PATH + 32];
    if (s_dll_dir[0]) {
        _snwprintf(path, MAX_PATH + 31, L"%ls\\nvngx_dlssg.dll", s_dll_dir);
        path[MAX_PATH + 31] = L'\0';
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return true;
    }
    return GetFileAttributesW(L"nvngx_dlssg.dll") != INVALID_FILE_ATTRIBUTES;
}

PT5_EXPORT int pt5_fg_caps(Pt5FgCaps* caps)
{
    if (!caps || caps->struct_size < sizeof(Pt5FgCaps)) return 0;
    const uint32_t sz = caps->struct_size;
    memset(caps, 0, sizeof *caps);
    caps->struct_size = sz;
    caps->max_generated = 1;
    if (!s_inited || !s_cap_pars) {
        snprintf(caps->message, sizeof caps->message, "NGX not initialized");
        return 1;
    }
    if (!fg_has_dlssg_dll()) {
        snprintf(caps->message, sizeof caps->message, "nvngx_dlssg.dll not found next to executable");
        return 1;
    }
    int avail = 0, need = 0, init_res = 0;
    unsigned int maj = 0, min = 0, multi = 0;
    s_cap_pars->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &avail);
    s_cap_pars->Get(NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &need);
    s_cap_pars->Get(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor, &maj);
    s_cap_pars->Get(NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor, &min);
    s_cap_pars->Get(NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &init_res);
    if (NVSDK_NGX_SUCCEED(s_cap_pars->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &multi)) && multi > 1)
        caps->max_generated = multi > 3 ? 3 : multi;
    caps->available = avail != 0 ? 1 : 0;
    caps->needs_driver = need != 0 ? 1 : 0;
    caps->min_driver_major = maj;
    caps->min_driver_minor = min;
    if (!avail) {
        if (need) {
            snprintf(caps->message, sizeof caps->message, "DLSS Frame Generation needs NVIDIA driver >= %u.%u", maj, min);
        } else if ((NVSDK_NGX_Result)init_res == NVSDK_NGX_Result_FAIL_FeatureNotSupported) {
            snprintf(caps->message, sizeof caps->message, "GPU does not support DLSS Frame Generation (requires RTX 40+)");
        } else if (fg_hags_enabled() == 0) {
            snprintf(caps->message, sizeof caps->message, "Enable Hardware-Accelerated GPU Scheduling in Windows Graphics Settings");
        } else {
            snprintf(caps->message, sizeof caps->message, "DLSS Frame Generation unavailable (%s 0x%08X)",
                     ngx_result_name((NVSDK_NGX_Result)init_res), (unsigned)init_res);
        }
    } else {
        snprintf(caps->message, sizeof caps->message, "OK (max %ux)", caps->max_generated + 1u);
    }
    return 1;
}

PT5_EXPORT void pt5_fg_destroy(void)
{
    if (s_fg_feature) {
        NVSDK_NGX_D3D12_ReleaseFeature(s_fg_feature);
        s_fg_feature = nullptr;
    }
    if (s_fg_pars) {
        NVSDK_NGX_D3D12_DestroyParameters(s_fg_pars);
        s_fg_pars = nullptr;
    }
    s_fg_out_w = s_fg_out_h = s_fg_ren_w = s_fg_ren_h = 0;
    s_fg_max_gen = 1;
}

PT5_EXPORT int pt5_fg_create(void* d3d12_cmd_list, const Pt5FgCreate* cfg,
                             char* reason_out, size_t reason_cap)
{
    if (!s_inited || !s_cap_pars || !d3d12_cmd_list || !cfg) {
        set_reason(reason_out, reason_cap, "Not initialized or invalid arguments");
        return 0;
    }
    if (cfg->struct_size < sizeof(Pt5FgCreate)) {
        set_reason(reason_out, reason_cap, "FG ABI struct_size mismatch");
        return 0;
    }
    pt5_fg_destroy();

    ID3D12GraphicsCommandList* cl = static_cast<ID3D12GraphicsCommandList*>(d3d12_cmd_list);
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_AllocateParameters(&s_fg_pars);
    if (NVSDK_NGX_FAILED(r) || !s_fg_pars) {
        char buf[160];
        snprintf(buf, sizeof buf, "FG AllocateParameters failed (%s 0x%08X)", ngx_result_name(r), (unsigned)r);
        set_reason(reason_out, reason_cap, buf);
        return 0;
    }

    const unsigned always = NVSDK_NGX_DLSSG_ResourceFlags_Backbuffer |
                            NVSDK_NGX_DLSSG_ResourceFlags_MVecs |
                            NVSDK_NGX_DLSSG_ResourceFlags_Depth |
                            NVSDK_NGX_DLSSG_ResourceFlags_OutputInterpolated;
    const unsigned never  = NVSDK_NGX_DLSSG_ResourceFlags_UI |
                            NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha |
                            NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField |
                            NVSDK_NGX_DLSSG_ResourceFlags_OutputReal |
                            NVSDK_NGX_DLSSG_ResourceFlags_OutputDisableInterpolation;
    NVSDK_NGX_Parameter_SetUI(s_fg_pars, NVSDK_NGX_DLSSG_Parameter_ResourceAlwaysProvided_Flags, always);
    NVSDK_NGX_Parameter_SetUI(s_fg_pars, NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags, never);
    NVSDK_NGX_Parameter_SetUI(s_fg_pars, NVSDK_NGX_DLSSG_Parameter_FullscreenMode, NVSDK_NGX_DLSSG_FullscreenMode_Windowed);
    NVSDK_NGX_Parameter_SetUI(s_fg_pars, NVSDK_NGX_DLSSG_Parameter_AsyncCreateEnabled, 0);
    NVSDK_NGX_Parameter_SetUI(s_fg_pars, NVSDK_NGX_DLSSG_Parameter_UserInterfaceRecompositionEnabled, 0);
    NVSDK_NGX_Parameter_SetUI(s_fg_pars, NVSDK_NGX_DLSSG_Parameter_Width, cfg->out_w);
    NVSDK_NGX_Parameter_SetUI(s_fg_pars, NVSDK_NGX_DLSSG_Parameter_Height, cfg->out_h);

    NVSDK_NGX_DLSSG_Create_Params cp = {};
    cp.Width = cfg->out_w;
    cp.Height = cfg->out_h;
    cp.NativeBackbufferFormat = (unsigned)DXGI_FORMAT_R8G8B8A8_UNORM;
    cp.RenderWidth = cfg->render_w;
    cp.RenderHeight = cfg->render_h;
    cp.DynamicResolutionScaling = false;

    r = NGX_D3D12_CREATE_DLSSG(cl, 1, 1, &s_fg_feature, s_fg_pars, &cp);
    if (NVSDK_NGX_FAILED(r) || !s_fg_feature) {
        char buf[192];
        snprintf(buf, sizeof buf, "NGX_D3D12_CREATE_DLSSG %ux%u (ren %ux%u) failed (%s 0x%08X)",
                 cfg->out_w, cfg->out_h, cfg->render_w, cfg->render_h,
                 ngx_result_name(r), (unsigned)r);
        set_reason(reason_out, reason_cap, buf);
        pt5_fg_destroy();
        return 0;
    }
    s_fg_out_w = cfg->out_w;
    s_fg_out_h = cfg->out_h;
    s_fg_ren_w = cfg->render_w;
    s_fg_ren_h = cfg->render_h;
    s_fg_max_gen = cfg->max_generated < 1 ? 1 : (cfg->max_generated > 3 ? 3 : cfg->max_generated);
    s_fg_hdr = cfg->is_hdr;
    s_fg_dinv = cfg->depth_inverted;
    set_reason(reason_out, reason_cap, "OK");
    return 1;
}

static void fg_mat_ident(float m[4][4])
{
    memset(m, 0, 16 * sizeof(float));
    m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
}

static bool fg_mat_inv4(const float in[4][4], float out[4][4])
{
    double a[4][8];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            a[i][j]     = (double)in[i][j];
            a[i][j + 4] = (i == j) ? 1.0 : 0.0;
        }
    }
    for (int col = 0; col < 4; col++) {
        int piv = col;
        double best = fabs(a[col][col]);
        for (int r = col + 1; r < 4; r++) {
            double v = fabs(a[r][col]);
            if (v > best) { best = v; piv = r; }
        }
        if (best < 1e-12) return false;
        if (piv != col) {
            for (int j = 0; j < 8; j++) { double t = a[col][j]; a[col][j] = a[piv][j]; a[piv][j] = t; }
        }
        double diag = a[col][col];
        for (int j = 0; j < 8; j++) a[col][j] /= diag;
        for (int r = 0; r < 4; r++) {
            if (r == col) continue;
            double f = a[r][col];
            for (int j = 0; j < 8; j++) a[r][j] -= f * a[col][j];
        }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i][j] = (float)a[i][j + 4];
    return true;
}

PT5_EXPORT int pt5_fg_eval(void* d3d12_cmd_list, const Pt5FgEval* ev,
                           char* reason_out, size_t reason_cap)
{
    if (!s_fg_feature || !s_fg_pars || !d3d12_cmd_list || !ev ||
        !ev->color || !ev->depth || !ev->mv || !ev->output[0]) {
        set_reason(reason_out, reason_cap, "FG not created or missing resources");
        return 0;
    }
    ID3D12GraphicsCommandList* cl = static_cast<ID3D12GraphicsCommandList*>(d3d12_cmd_list);
    const uint32_t gen = ev->generated < 1 ? 1 : (ev->generated > s_fg_max_gen ? s_fg_max_gen : ev->generated);
    const uint32_t rw = ev->render_w ? ev->render_w : s_fg_ren_w;
    const uint32_t rh = ev->render_h ? ev->render_h : s_fg_ren_h;
    const uint32_t ow = ev->out_w ? ev->out_w : s_fg_out_w;
    const uint32_t oh = ev->out_h ? ev->out_h : s_fg_out_h;

    NVSDK_NGX_DLSSG_Opt_Eval_Params op = {};
    const bool has_ext = ev->struct_size >= sizeof(Pt5FgEval);
    bool used_real_cam = false;
    if (has_ext && ev->has_camera) {
        memcpy(op.cameraViewToClip, ev->view_to_clip, sizeof(op.cameraViewToClip));
        if (fg_mat_inv4(op.cameraViewToClip, op.clipToCameraView)) {
            memcpy(op.cameraPos, ev->cam_pos, sizeof(op.cameraPos));
            memcpy(op.cameraUp, ev->cam_up, sizeof(op.cameraUp));
            memcpy(op.cameraRight, ev->cam_right, sizeof(op.cameraRight));
            memcpy(op.cameraFwd, ev->cam_fwd, sizeof(op.cameraFwd));
            op.cameraNear = ev->cam_near;
            op.cameraFar = ev->cam_far;
            op.cameraFOV = ev->cam_fov;
            op.cameraAspectRatio = ev->cam_aspect;
            op.cameraPinholeOffset[0] = ev->cam_pinhole[0];
            op.cameraPinholeOffset[1] = ev->cam_pinhole[1];
            used_real_cam = true;
        }
    }
    if (!used_real_cam) {
        const float aspect = rh ? (float)rw / (float)rh : 16.0f / 9.0f;
        const float fov = 0.9f, zn = 0.1f, zf = 1000.0f;
        const float ys = 1.0f / tanf(fov * 0.5f), xs = ys / aspect, zq = zf / (zf - zn);
        op.cameraViewToClip[0][0] = xs;
        op.cameraViewToClip[1][1] = ys;
        op.cameraViewToClip[2][2] = zq;
        op.cameraViewToClip[2][3] = 1.0f;
        op.cameraViewToClip[3][2] = -zn * zq;
        op.clipToCameraView[0][0] = 1.0f / xs;
        op.clipToCameraView[1][1] = 1.0f / ys;
        op.clipToCameraView[2][3] = -1.0f / (zn * zq);
        op.clipToCameraView[3][2] = 1.0f;
        op.clipToCameraView[3][3] = 1.0f / zn;
        op.cameraUp[1] = 1.0f;
        op.cameraRight[0] = 1.0f;
        op.cameraFwd[2] = 1.0f;
        op.cameraNear = zn;
        op.cameraFar = zf;
        op.cameraFOV = fov;
        op.cameraAspectRatio = aspect;
    }
    fg_mat_ident(op.clipToLensClip);
    if (ev->has_clip_to_prev) {
        memcpy(op.clipToPrevClip, ev->clip_to_prev, sizeof(op.clipToPrevClip));
        if (!fg_mat_inv4(op.clipToPrevClip, op.prevClipToClip)) {
            fg_mat_ident(op.clipToPrevClip);
            fg_mat_ident(op.prevClipToClip);
        }
        op.jitterOffset[0] = ev->jitter_x;
        op.jitterOffset[1] = ev->jitter_y;
    } else {
        fg_mat_ident(op.clipToPrevClip);
        fg_mat_ident(op.prevClipToClip);
    }
    op.mvecScale[0] = ev->mv_scale_x != 0.0f ? ev->mv_scale_x : 1.0f;
    op.mvecScale[1] = ev->mv_scale_y != 0.0f ? ev->mv_scale_y : 1.0f;
    op.colorBuffersHDR = s_fg_hdr != 0;
    op.depthInverted = s_fg_dinv != 0;
    op.cameraMotionIncluded = true;
    op.reset = ev->reset != 0;
    op.automodeOverrideReset = false;
    op.notRenderingGameFrames = ev->not_game != 0;
    op.orthoProjection = false;
    op.motionVectorsInvalidValue = 0.0f;
    op.motionVectorsDilated = false;
    op.menuDetectionEnabled = false;
    op.mvecsSubrectSize.Width = rw;
    op.mvecsSubrectSize.Height = rh;
    op.depthSubrectSize.Width = rw;
    op.depthSubrectSize.Height = rh;
    op.backbufferSubrectSize.Width = ow;
    op.backbufferSubrectSize.Height = oh;
    op.outputInterpSubrectSize.Width = ow;
    op.outputInterpSubrectSize.Height = oh;
    ID3D12Resource* hudless = (has_ext && ev->hudless) ? static_cast<ID3D12Resource*>(ev->hudless) : nullptr;
    if (hudless) {
        op.hudLessSubrectSize.Width = ow;
        op.hudLessSubrectSize.Height = oh;
    }

    NVSDK_NGX_Parameter_SetULL(s_fg_pars, NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, ev->frame_id);
    NVSDK_NGX_Parameter_SetUI(s_fg_pars, NVSDK_NGX_DLSSG_Parameter_EvalFlags, 0);

    for (uint32_t k = 0; k < gen; k++) {
        if (!ev->output[k]) {
            set_reason(reason_out, reason_cap, "Missing FG output texture");
            return 0;
        }
        NVSDK_NGX_D3D12_DLSSG_Eval_Params ep = {};
        ep.pBackbuffer = static_cast<ID3D12Resource*>(ev->color);
        ep.pDepth = static_cast<ID3D12Resource*>(ev->depth);
        ep.pMVecs = static_cast<ID3D12Resource*>(ev->mv);
        ep.pHudless = hudless;
        ep.pOutputInterpFrame = static_cast<ID3D12Resource*>(ev->output[k]);
        op.multiFrameCount = gen;
        op.multiFrameIndex = k + 1u;
        NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSSG(cl, s_fg_feature, s_fg_pars, &ep, &op);
        if (NVSDK_NGX_FAILED(r)) {
            char buf[160];
            snprintf(buf, sizeof buf, "NGX_D3D12_EVALUATE_DLSSG failed (%s 0x%08X)",
                     ngx_result_name(r), (unsigned)r);
            set_reason(reason_out, reason_cap, buf);
            return 0;
        }
    }
    return 1;
}

PT5_EXPORT void pt5_upscale_shutdown(void* d3d12_device)
{
    pt5_fg_destroy();
    pt5_upscale_destroy_feature();
    if (s_eval_pars) {
        NVSDK_NGX_D3D12_DestroyParameters(s_eval_pars);
        s_eval_pars = nullptr;
    }
    if (s_cap_pars) {
        NVSDK_NGX_D3D12_DestroyParameters(s_cap_pars);
        s_cap_pars = nullptr;
    }
    if (s_inited) {
        NVSDK_NGX_D3D12_Shutdown1(static_cast<ID3D12Device*>(d3d12_device));
        s_inited = false;
    }
    s_device = nullptr;
}
