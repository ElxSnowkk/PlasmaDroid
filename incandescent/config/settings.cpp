

#include "settings.h"
#include "config.h"
#include "../gl/log.h"
#include "../gl/envvars.h"
#include "gpu_utils.h"
#include "../gl/getter.h"

#define DEBUG 0

global_settings_t global_settings;

static void parse_multidraw_orders();

void init_settings() {
#if defined(__APPLE__)
    global_settings.angle = AngleMode::Disabled;
    global_settings.angle_config = AngleConfig::DisableIfPossible;
    global_settings.angle_supported = false;
    global_settings.ignore_error = IgnoreErrorLevel::Partial;
    global_settings.ext_compute_shader = false;
    global_settings.max_glsl_cache_size = 30 * 1024 * 1024;
    global_settings.angle_depth_clear_fix_mode = AngleDepthClearFixMode::Disabled;
    global_settings.ext_direct_state_access = true;
    global_settings.custom_gl_version = {0, 0, 0};
    global_settings.fsr1_setting = FSR1_Quality_Preset::Disabled;
    global_settings.hide_mg_env_level = HideMGEnvLevel::Disabled;

#else

    int success = initialized;
    if (!success) {
        success = config_refresh();
        if (!success) {
            LOG_V("Failed to load config. Use default config.")
        }
    }

    AngleConfig angleConfig =
        success ? static_cast<AngleConfig>(config_get_int("enableANGLE")) : AngleConfig::DisableIfPossible;
    NoErrorConfig noErrorConfig =
        success ? static_cast<NoErrorConfig>(config_get_int("enableNoError")) : NoErrorConfig::Auto;
    bool enableExtComputeShader = success ? (config_get_int("enableExtComputeShader") > 0) : false;
    bool enableExtTimerQuery = success ? (config_get_int("enableExtTimerQuery") > 0) : false;
    bool enableExtDirectStateAccess = success ? (config_get_int("enableExtDirectStateAccess") > 0) : false;
    AngleDepthClearFixMode angleDepthClearFixMode =
        success ? static_cast<AngleDepthClearFixMode>(config_get_int("angleDepthClearFixMode"))
                : AngleDepthClearFixMode::Disabled;
    int customGLVersionInt = success ? config_get_int("customGLVersion") : DEFAULT_GL_VERSION;
    FSR1_Quality_Preset fsr1Setting =
        success ? static_cast<FSR1_Quality_Preset>(config_get_int("fsr1Setting")) : FSR1_Quality_Preset::Disabled;
    int upscalerMode = success ? config_get_int("upscalerMode") : 0;
    int upscalerSharpness = success ? config_get_int("upscalerSharpness") : 80;
    int upscalerDetail = success ? config_get_int("upscalerDetail") : 40;
    int upscalerTier = success ? config_get_int("upscalerTier") : 2;
    int upscalerCompute = success ? config_get_int("upscalerCompute") : 1;
    int upscalerScalePct = success ? config_get_int("upscalerScalePct") : 0;
    int upscalerMinBase = success ? config_get_int("upscalerMinBase") : 0;
    if (upscalerMode < 0 || upscalerMode > 1) upscalerMode = 0;
    if (upscalerSharpness < 0 || upscalerSharpness > 100) upscalerSharpness = 80;
    if (upscalerDetail < 0 || upscalerDetail > 100) upscalerDetail = 40;
    if (upscalerTier < 0 || upscalerTier > 3) upscalerTier = 2;
    if (upscalerCompute < 0 || upscalerCompute > 1) upscalerCompute = 1;
    if (upscalerScalePct < 50 || upscalerScalePct > 99) upscalerScalePct = 0;
    if (upscalerMinBase < 120 || upscalerMinBase > 2160) upscalerMinBase = 0;
    HideMGEnvLevel hideMGEnvLevel =
        success ? static_cast<HideMGEnvLevel>(config_get_int("hideMGEnvLevel")) : HideMGEnvLevel::Disabled;

    if (customGLVersionInt < 0) {
        customGLVersionInt = 0;
    }

    size_t maxGlslCacheSize = 128u * 1024u * 1024u;
    if (success) {
        const int cfgCache = config_get_int("maxGlslCacheSize");
        if (cfgCache >= 0) maxGlslCacheSize = static_cast<size_t>(cfgCache) * 1024u * 1024u;
    }

    int maliTuning = success ? config_get_int("maliTuning") : 1;
    if (maliTuning < 0 || maliTuning > 2) maliTuning = 1;
    const int perfStats = (success && config_get_int("perfStats") > 0) ? 1 : 0;

    if (static_cast<int>(angleConfig) < 0 || static_cast<int>(angleConfig) > 3) {
        angleConfig = AngleConfig::DisableIfPossible;
    }
    if (static_cast<int>(noErrorConfig) < 0 || static_cast<int>(noErrorConfig) > 3) {
        noErrorConfig = NoErrorConfig::Auto;
    }
    if (static_cast<int>(angleDepthClearFixMode) < 0 ||
        static_cast<int>(angleDepthClearFixMode) >= static_cast<int>(AngleDepthClearFixMode::MaxValue)) {
        angleDepthClearFixMode = AngleDepthClearFixMode::Disabled;
    }
    if (customGLVersionInt > 46) {
        customGLVersionInt = 46;
    } else if (customGLVersionInt < 32 && customGLVersionInt != 0) {
        customGLVersionInt = 32;
    } else if (customGLVersionInt > 33 && customGLVersionInt < 40) {
        customGLVersionInt = 33;
    } else if (customGLVersionInt == 0) {
        customGLVersionInt = DEFAULT_GL_VERSION;
    }
    if (static_cast<int>(fsr1Setting) < 0 ||
        static_cast<int>(fsr1Setting) >= static_cast<int>(FSR1_Quality_Preset::MaxValue)) {
        fsr1Setting = FSR1_Quality_Preset::Disabled;
    }
    if (static_cast<int>(hideMGEnvLevel) < 0 ||
        static_cast<int>(hideMGEnvLevel) >= static_cast<int>(HideMGEnvLevel::MaxValue)) {
        hideMGEnvLevel = HideMGEnvLevel::Disabled;
    }

    Version customGLVersion(customGLVersionInt);

    int isInPluginApp = 0;
    GetEnvVarInt("INC_PLUGIN_STATUS", &isInPluginApp, 0);
    int fclVersion = 0;
    GetEnvVarInt("FCL_VERSION_CODE", &fclVersion, 0);
    int zlVersion = 0;
    GetEnvVarInt("ZALITH_VERSION_CODE", &zlVersion, 0);
    int pgwVersion = 0;
    GetEnvVarInt("PGW_VERSION_CODE", &pgwVersion, 0);

    LOG_V("INC_DIR_PATH = %s", inc_directory_path ? inc_directory_path : "(default)")

    if (isInPluginApp == 0 && fclVersion == 0 && zlVersion == 0 && pgwVersion == 0 && !is_custom_mg_dir) {
        LOG_V("Unsupported launcher detected, force using default config.")
        angleConfig = AngleConfig::DisableIfPossible;
        noErrorConfig = NoErrorConfig::Auto;
        enableExtComputeShader = false;
        enableExtTimerQuery = true;
        enableExtDirectStateAccess = true;
        maxGlslCacheSize = 128u * 1024u * 1024u;
        angleDepthClearFixMode = AngleDepthClearFixMode::Disabled;
        fsr1Setting = FSR1_Quality_Preset::Disabled;
        hideMGEnvLevel = HideMGEnvLevel::Disabled;
    }

    AngleMode finalAngleMode = AngleMode::Disabled;
    std::string gpuString = getGPUInfo();
    const char* gpu_cstr = gpuString.c_str();
    LOG_D("GPU: %s", gpu_cstr ? gpu_cstr : "(unknown)")

    int hasVk12 = hasVulkan12();
    int isQcom = isAdreno(gpu_cstr);
    int is730 = isAdreno730(gpu_cstr);
    int is740 = isAdreno740(gpu_cstr);
    int is830 = isAdreno830(gpu_cstr);
    bool isANGLESupported = checkIfANGLESupported(gpu_cstr);

    LOG_D("Has Vulkan 1.2? = %s", hasVk12 ? "true" : "false")
    LOG_D("Is Adreno? = %s", isQcom ? "true" : "false")
    LOG_D("Is Adreno 730? = %s", is730 ? "true" : "false")
    LOG_D("Is Adreno 740? = %s", is740 ? "true" : "false")
    LOG_D("Is Adreno 830? = %s", is830 ? "true" : "false")
    LOG_D("Is ANGLE supported? = %s", isANGLESupported ? "true" : "false")

    {
        int angleEnv = -1;
        GetEnvVarInt("INC_ANGLE", &angleEnv, -1);
        if (angleEnv == 1) angleConfig = AngleConfig::ForceEnable;
        else if (angleEnv == 0) angleConfig = AngleConfig::ForceDisable;
    }

    switch (angleConfig) {
    case AngleConfig::ForceDisable:
        finalAngleMode = AngleMode::Disabled;
        LOG_D("ANGLE: Force disabled");
        break;

    case AngleConfig::ForceEnable:
        finalAngleMode = AngleMode::Enabled;
        LOG_D("ANGLE: Force enabled");
        break;

    case AngleConfig::EnableIfPossible: {
        finalAngleMode = isANGLESupported ? AngleMode::Enabled : AngleMode::Disabled;
        LOG_D("ANGLE: Conditionally %s", (finalAngleMode == AngleMode::Enabled) ? "enabled" : "disabled");
        break;
    }

    case AngleConfig::DisableIfPossible:
    default:
        finalAngleMode = AngleMode::Disabled;
        break;
    }

    global_settings.angle = finalAngleMode;
    global_settings.angle_config = angleConfig;
    global_settings.angle_supported = isANGLESupported;
    LOG_D("Final ANGLE setting: %d", static_cast<int>(global_settings.angle))
    global_settings.buffer_coherent_as_flush = (global_settings.angle == AngleMode::Disabled);

    if (global_settings.angle == AngleMode::Enabled) {

    }

    switch (noErrorConfig) {
    case NoErrorConfig::Level1:
        global_settings.ignore_error = IgnoreErrorLevel::Partial;
        LOG_D("Error ignoring: Level 1 (Partial)");
        break;

    case NoErrorConfig::Level2:
        global_settings.ignore_error = IgnoreErrorLevel::Full;
        LOG_D("Error ignoring: Level 2 (Full)");
        break;

    case NoErrorConfig::Auto:
    case NoErrorConfig::Disable:
    default:
        global_settings.ignore_error = IgnoreErrorLevel::None;
        LOG_D("Error ignoring: Disabled");
        break;
    }

    global_settings.ext_compute_shader = enableExtComputeShader;
    global_settings.ext_timer_query = enableExtTimerQuery;
    global_settings.ext_direct_state_access = enableExtDirectStateAccess;
    global_settings.max_glsl_cache_size = maxGlslCacheSize;
    global_settings.mali_tuning = maliTuning;
    global_settings.perf_stats = perfStats;
    global_settings.angle_depth_clear_fix_mode = angleDepthClearFixMode;
    global_settings.custom_gl_version = customGLVersion;
    global_settings.fsr1_setting = fsr1Setting;
    global_settings.upscaler_mode = upscalerMode;
    global_settings.upscaler_sharpness = upscalerSharpness;
    global_settings.upscaler_detail = upscalerDetail;
    global_settings.upscaler_tier = upscalerTier;
    global_settings.upscaler_compute = upscalerCompute;
    global_settings.upscaler_scale_pct = upscalerScalePct;
    global_settings.upscaler_min_base = upscalerMinBase;
    global_settings.hide_mg_env_level = hideMGEnvLevel;
#endif

    LOG_V("[Incandescent] Setting: enableAngle                 = %s",
          global_settings.angle == AngleMode::Enabled ? "true" : "false")
    LOG_V("[Incandescent] Setting: ignoreError                 = %i", static_cast<int>(global_settings.ignore_error))
    LOG_V("[Incandescent] Setting: enableExtComputeShader      = %s",
          global_settings.ext_compute_shader ? "true" : "false")
    LOG_V("[Incandescent] Setting: enableExtTimerQuery         = %s", global_settings.ext_timer_query ? "true" : "false")
    LOG_V("[Incandescent] Setting: enableExtDirectStateAccess  = %s",
          global_settings.ext_direct_state_access ? "true" : "false")
    LOG_V("[Incandescent] Setting: maxGlslCacheSize            = %i",
          static_cast<int>(global_settings.max_glsl_cache_size / 1024 / 1024))
    LOG_V("[Incandescent] Setting: maliTuning                = %i", global_settings.mali_tuning)
    LOG_V("[Incandescent] Setting: angleDepthClearFixMode      = %i",
          static_cast<int>(global_settings.angle_depth_clear_fix_mode))
    LOG_V("[Incandescent] Setting: bufferCoherentAsFlush       = %i",
          static_cast<int>(global_settings.buffer_coherent_as_flush))
    if (global_settings.custom_gl_version.isEmpty()) {
        LOG_V("[Incandescent] Setting: customGLVersion             = (default)");
    } else {
        LOG_V("[Incandescent] Setting: customGLVersion             = %s",
              global_settings.custom_gl_version.toString().c_str());
    }
    LOG_V("[Incandescent] Setting: fsr1Setting                 = %i", static_cast<int>(global_settings.fsr1_setting))
    LOG_V("[Incandescent] Setting: upscaler(KryoSS v4) mode/tier/sharp/detail/scale = %i/%i/%i/%i/%i", global_settings.upscaler_mode,
          global_settings.upscaler_tier, global_settings.upscaler_sharpness, global_settings.upscaler_detail,
          global_settings.upscaler_scale_pct)
    LOG_V("[Incandescent] Setting: hideMGEnvLevel              = %i",
          static_cast<int>(global_settings.hide_mg_env_level))

    GLVersion =
        global_settings.custom_gl_version.isEmpty() ? Version(DEFAULT_GL_VERSION) : global_settings.custom_gl_version;

    parse_multidraw_orders();
}

