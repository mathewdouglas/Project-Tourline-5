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
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_defs.h"
#include "nvsdk_ngx_helpers.h"
#include "rsx_upscale_plugin.h"

#define PT5_EXPORT extern "C" __declspec(dllexport)

/* Custom Project ID GUID for Project Tourline 5 (NGX_ENGINE_TYPE_CUSTOM). */
static const char* const kProjectId    = "8F3A4D61-7B2C-4E19-9A5F-0D4C1E72B501";
static const char* const kEngineVer    = "0.3.0";

static bool                 s_inited   = false;
static NVSDK_NGX_Parameter* s_cap_pars = nullptr;
static NVSDK_NGX_Parameter* s_eval_pars = nullptr;
static NVSDK_NGX_Handle*    s_feature  = nullptr;

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

PT5_EXPORT void pt5_upscale_shutdown(void* d3d12_device)
{
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
}