namespace {

using B = md_backend_t;
using E = md_entry_t;

constexpr unsigned md_bit(B b) {
    return 1u << static_cast<int>(b);
}

struct md_backend_name_t {
    const char* name;
    B backend;
};

const md_backend_name_t k_md_backend_names[] = {
    {"auto", B::Auto},           {"unroll", B::Unroll},   {"basevertex", B::BaseVertex},
    {"indirect", B::Indirect},   {"multiindirect", B::MultiIndirect}, {"multibasevertex", B::MultiBaseVertex},
    {"multiarrays", B::MultiArrays}, {"compute", B::Compute},
};

const char* const k_md_default_global_order[] = {
    "native", "multiindirect", "multibasevertex", "multiarrays", "indirect", "basevertex", "unroll", "compute",
};
constexpr int MD_GLOBAL_ITEMS = 8;

struct md_entry_desc_t {
    const char* order_key;
    const char* legacy_mode_key;
    const char* label;
    unsigned allowed;
    B native_backend;
    const char* why;
};

const md_entry_desc_t k_md_entries[MD_ENTRY_COUNT] = {
    {"multidrawOrderArrays", "multidrawModeArrays", "glMultiDrawArrays",
     md_bit(B::Unroll) | md_bit(B::MultiArrays) | md_bit(B::MultiIndirect),
     B::MultiArrays,
     "glMultiDrawArrays draws no indices, so index-side backends do not apply"},

    {"multidrawOrderElements", "multidrawModeElements", "glMultiDrawElements",
     md_bit(B::Unroll) | md_bit(B::Indirect) | md_bit(B::MultiIndirect) | md_bit(B::MultiBaseVertex) |
         md_bit(B::MultiArrays),
     B::MultiArrays,
     "glMultiDrawElements has no base vertex, so basevertex/compute are the same loop as unroll"},

    {"multidrawOrderElementsBaseVertex", "multidrawModeElementsBaseVertex", "glMultiDrawElementsBaseVertex",
     md_bit(B::Unroll) | md_bit(B::BaseVertex) | md_bit(B::Indirect) | md_bit(B::MultiIndirect) |
         md_bit(B::MultiBaseVertex) | md_bit(B::Compute),
     B::MultiBaseVertex,
     "multiarrays (EXT_multi_draw_arrays) carries no base vertex"},

    {"multidrawOrderArraysIndirect", "multidrawModeArraysIndirect", "glMultiDrawArraysIndirect",
     md_bit(B::Indirect) | md_bit(B::MultiIndirect),
     B::MultiIndirect,
     "the application supplies the commands, so only indirect/multiindirect exist here"},

    {"multidrawOrderElementsIndirect", "multidrawModeElementsIndirect", "glMultiDrawElementsIndirect",
     md_bit(B::Indirect) | md_bit(B::MultiIndirect),
     B::MultiIndirect,
     "the application supplies the commands, so only indirect/multiindirect exist here"},
};

struct md_caps_t {
    bool basevertex;
    bool indirect_arrays;
    bool indirect_elements;
    bool multiindirect_arrays;
    bool multiindirect_elements;
    bool multibasevertex;
    bool multiarrays;
    bool compute;
};

bool md_is_arrays_side(E e) {
    return e == E::Arrays || e == E::ArraysIndirect;
}

bool md_backend_available(E e, B b, const md_caps_t& c) {
    switch (b) {
    case B::Unroll:
        return true;
    case B::BaseVertex:
        return c.basevertex;
    case B::Indirect:
        return md_is_arrays_side(e) ? c.indirect_arrays : c.indirect_elements;
    case B::MultiIndirect:
        return md_is_arrays_side(e) ? c.multiindirect_arrays : c.multiindirect_elements;
    case B::MultiBaseVertex:
        return c.multibasevertex;
    case B::MultiArrays:
        return c.multiarrays;
    case B::Compute:
        return c.compute;
    default:
        return false;
    }
}

bool md_parse_backend(const std::string& raw, B* out) {
    std::string s;
    for (char ch : raw) {
        if (ch == ' ' || ch == '\t' || ch == '_' || ch == '-') continue;
        s += static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    }
    if (s.empty()) return false;
    for (const auto& n : k_md_backend_names) {
        if (s == n.name) {
            *out = n.backend;
            return true;
        }
    }
    return false;
}

std::string md_config_string(const char* key) {
    const char* v = config_get_string(const_cast<char*>(key));

    return v ? std::string(v) : std::string();
}

}

const char* md_backend_name(md_backend_t b) {
    for (const auto& n : k_md_backend_names) {
        if (n.backend == b) return n.name;
    }
    return "(unknown)";
}

const char* md_backend_suffix(md_backend_t b) {
    switch (b) {
    case B::Unroll:
        return "_drawelements";
    case B::BaseVertex:
        return "_basevertex";
    case B::Indirect:
        return "_indirect";
    case B::MultiIndirect:
        return "_multiindirect";
    case B::MultiBaseVertex:
        return "_multibasevertex";
    case B::MultiArrays:
        return "_multiarrays";
    case B::Compute:
        return "_compute";
    default:
        return nullptr;
    }
}

struct md_order_item_t {
    bool is_native;
    B backend;
};

static int md_parse_order_list(const char* key, const std::string& raw, bool allow_native, md_order_item_t* out,
                               int out_max) {
    int n = 0;
    std::string token;
    for (size_t i = 0; i <= raw.size(); ++i) {
        const char ch = i < raw.size() ? raw[i] : ',';
        if (ch != ',' && ch != ';') {
            token += ch;
            continue;
        }

        std::string s;
        for (char c : token) {
            if (c == ' ' || c == '\t' || c == '_' || c == '-') continue;
            s += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        }
        token.clear();
        if (s.empty()) continue;
        if (n >= out_max) break;
        if (s == "native") {
            if (allow_native) {
                out[n++] = {true, B::Auto};
            } else {
                LOG_W_FORCE("%s: 'native' is only meaningful in the global multidrawOrder, ignored", key)
            }
            continue;
        }
        B b;
        if (!md_parse_backend(s, &b) || b == B::Auto) {
            LOG_W_FORCE("%s: '%s' is not a backend name, ignored", key, s.c_str())
            continue;
        }
        out[n++] = {false, b};
    }
    return n;
}

static B s_md_requested[MD_ENTRY_COUNT][MD_BACKEND_COUNT];
static int s_md_requested_len[MD_ENTRY_COUNT];

static void md_expand_order(E e, const md_order_item_t* items, int item_count) {
    const md_entry_desc_t& d = k_md_entries[static_cast<int>(e)];
    B* out = s_md_requested[static_cast<int>(e)];
    int n = 0;
    unsigned seen = 0;

    auto push = [&](B b) {
        if ((d.allowed & md_bit(b)) == 0) return;
        if (seen & md_bit(b)) return;
        seen |= md_bit(b);
        out[n++] = b;
    };

    for (int i = 0; i < item_count; ++i) {
        push(items[i].is_native ? d.native_backend : items[i].backend);
    }

    for (const char* name : k_md_default_global_order) {
        if (std::string(name) == "native") {
            push(d.native_backend);
        } else {
            B b;
            if (md_parse_backend(name, &b)) push(b);
        }
    }
    s_md_requested_len[static_cast<int>(e)] = n;
}

static void parse_multidraw_orders() {
    md_order_item_t global_items[MD_BACKEND_COUNT + 1];
    int global_count = 0;

    const std::string raw_global = md_config_string("multidrawOrder");
    if (!raw_global.empty()) {
        global_count =
            md_parse_order_list("multidrawOrder", raw_global, true, global_items, MD_BACKEND_COUNT + 1);
    }
    if (global_count == 0) {
        for (const char* name : k_md_default_global_order) {
            md_order_item_t item{};
            if (std::string(name) == "native") {
                item.is_native = true;
            } else if (!md_parse_backend(name, &item.backend)) {
                continue;
            }
            global_items[global_count++] = item;
        }
    }

    for (int i = 0; i < MD_ENTRY_COUNT; ++i) {
        const E e = static_cast<E>(i);
        const md_entry_desc_t& d = k_md_entries[i];
        const std::string raw = md_config_string(d.order_key);
        if (!raw.empty()) {

            md_order_item_t items[MD_BACKEND_COUNT];
            const int count = md_parse_order_list(d.order_key, raw, false, items, MD_BACKEND_COUNT);
            for (int k = 0; k < count; ++k) {
                if (!items[k].is_native && (d.allowed & md_bit(items[k].backend)) == 0) {
                    LOG_W_FORCE("%s: '%s' is not a distinct strategy for %s (%s), ignored", d.order_key,
                                md_backend_name(items[k].backend), d.label, d.why)
                }
            }
            md_expand_order(e, items, count);
        } else {
            md_expand_order(e, global_items, global_count);
        }
    }
}

void set_multidraw_setting() {

    if (config_get_int(const_cast<char*>("multidrawMode")) != -1 ||
        config_get_string(const_cast<char*>("multidrawDisableBackends")) != nullptr) {
        LOG_W_FORCE("multidrawMode/multidrawDisableBackends are no longer used. The selection is an order "
                    "now: multidrawOrder (global, backend names best first, may contain \"native\") and "
                    "multidrawOrder<EntryPoint> for per-function exceptions.")
    }
    for (const auto& d : k_md_entries) {
        if (config_get_string(const_cast<char*>(d.legacy_mode_key)) != nullptr) {
            LOG_W_FORCE("%s is no longer used; see multidrawOrder / %s", d.legacy_mode_key, d.order_key)
            break;
        }
    }
}

md_backend_t md_next_backend(md_entry_t e, md_backend_t cur) {
    const int idx = static_cast<int>(e);
    const int len = global_settings.multidraw_order_len[idx];
    const B* order = global_settings.multidraw_order[idx];
    if (len <= 0) return B::Unroll;
    for (int i = 0; i < len; ++i) {
        if (order[i] == cur) {
            return order[i + 1 < len ? i + 1 : len - 1];
        }
    }

    return order[len - 1];
}

void init_settings_post() {
    const bool has_es31 = (g_gles_caps.major > 3) || (g_gles_caps.major == 3 && g_gles_caps.minor >= 1);
    const bool has_es32 = (g_gles_caps.major > 3) || (g_gles_caps.major == 3 && g_gles_caps.minor >= 2);
    const bool has_bv_ext =
        g_gles_caps.GL_EXT_draw_elements_base_vertex || g_gles_caps.GL_OES_draw_elements_base_vertex;

    const bool multidraw = g_gles_caps.GL_EXT_multi_draw_indirect && GLES.glMultiDrawElementsIndirectEXT != nullptr;
    const bool basevertex = (has_bv_ext || has_es32) && GLES.glDrawElementsBaseVertex != nullptr;
    const bool indirect = has_es31 && GLES.glDrawElementsIndirect != nullptr;

    const bool multibasevertex = inc_multi_draw_elements_basevertex_ext_available();

    bool compute = false;
    if (has_es31 && GLES.glDispatchCompute) {
        GLint ssbo_blocks = 0;
        GLES.glGetIntegerv(GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS, &ssbo_blocks);

        compute = ssbo_blocks >= 4;
        if (!compute) LOG_W_FORCE("Compute multidraw needs 4 SSBO blocks, driver reports %d", ssbo_blocks)
    }

    md_caps_t md_caps = {};
    md_caps.basevertex = basevertex;
    md_caps.indirect_elements = indirect;
    md_caps.indirect_arrays = has_es31 && GLES.glDrawArraysIndirect != nullptr;
    md_caps.multiindirect_elements = multidraw;
    md_caps.multiindirect_arrays =
        g_gles_caps.GL_EXT_multi_draw_indirect && GLES.glMultiDrawArraysIndirectEXT != nullptr;
    md_caps.multibasevertex = multibasevertex;
    md_caps.multiarrays = inc_multi_draw_arrays_ext_available();
    md_caps.compute = compute;

    for (int i = 0; i < MD_ENTRY_COUNT; ++i) {
        const md_entry_t e = static_cast<md_entry_t>(i);
        int n = 0;
        for (int k = 0; k < s_md_requested_len[i]; ++k) {
            const B cand = s_md_requested[i][k];
            if (!md_backend_available(e, cand, md_caps)) continue;
            global_settings.multidraw_order[i][n++] = cand;
        }
        if (n == 0) {
            LOG_W_FORCE("%s: no backend in the order is available on this device; keeping %s", k_md_entries[i].label,
                        md_backend_name(s_md_requested[i][0]))
            global_settings.multidraw_order[i][n++] = s_md_requested[i][0];
        }
        global_settings.multidraw_order_len[i] = n;
        global_settings.multidraw_backend[i] = global_settings.multidraw_order[i][0];

        std::string order_str;
        for (int k = 0; k < n; ++k) {
            if (!order_str.empty()) order_str += " > ";
            order_str += md_backend_name(global_settings.multidraw_order[i][k]);
        }
        LOG_V("[Incandescent] %-34s = %s", k_md_entries[i].order_key, order_str.c_str())
    }
}

std::string dump_settings_string(std::string prefix) {
    std::stringstream ss;

    ss << prefix << "Angle: " << (global_settings.angle == AngleMode::Enabled ? "Enabled" : "Disabled") << "\n";
    ss << prefix << "IgnoreError: ";
    switch (global_settings.ignore_error) {
    case IgnoreErrorLevel::None:
        ss << "None";
        break;
    case IgnoreErrorLevel::Partial:
        ss << "Partial";
        break;
    case IgnoreErrorLevel::Full:
        ss << "Full";
        break;
    }
    ss << "\n";

    ss << prefix << "ExtComputeShader: " << (global_settings.ext_compute_shader ? "True" : "False") << "\n";
    ss << prefix << "ExtTimerQuery: " << (global_settings.ext_timer_query ? "True" : "False") << "\n";
    ss << prefix << "ExtDirectStateAccess: " << (global_settings.ext_direct_state_access ? "True" : "False") << "\n";
    ss << prefix << "MaxGlslCacheSize: " << (global_settings.max_glsl_cache_size / 1024 / 1024) << "MB\n";

    for (int i = 0; i < MD_ENTRY_COUNT; ++i) {
        ss << prefix << k_md_entries[i].order_key << ": ";
        for (int k = 0; k < global_settings.multidraw_order_len[i]; ++k) {
            if (k > 0) ss << " > ";
            ss << md_backend_name(global_settings.multidraw_order[i][k]);
        }
        ss << "\n";
    }

    ss << prefix << "AngleDepthClearFixMode: "
       << (global_settings.angle_depth_clear_fix_mode == AngleDepthClearFixMode::Disabled ? "Disabled" : "Enabled")
       << "\n";

    ss << prefix << "BufferCoherentAsFlush: " << (global_settings.buffer_coherent_as_flush ? "True" : "False") << "\n";

    ss << prefix << "CustomGLVersion: "
       << ((GLVersion.toInt(2) == DEFAULT_GL_VERSION) ? "(Default)" : std::to_string(GLVersion.toInt(2))) << "\n";

    ss << prefix << "Fsr1Setting: ";

    switch (global_settings.fsr1_setting) {
    case FSR1_Quality_Preset::Disabled:
        ss << "Disabled";
        break;
    case FSR1_Quality_Preset::UltraQuality:
        ss << "UltraQuality";
        break;
    case FSR1_Quality_Preset::Quality:
        ss << "Quality";
        break;
    case FSR1_Quality_Preset::Balanced:
        ss << "Balanced";
        break;
    case FSR1_Quality_Preset::Performance:
        ss << "Performance";
        break;
    default:
        ss << "Unknown";
        break;
    }
    ss << "\n";

    ss << prefix << "HideMGEnvLevel: "
       << ((global_settings.hide_mg_env_level == HideMGEnvLevel::Disabled)
               ? "Disabled"
               : std::to_string(static_cast<int>(global_settings.hide_mg_env_level)));

    ss << "\n";

    return ss.str();
}
