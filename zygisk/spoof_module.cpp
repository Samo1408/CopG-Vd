#include <jni.h>
#include <cstdint>
#include <string>
#include <zygisk.hpp>
#include <json.hpp>
#include <fstream>
#include <android/log.h>
#include <algorithm>
#include <cctype>
#include <thread>
#include <chrono>
#include <vector>
#include <utility>
#include <unistd.h>
#include <cstdint>

using json = nlohmann::json;

#define LOG_TAG "COPG"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ERROR_LOG(...) LOGE("[ERROR] " __VA_ARGS__)

static const std::string config_file = "/data/adb/modules/COPG/COPG.json";
static inline std::string trim(const std::string& str);

// Resolve the device profile used by the always-on Zygisk path. Older builds used the
// top-level "COPG" object; the current WebUI stores devices as PACKAGES_<NAME>_DEVICE.
// Prefer an explicit global-device.name (friendly NAME or profile key). For migration from
// the current UI, accept a single profile carrying SPOOF_OPTIONS; never guess among several.
static json readEffectiveDeviceConfig() {
    std::ifstream file(config_file);
    if (!file.is_open()) return json::object();
    try {
        json config = json::parse(file);
        std::string requested;
        {
            std::ifstream marker("/data/adb/COPG/global-device.name");
            if (marker.is_open()) std::getline(marker, requested);
            requested = trim(requested);
        }
        if (!requested.empty()) {
            for (auto it = config.begin(); it != config.end(); ++it) {
                const std::string key = it.key();
                if (key.rfind("PACKAGES_", 0) == 0 && key.size() >= 7 &&
                    key.substr(key.size() - 7) == "_DEVICE" && it.value().is_object() &&
                    (key == requested || it.value().value("NAME", std::string()) == requested))
                    return it.value();
            }
        }
        if (config.contains("COPG") && config["COPG"].is_object()) return config["COPG"];

        json sole = json::object();
        int optionsProfiles = 0;
        for (auto it = config.begin(); it != config.end(); ++it) {
            const std::string key = it.key();
            if (key.rfind("PACKAGES_", 0) != 0 || key.size() < 7 || key.substr(key.size() - 7) != "_DEVICE" || !it.value().is_object()) continue;
            const json& profile = it.value();
            if (profile.contains("SPOOF_OPTIONS") && profile["SPOOF_OPTIONS"].is_object()) {
                ++optionsProfiles;
                sole = profile;
            }
        }
        if (optionsProfiles == 1) return sole;
        if (optionsProfiles > 1) LOGE("Multiple device profiles contain SPOOF_OPTIONS; set /data/adb/COPG/global-device.name to select one");
        return json::object();
    } catch (const std::exception& e) {
        LOGE("Could not resolve device profile: %s", e.what());
        return json::object();
    }
}

static std::string readGlobalDeviceProperty(const char* propertyName) {
    if (!propertyName || !*propertyName) return {};
    std::ifstream file("/data/adb/COPG/global-device.props");
    if (!file.is_open()) return {};
    std::string line;
    while (std::getline(file, line)) {
        const auto tab = line.find('\t');
        if (tab == std::string::npos || line.compare(0, tab, propertyName) != 0) continue;
        if (tab != std::string(propertyName).size()) continue;
        std::string value = trim(line.substr(tab + 1));
        if (!value.empty() && value != "Undefined") return value;
    }
    return {};
}

static std::string effectiveDeviceValue(const char* field) {
    json device = readEffectiveDeviceConfig();
    if (!device.is_object()) return {};
    std::string value;
    if (device.contains(field) && device[field].is_string()) value = trim(device[field].get<std::string>());
    const std::string requestedField(field);
    if ((value.empty() || value == "Undefined") && requestedField == "BASEBAND" &&
        device.contains("RADIO") && device["RADIO"].is_string()) value = trim(device["RADIO"].get<std::string>());
    if ((value.empty() || value == "Undefined") && requestedField == "SERIAL" &&
        device.contains("SPOOF_OPTIONS") && device["SPOOF_OPTIONS"].is_object()) {
        const auto& opts = device["SPOOF_OPTIONS"];
        if (opts.contains("controls") && opts["controls"].is_object()) {
            const auto& controls = opts["controls"];
            if (controls.value("pfSerial", false) && controls.contains("pfSerialVal") && controls["pfSerialVal"].is_string())
                value = trim(controls["pfSerialVal"].get<std::string>());
        }
    }
    return value == "Undefined" ? std::string() : value;
}

// Java SystemProperties native_get hook.  TelephonyManager on modern Android ultimately
// resolves several network fields through TelephonyProperties -> SystemProperties.  Keeping
// a read-side hook in addition to resetprop closes the window where an app has not observed the
// property update yet (and also covers cached/framework-generated reads in the app process).
using SystemPropertiesNativeGetFn = jstring (*)(JNIEnv*, jclass, jstring, jstring);
using SystemPropertiesNativeGetHandleFn = jstring (*)(JNIEnv*, jclass, jlong);
static SystemPropertiesNativeGetFn g_originalSystemPropertiesNativeGet = nullptr;
static SystemPropertiesNativeGetHandleFn g_originalSystemPropertiesNativeGetHandle = nullptr;
static zygisk::Api* g_zygiskApi = nullptr;

static std::string readConfigString(const char* objectKey, const char* fieldKey) {
    std::ifstream file(config_file);
    if (!file.is_open()) return {};
    try {
        json config = json::parse(file);
        if (!config.contains(objectKey) || !config[objectKey].is_object()) return {};
        const auto& obj = config[objectKey];
        if (!obj.contains(fieldKey) || !obj[fieldKey].is_string()) return {};
        std::string value = trim(obj[fieldKey].get<std::string>());
        if (value == "Undefined") return {};
        return value;
    } catch (...) {
        return {};
    }
}

static std::string spoofedSystemPropertyValue(const std::string& key) {
    // The profile's Build/property values must be visible through Android's read-side API too,
    // not only through Build static fields or the boot-time resetprop script.
    static const std::pair<const char*, const char*> profileProps[] = {
        {"ro.product.brand", "BRAND"}, {"ro.product.system.brand", "BRAND"},
        {"ro.product.vendor.brand", "BRAND"}, {"ro.product.manufacturer", "MANUFACTURER"},
        {"ro.product.system.manufacturer", "MANUFACTURER"}, {"ro.product.vendor.manufacturer", "MANUFACTURER"},
        {"ro.product.model", "MODEL"}, {"ro.product.system.model", "MODEL"}, {"ro.product.vendor.model", "MODEL"},
        {"ro.product.device", "DEVICE"}, {"ro.product.system.device", "DEVICE"}, {"ro.product.vendor.device", "DEVICE"},
        {"ro.product.name", "PRODUCT"}, {"ro.product.system.name", "PRODUCT"}, {"ro.product.vendor.name", "PRODUCT"},
        {"ro.product.board", "BOARD"}, {"ro.board.platform", "BOARD_PLATFORM"},
        {"ro.hardware", "HARDWARE"}, {"ro.boot.hardware", "HARDWARE"},
        {"ro.bootloader", "BOOTLOADER"}, {"ro.boot.bootloader", "BOOTLOADER"},
        {"ro.serialno", "SERIAL"}, {"ro.boot.serialno", "SERIAL"},
        {"ro.build.fingerprint", "FINGERPRINT"}, {"ro.system.build.fingerprint", "FINGERPRINT"}, {"ro.vendor.build.fingerprint", "FINGERPRINT"},
        {"ro.build.id", "ID"}, {"ro.build.display.id", "DISPLAY"},
        {"ro.build.version.incremental", "INCREMENTAL"}, {"ro.build.version.security_patch", "SECURITY_PATCH"},
        {"ro.build.version.release", "ANDROID_VERSION"}, {"ro.build.version.sdk", "SDK_INT"},
        {"ro.soc.manufacturer", "SOC_MANUFACTURER"}, {"ro.soc.model", "SOC_MODEL"}
    };
    for (const auto& mapping : profileProps) {
        if (key == mapping.first) {
            std::string value = effectiveDeviceValue(mapping.second);
            if (!value.empty()) return value;
        }
    }
    const auto countryEnabled = [](const char* field) {
        std::ifstream file(config_file);
        if (!file.is_open()) return false;
        try {
            json config = json::parse(file);
            if (!config.contains("COPG-Country") || !config["COPG-Country"].is_object()) return false;
            return config["COPG-Country"].value(field, false);
        } catch (...) { return false; }
    };
    const auto country = [&](const char* field, const char* enabled) -> std::string {
        if (!countryEnabled(enabled)) return {};
        return readConfigString("COPG-Country", field);
    };

    if (key == "gsm.operator.iso-country") return country("ISO", "ISO_ENABLED");
    if (key == "gsm.sim.operator.iso-country") return country("ISO", "ISO_ENABLED");
    if (key == "gsm.operator.iso-country.2") return country("ISO", "ISO_ENABLED");
    if (key == "gsm.operator.numeric") {
        auto v = country("OPERATOR_NUMERIC", "OPERATOR_NUMERIC_ENABLED");
        return v.empty() ? country("MCCMNC", "MCCMNC_ENABLED") : v;
    }
    if (key == "gsm.sim.operator.numeric" || key == "gsm.sim.cdmaoperator.numeric" ||
        key == "gsm.apn.sim.operator.numeric") return country("MCCMNC", "MCCMNC_ENABLED");
    if (key == "gsm.operator.alpha") return country("OPERATOR_NAME", "OPERATOR_NAME_ENABLED");
    if (key == "gsm.sim.operator.alpha") return country("OPERATOR_NAME", "OPERATOR_NAME_ENABLED");
    if (key == "gsm.version.baseband" || key == "ro.baseband") {
        // Resolve in the same order as the device editor: selected profile BASEBAND,
        // its RADIO alias, the exact value published for resetprop, then legacy COPG config.
        std::string value = effectiveDeviceValue("BASEBAND");
        if (value.empty()) value = readGlobalDeviceProperty("ro.baseband");
        if (value.empty()) value = readConfigString("COPG", "BASEBAND");
        return value;
    }
    return {};
}

static jstring hookSystemPropertiesNativeGet(JNIEnv* env, jclass clazz, jstring key, jstring def) {
    if (!env) return nullptr;
    const char* keyChars = key ? env->GetStringUTFChars(key, nullptr) : nullptr;
    if (keyChars) {
        std::string spoofed = spoofedSystemPropertyValue(keyChars);
        env->ReleaseStringUTFChars(key, keyChars);
        if (!spoofed.empty()) return env->NewStringUTF(spoofed.c_str());
    }
    return g_originalSystemPropertiesNativeGet
        ? g_originalSystemPropertiesNativeGet(env, clazz, key, def)
        : (def ? def : env->NewStringUTF(""));
}

static jstring hookSystemPropertiesNativeGetHandle(JNIEnv* env, jclass clazz, jlong handle) {
    return g_originalSystemPropertiesNativeGetHandle
        ? g_originalSystemPropertiesNativeGetHandle(env, clazz, handle)
        : env->NewStringUTF("");
}

// What the ROM really is. NEVER a system property: the module rewrites those very props, so
// asking the system would be asking our own lie. /build.prop does not exist on these devices;
// on a custom ROM the fingerprint line inside this file is stale, but ro.build.version.* is good.
static const std::string rom_prop_file = "/data/adb/COPG/global-device.props";
static const std::string version_policy_file = "/data/adb/modules/COPG/.spoof.version";

// The Android version belongs to the ROM, not to the build being spoofed. An app told the SDK
// is newer than the framework really is calls APIs that do not exist: Google's apps crash, the
// device reboots, and it repeats - a softloop, which leaves nothing in the boot logs.
//   Never = the version group is never applied (default)
//   Rom   = only what does not exceed the ROM (in practice, lowering the SDK)
//   Force = whatever the config says
enum class VersionPolicy { Never, Rom, Force };

struct RomVersion {
    std::string release;
    std::string codename;
    int sdk = 0;
};

struct DeviceInfo {
    std::string brand;
    std::string baseband;
    std::string device;
    std::string manufacturer;
    std::string model;
    std::string fingerprint;
    std::string product;
    std::string android_version;
    int version_sdk_int = 0;
    std::string board;
    std::string bootloader;
    std::string hardware;
    std::string id;
    std::string display;
    std::string host;
    std::string odm_sku;
    std::string sku;
    std::string user;
    int64_t time = 0;
    std::string version_incremental;
    std::string version_sdk;
    int version_sdk_int_full = 0;
    std::string version_security_patch;
    std::string version_release_or_codename;
    std::string version_release_or_preview_display;
    std::string version_codename;
    std::string serial;
    std::string soc_manufacturer;
    std::string soc_model;
};


struct CountryInfo {
    bool iso_enabled = false;
    bool mccmnc_enabled = false;
    bool operator_name_enabled = false;
    bool operator_numeric_enabled = false;
    std::string iso;
    std::string mccmnc;
    std::string operator_name;
    std::string operator_numeric;
    double latitude = 0.0;
    double longitude = 0.0;
};

static inline std::string trim(const std::string& str) {
    auto start = std::find_if_not(str.begin(), str.end(), [](unsigned char c) { 
        return std::isspace(c); 
    });
    auto end = std::find_if_not(str.rbegin(), str.rend(), [](unsigned char c) { 
        return std::isspace(c); 
    }).base();
    return (start < end) ? std::string(start, end) : std::string();
}

static RomVersion readRomVersion() {
    RomVersion rom;
    std::ifstream file(rom_prop_file);
    if (!file.is_open()) return rom;                 // unknown ROM -> nothing is allowed through
    std::string line;
    while (std::getline(file, line)) {
        auto take = [&line](const char* key, std::string& out) {
            const std::string needle = std::string(key) + "=";
            if (line.rfind(needle, 0) == 0) out = trim(line.substr(needle.size()));
        };
        take("ro.build.version.release", rom.release);
        take("ro.build.version.codename", rom.codename);
        std::string sdk;
        take("ro.build.version.sdk", sdk);
        if (!sdk.empty()) {
            try { rom.sdk = std::stoi(sdk); } catch (const std::exception&) { rom.sdk = 0; }
        }
    }
    return rom;
}

static VersionPolicy readVersionPolicy() {
    std::ifstream file(version_policy_file);
    if (!file.is_open()) return VersionPolicy::Never;
    std::string value;
    std::getline(file, value);
    value = trim(value);
    if (value == "force") return VersionPolicy::Force;
    if (value == "rom") return VersionPolicy::Rom;
    return VersionPolicy::Never;
}

// AOSP: RELEASE_OR_CODENAME = "REL".equals(CODENAME) ? RELEASE : CODENAME. Copying CODENAME
// into it publishes the literal string "REL" where the version number belongs, which is a
// combination no real device reports.
static std::string releaseOrCodename(const std::string& codename, const std::string& release) {
    return (codename.empty() || codename == "REL") ? release : codename;
}



static bool readCountryConfig(CountryInfo& out) {
    std::ifstream file(config_file);
    if (!file.is_open()) return false;
    try {
        json config = json::parse(file);
        if (!config.contains("COPG-Country") || !config["COPG-Country"].is_object()) return false;
        const auto& c = config["COPG-Country"];
        out.iso_enabled = c.value("ISO_ENABLED", false);
        out.mccmnc_enabled = c.value("MCCMNC_ENABLED", false);
        out.operator_name_enabled = c.value("OPERATOR_NAME_ENABLED", false);
        out.operator_numeric_enabled = c.value("OPERATOR_NUMERIC_ENABLED", false);
        out.iso = trim(c.value("ISO", ""));
        out.mccmnc = trim(c.value("MCCMNC", ""));
        out.operator_name = trim(c.value("OPERATOR_NAME", ""));
        out.operator_numeric = trim(c.value("OPERATOR_NUMERIC", ""));
        if (c.contains("LATITUDE") && c["LATITUDE"].is_number()) out.latitude = c["LATITUDE"].get<double>();
        if (c.contains("LONGITUDE") && c["LONGITUDE"].is_number()) out.longitude = c["LONGITUDE"].get<double>();
        return true;
    } catch (const std::exception& e) {
        ERROR_LOG("Country config error: %s", e.what());
        return false;
    }
}

static bool callTelephonyStringListSetter(JNIEnv* env, const char* methodName, const std::string& value) {
    if (!env || !methodName || value.empty()) return false;
    jclass cls = env->FindClass("android/sysprop/TelephonyProperties");
    if (!cls) { env->ExceptionClear(); return false; }
    jmethodID method = env->GetStaticMethodID(cls, methodName, "(Ljava/util/List;)V");
    if (env->ExceptionCheck()) { env->ExceptionClear(); method = nullptr; }
    bool ok = false;
    if (method) {
        jclass listClass = env->FindClass("java/util/ArrayList");
        if (listClass) {
            jmethodID ctor = env->GetMethodID(listClass, "<init>", "()V");
            jmethodID add = env->GetMethodID(listClass, "add", "(Ljava/lang/Object;)Z");
            jobject list = (ctor && add) ? env->NewObject(listClass, ctor) : nullptr;
            jstring jv = env->NewStringUTF(value.c_str());
            if (list && jv) {
                env->CallBooleanMethod(list, add, jv);
                if (env->ExceptionCheck()) env->ExceptionClear();
                else {
                    env->CallStaticVoidMethod(cls, method, list);
                    if (env->ExceptionCheck()) env->ExceptionClear(); else ok = true;
                }
            }
            if (jv) env->DeleteLocalRef(jv);
            if (list) env->DeleteLocalRef(list);
            env->DeleteLocalRef(listClass);
        } else env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
    return ok;
}

static bool callTelephonyStringSetter(JNIEnv* env, const char* methodName, const std::string& value) {
    if (!env || !methodName || value.empty()) return false;
    jclass cls = env->FindClass("android/sysprop/TelephonyProperties");
    if (!cls) { env->ExceptionClear(); return false; }
    jmethodID method = env->GetStaticMethodID(cls, methodName, "(Ljava/lang/String;)V");
    if (env->ExceptionCheck()) { env->ExceptionClear(); method = nullptr; }
    bool ok = false;
    if (method) {
        jstring jv = env->NewStringUTF(value.c_str());
        if (jv) {
            env->CallStaticVoidMethod(cls, method, jv);
            if (env->ExceptionCheck()) env->ExceptionClear(); else ok = true;
            env->DeleteLocalRef(jv);
        }
    }
    env->DeleteLocalRef(cls);
    return ok;
}

static void setSystemProperty(JNIEnv* env, const char* key, const std::string& value) {
    if (!env || value.empty()) return;
    jclass cls = env->FindClass("android/os/SystemProperties");
    if (!cls) { env->ExceptionClear(); return; }
    jmethodID set = env->GetStaticMethodID(cls, "set", "(Ljava/lang/String;Ljava/lang/String;)V");
    if (env->ExceptionCheck()) { env->ExceptionClear(); set = nullptr; }
    if (set) {
        jstring jk = env->NewStringUTF(key);
        jstring jv = env->NewStringUTF(value.c_str());
        if (jk && jv) env->CallStaticVoidMethod(cls, set, jk, jv);
        if (env->ExceptionCheck()) { env->ExceptionClear(); LOGE("Failed setting property %s", key); }
        if (jk) env->DeleteLocalRef(jk);
        if (jv) env->DeleteLocalRef(jv);
    }
    env->DeleteLocalRef(cls);
}


static bool callGeneratedTelephonySetter(JNIEnv* env, const char* className,
                                         const char* methodName, const std::string& value) {
    if (!env || !className || !methodName || value.empty()) return false;
    jclass cls = env->FindClass(className);
    if (!cls) { env->ExceptionClear(); return false; }
    bool ok = false;
    // Samsung's SemTelephonyProps has changed signatures between One UI generations. Try the
    // generated StringList setter first, then a String setter for vendor variants.
    jmethodID listMethod = env->GetStaticMethodID(cls, methodName, "(Ljava/util/List;)V");
    if (env->ExceptionCheck()) { env->ExceptionClear(); listMethod = nullptr; }
    if (listMethod) {
        jclass listClass = env->FindClass("java/util/ArrayList");
        if (listClass) {
            jmethodID ctor = env->GetMethodID(listClass, "<init>", "()V");
            jmethodID add = env->GetMethodID(listClass, "add", "(Ljava/lang/Object;)Z");
            jobject list = (ctor && add) ? env->NewObject(listClass, ctor) : nullptr;
            jstring jv = env->NewStringUTF(value.c_str());
            if (list && jv) {
                env->CallBooleanMethod(list, add, jv);
                if (!env->ExceptionCheck()) {
                    env->CallStaticVoidMethod(cls, listMethod, list);
                    if (!env->ExceptionCheck()) ok = true;
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            if (jv) env->DeleteLocalRef(jv);
            if (list) env->DeleteLocalRef(list);
            env->DeleteLocalRef(listClass);
        } else env->ExceptionClear();
    }
    if (!ok) {
        jmethodID stringMethod = env->GetStaticMethodID(cls, methodName, "(Ljava/lang/String;)V");
        if (env->ExceptionCheck()) { env->ExceptionClear(); stringMethod = nullptr; }
        if (stringMethod) {
            jstring jv = env->NewStringUTF(value.c_str());
            if (jv) {
                env->CallStaticVoidMethod(cls, stringMethod, jv);
                if (!env->ExceptionCheck()) ok = true;
                if (env->ExceptionCheck()) env->ExceptionClear();
                env->DeleteLocalRef(jv);
            }
        }
    }
    env->DeleteLocalRef(cls);
    return ok;
}

static void applySemTelephonyProperties(JNIEnv* env, const CountryInfo& c) {
    // Samsung generated sysprop class. These names cover the common One UI generated API; the
    // calls are best-effort because non-Samsung builds simply do not contain this class.
    if (!env) return;
    if (c.iso_enabled && !c.iso.empty()) {
        callGeneratedTelephonySetter(env, "android/sysprop/SemTelephonyProps", "operatorIsoCountry", c.iso);
        callGeneratedTelephonySetter(env, "android/sysprop/SemTelephonyProps", "iccOperatorIsoCountry", c.iso);
        callGeneratedTelephonySetter(env, "android/sysprop/SemTelephonyProps", "simOperatorIsoCountry", c.iso);
    }
    if (c.mccmnc_enabled && !c.mccmnc.empty()) {
        callGeneratedTelephonySetter(env, "android/sysprop/SemTelephonyProps", "operatorNumeric", c.mccmnc);
        callGeneratedTelephonySetter(env, "android/sysprop/SemTelephonyProps", "iccOperatorNumeric", c.mccmnc);
        callGeneratedTelephonySetter(env, "android/sysprop/SemTelephonyProps", "simOperatorNumeric", c.mccmnc);
    }
    if (c.operator_name_enabled && !c.operator_name.empty()) {
        callGeneratedTelephonySetter(env, "android/sysprop/SemTelephonyProps", "operatorAlpha", c.operator_name);
        callGeneratedTelephonySetter(env, "android/sysprop/SemTelephonyProps", "iccOperatorAlpha", c.operator_name);
        callGeneratedTelephonySetter(env, "android/sysprop/SemTelephonyProps", "simOperatorAlpha", c.operator_name);
    }
}

static void applyCountryProperties(JNIEnv* env, const CountryInfo& c) {
    auto setTelephony = [&](const char* method, const char* prop, const std::string& value) {
        if (value.empty()) return;
        // Framework generated sysprop API first; SystemProperties is the fallback used by
        // Android releases where TelephonyProperties is hidden/renamed.
        if (!callTelephonyStringListSetter(env, method, value)) {
            callTelephonyStringSetter(env, method, value);
        }
        setSystemProperty(env, prop, value);
    };

    if (c.iso_enabled && !c.iso.empty()) {
        const std::string iso = [&]() { std::string v = c.iso; std::transform(v.begin(), v.end(), v.begin(), [](unsigned char x){ return std::tolower(x); }); return v; }();
        setTelephony("operatorIsoCountry", "gsm.operator.iso-country", iso);
        setTelephony("iccOperatorIsoCountry", "gsm.sim.operator.iso-country", iso);
        setSystemProperty(env, "gsm.operator.iso-country.2", iso);
    }
    if (c.mccmnc_enabled && !c.mccmnc.empty()) {
        setTelephony("iccOperatorNumeric", "gsm.sim.operator.numeric", c.mccmnc);
        setTelephony("operatorNumeric", "gsm.operator.numeric", c.mccmnc);
        if (c.mccmnc.size() >= 5) {
            setSystemProperty(env, "gsm.sim.operator.mcc", c.mccmnc.substr(0, 3));
            setSystemProperty(env, "gsm.sim.operator.mnc", c.mccmnc.substr(3));
        }
    }
    if (c.operator_name_enabled && !c.operator_name.empty()) {
        setTelephony("operatorAlpha", "gsm.operator.alpha", c.operator_name);
        setTelephony("iccOperatorAlpha", "gsm.sim.operator.alpha", c.operator_name);
    }
    if (c.operator_numeric_enabled && !c.operator_numeric.empty()) {
        setTelephony("operatorNumeric", "gsm.operator.numeric", c.operator_numeric);
        setTelephony("iccOperatorNumeric", "gsm.sim.operator.numeric", c.operator_numeric);
    }

    // Additional framework telephony keys requested for complete coverage.
    if (c.mccmnc_enabled && !c.mccmnc.empty()) {
        setTelephony("iccOperatorNumeric", "gsm.sim.cdmaoperator.numeric", c.mccmnc);
        setTelephony("iccOperatorNumeric", "gsm.apn.sim.operator.numeric", c.mccmnc);
    }
    applySemTelephonyProperties(env, c);
}




// SubscriptionInfo is a real Parcelable object, so changing system properties alone does not
// rewrite the already-materialized fields carried by that object.  On app processes we obtain
// the active SubscriptionInfo objects through the public framework API and normalize their
// framework fields.  This is deliberately reflection-based and best-effort: failure on a ROM
// with different/private fields is harmless and does not block boot.
static int patchSubscriptionInfoObjects(JNIEnv* env, const CountryInfo& c) {
    if (!env) return 0;
    if ((!c.iso_enabled || c.iso.empty()) &&
        (!c.mccmnc_enabled || c.mccmnc.empty()) &&
        (!c.operator_name_enabled || c.operator_name.empty())) return 0;

    jclass activityThread = env->FindClass("android/app/ActivityThread");
    if (!activityThread) { env->ExceptionClear(); return 0; }
    jmethodID currentApplication = env->GetStaticMethodID(
        activityThread, "currentApplication", "()Landroid/app/Application;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); currentApplication = nullptr; }
    if (!currentApplication) { env->DeleteLocalRef(activityThread); return 0; }
    jobject context = env->CallStaticObjectMethod(activityThread, currentApplication);
    if (env->ExceptionCheck()) { env->ExceptionClear(); context = nullptr; }
    env->DeleteLocalRef(activityThread);
    if (!context) return 0;

    jclass subMgr = env->FindClass("android/telephony/SubscriptionManager");
    if (!subMgr) { env->ExceptionClear(); env->DeleteLocalRef(context); return 0; }
    jmethodID from = env->GetStaticMethodID(
        subMgr, "from", "(Landroid/content/Context;)Landroid/telephony/SubscriptionManager;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); from = nullptr; }
    if (!from) { env->DeleteLocalRef(subMgr); env->DeleteLocalRef(context); return 0; }
    jobject manager = env->CallStaticObjectMethod(subMgr, from, context);
    if (env->ExceptionCheck()) { env->ExceptionClear(); manager = nullptr; }
    if (!manager) { env->DeleteLocalRef(subMgr); env->DeleteLocalRef(context); return 0; }

    jmethodID getList = env->GetMethodID(
        subMgr, "getActiveSubscriptionInfoList", "()Ljava/util/List;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); getList = nullptr; }
    jobject list = getList ? env->CallObjectMethod(manager, getList) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); list = nullptr; }
    if (!list) {
        env->DeleteLocalRef(manager); env->DeleteLocalRef(subMgr); env->DeleteLocalRef(context); return 0;
    }

    jclass listClass = env->FindClass("java/util/List");
    jmethodID size = listClass ? env->GetMethodID(listClass, "size", "()I") : nullptr;
    jmethodID get = listClass ? env->GetMethodID(listClass, "get", "(I)Ljava/lang/Object;") : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); }

    jclass fieldClass = env->FindClass("java/lang/reflect/Field");
    jmethodID setAccessible = fieldClass ? env->GetMethodID(fieldClass, "setAccessible", "(Z)V") : nullptr;
    jmethodID setField = fieldClass ? env->GetMethodID(fieldClass, "set", "(Ljava/lang/Object;Ljava/lang/Object;)V") : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); }

    jclass subInfo = env->FindClass("android/telephony/SubscriptionInfo");
    jclass classClass = env->FindClass("java/lang/Class");
    jmethodID getDeclaredField = classClass ? env->GetMethodID(
        classClass, "getDeclaredField", "(Ljava/lang/String;)Ljava/lang/reflect/Field;") : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); }

    int patched = 0;
    if (listClass && size && get && fieldClass && setAccessible && setField && subInfo && getDeclaredField) {
        auto setStringField = [&](jobject obj, const char* name, const std::string& value) -> bool {
            if (!obj || value.empty()) return false;
            jstring jname = env->NewStringUTF(name);
            jobject field = jname ? env->CallObjectMethod(subInfo, getDeclaredField, jname) : nullptr;
            if (env->ExceptionCheck()) { env->ExceptionClear(); field = nullptr; }
            if (jname) env->DeleteLocalRef(jname);
            if (!field) return false;
            env->CallVoidMethod(field, setAccessible, JNI_TRUE);
            if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(field); return false; }
            jstring jvalue = env->NewStringUTF(value.c_str());
            if (!jvalue) { env->DeleteLocalRef(field); return false; }
            env->CallVoidMethod(field, setField, obj, jvalue);
            bool ok = !env->ExceptionCheck();
            if (!ok) env->ExceptionClear();
            env->DeleteLocalRef(jvalue);
            env->DeleteLocalRef(field);
            return ok;
        };

        const jint count = env->CallIntMethod(list, size);
        if (!env->ExceptionCheck()) {
            for (jint i = 0; i < count; ++i) {
                jobject info = env->CallObjectMethod(list, get, i);
                if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
                if (!info) continue;
                bool changed = false;
                if (c.iso_enabled && !c.iso.empty()) changed |= setStringField(info, "mCountryIso", c.iso);
                if (c.mccmnc_enabled && c.mccmnc.size() >= 5) {
                    changed |= setStringField(info, "mMcc", c.mccmnc.substr(0, 3));
                    changed |= setStringField(info, "mMnc", c.mccmnc.substr(3));
                }
                if (c.operator_name_enabled && !c.operator_name.empty()) {
                    changed |= setStringField(info, "mCarrierName", c.operator_name);
                    changed |= setStringField(info, "mDisplayName", c.operator_name);
                }
                if (changed) ++patched;
                env->DeleteLocalRef(info);
            }
        } else env->ExceptionClear();
    }

    if (classClass) env->DeleteLocalRef(classClass);
    if (subInfo) env->DeleteLocalRef(subInfo);
    if (fieldClass) env->DeleteLocalRef(fieldClass);
    if (listClass) env->DeleteLocalRef(listClass);
    env->DeleteLocalRef(list);
    env->DeleteLocalRef(manager);
    env->DeleteLocalRef(subMgr);
    env->DeleteLocalRef(context);
    return patched;
}

static void startSubscriptionInfoWatcher(JavaVM* vm) {
    if (!vm) return;
    std::thread([vm]() {
        for (;;) {
            JNIEnv* env = nullptr;
#if defined(__ANDROID__)
            if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK && env) {
#else
            if (vm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr) == JNI_OK && env) {
#endif
                CountryInfo country;
                if (readCountryConfig(country)) {
                    const int patched = patchSubscriptionInfoObjects(env, country);
                    if (patched > 0) LOGE("SubscriptionInfo normalized: %d object(s)", patched);
                }
                vm->DetachCurrentThread();
            }
            sleep(5);
        }
    }).detach();
}

static jobject makeLocation(JNIEnv* env, double latitude, double longitude) {
    if (!env) return nullptr;
    jclass locationClass = env->FindClass("android/location/Location");
    if (!locationClass) { env->ExceptionClear(); return nullptr; }
    jmethodID ctor = env->GetMethodID(locationClass, "<init>", "(Ljava/lang/String;)V");
    jmethodID setLatitude = env->GetMethodID(locationClass, "setLatitude", "(D)V");
    jmethodID setLongitude = env->GetMethodID(locationClass, "setLongitude", "(D)V");
    if (env->ExceptionCheck()) { env->ExceptionClear(); }
    jobject location = nullptr;
    if (ctor && setLatitude && setLongitude) {
        jstring provider = env->NewStringUTF("gps");
        if (provider) {
            location = env->NewObject(locationClass, ctor, provider);
            env->DeleteLocalRef(provider);
            if (env->ExceptionCheck()) { env->ExceptionClear(); location = nullptr; }
        }
        if (location) {
            env->CallVoidMethod(location, setLatitude, latitude);
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->CallVoidMethod(location, setLongitude, longitude);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }
    env->DeleteLocalRef(locationClass);
    return location;
}

static bool invokeLocationManager(JNIEnv* env, jobject service, const CountryInfo& c) {
    if (!service) return false;
    jclass serviceClass = env->GetObjectClass(service);
    if (!serviceClass) return false;
    jclass locationClass = env->FindClass("android/location/Location");
    if (!locationClass) { env->ExceptionClear(); env->DeleteLocalRef(serviceClass); return false; }
    jclass listClass = env->FindClass("java/util/ArrayList");
    jclass propsClass = env->FindClass("android/location/provider/ProviderProperties");
    if (!propsClass) { env->ExceptionClear(); propsClass = env->FindClass("android/location/ProviderProperties"); }

    bool changed = false;
    jobject location = makeLocation(env, c.latitude, c.longitude);
    if (!location) { env->DeleteLocalRef(locationClass); env->DeleteLocalRef(serviceClass); if (listClass) env->DeleteLocalRef(listClass); if (propsClass) env->DeleteLocalRef(propsClass); return false; }

    // Build a ProviderProperties instance on Android 12+; older releases use the legacy
    // boolean/int overload. Reflection keeps one binary usable across both API families.
    jobject props = nullptr;
    if (propsClass) {
        jclass builderClass = env->FindClass("android/location/provider/ProviderProperties$Builder");
        if (!builderClass) { env->ExceptionClear(); builderClass = env->FindClass("android/location/ProviderProperties$Builder"); }
        if (builderClass) {
            jmethodID bctor = env->GetMethodID(builderClass, "<init>", "()V");
            jmethodID build = env->GetMethodID(builderClass, "build", "()Landroid/location/provider/ProviderProperties;");
            if (!build) { env->ExceptionClear(); build = env->GetMethodID(builderClass, "build", "()Landroid/location/ProviderProperties;"); }
            if (bctor && build) {
                jobject builder = env->NewObject(builderClass, bctor);
                if (builder) props = env->CallObjectMethod(builder, build);
                if (env->ExceptionCheck()) { env->ExceptionClear(); props = nullptr; }
                if (builder) env->DeleteLocalRef(builder);
            }
            env->DeleteLocalRef(builderClass);
        }
    }

    // Invoke addTestProvider overloads without hard-coding an Android release-specific AIDL.
    jmethodID addLegacy = env->GetMethodID(serviceClass, "addTestProvider", "(Ljava/lang/String;ZZZZZZZII)V");
    if (env->ExceptionCheck()) { env->ExceptionClear(); addLegacy = nullptr; }
    jmethodID setLegacy = env->GetMethodID(serviceClass, "setTestProviderLocation", "(Ljava/lang/String;Landroid/location/Location;)V");
    if (env->ExceptionCheck()) { env->ExceptionClear(); setLegacy = nullptr; }

    jstring gps = env->NewStringUTF("gps");
    jstring network = env->NewStringUTF("network");
    if (addLegacy && setLegacy) {
        env->CallVoidMethod(service, addLegacy, gps, JNI_TRUE, JNI_FALSE, JNI_TRUE, JNI_FALSE, JNI_TRUE, JNI_TRUE, JNI_TRUE, 2, 1);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->CallVoidMethod(service, addLegacy, network, JNI_TRUE, JNI_FALSE, JNI_TRUE, JNI_FALSE, JNI_TRUE, JNI_TRUE, JNI_TRUE, 2, 1);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->CallVoidMethod(service, setLegacy, gps, location);
        if (env->ExceptionCheck()) env->ExceptionClear(); else changed = true;
        env->CallVoidMethod(service, setLegacy, network, location);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // Modern Android uses addTestProvider(name, ProviderProperties, tags, package, attribution)
    // and setTestProviderLocation(name, Location, package, attribution). Try those forms too.
    if (props) {
        jmethodID addModern = env->GetMethodID(serviceClass, "addTestProvider", "(Ljava/lang/String;Landroid/location/provider/ProviderProperties;Ljava/util/List;Ljava/lang/String;Ljava/lang/String;)V");
        if (env->ExceptionCheck()) { env->ExceptionClear(); addModern = nullptr; }
        jmethodID setModern = env->GetMethodID(serviceClass, "setTestProviderLocation", "(Ljava/lang/String;Landroid/location/Location;Ljava/lang/String;Ljava/lang/String;)V");
        if (env->ExceptionCheck()) { env->ExceptionClear(); setModern = nullptr; }
        if (addModern && setModern && listClass) {
            jmethodID listCtor = env->GetMethodID(listClass, "<init>", "()V");
            jobject tags = listCtor ? env->NewObject(listClass, listCtor) : nullptr;
            jstring pkg = env->NewStringUTF("android");
            if (tags && pkg) {
                env->CallVoidMethod(service, addModern, gps, props, tags, pkg, nullptr);
                if (env->ExceptionCheck()) env->ExceptionClear();
                env->CallVoidMethod(service, addModern, network, props, tags, pkg, nullptr);
                if (env->ExceptionCheck()) env->ExceptionClear();
                env->CallVoidMethod(service, setModern, gps, location, pkg, nullptr);
                if (env->ExceptionCheck()) env->ExceptionClear(); else changed = true;
                env->CallVoidMethod(service, setModern, network, location, pkg, nullptr);
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            if (pkg) env->DeleteLocalRef(pkg);
            if (tags) env->DeleteLocalRef(tags);
        }
    }

    if (gps) env->DeleteLocalRef(gps);
    if (network) env->DeleteLocalRef(network);
    if (props) env->DeleteLocalRef(props);
    env->DeleteLocalRef(location);
    env->DeleteLocalRef(locationClass);
    if (listClass) env->DeleteLocalRef(listClass);
    if (propsClass) env->DeleteLocalRef(propsClass);
    env->DeleteLocalRef(serviceClass);
    return changed;
}

static void applyCountryLocation(JNIEnv* env, const CountryInfo& c) {
    if (!c.iso_enabled || c.iso.empty()) return;
    if (c.latitude == 0.0 && c.longitude == 0.0) return;

    jclass smClass = env->FindClass("android/os/ServiceManager");
    if (!smClass) { env->ExceptionClear(); return; }
    jmethodID getService = env->GetStaticMethodID(smClass, "getService", "(Ljava/lang/String;)Landroid/os/IBinder;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); getService = nullptr; }
    if (!getService) { env->DeleteLocalRef(smClass); return; }
    jstring name = env->NewStringUTF("location");
    jobject binder = nullptr;
    for (int i = 0; i < 30 && !binder; ++i) {
        binder = env->CallStaticObjectMethod(smClass, getService, name);
        if (env->ExceptionCheck()) { env->ExceptionClear(); binder = nullptr; }
        if (!binder) usleep(500000);
    }
    if (name) env->DeleteLocalRef(name);
    if (!binder) { env->DeleteLocalRef(smClass); return; }

    jclass stubClass = env->FindClass("android/location/ILocationManager$Stub");
    if (!stubClass) { env->ExceptionClear(); env->DeleteLocalRef(binder); env->DeleteLocalRef(smClass); return; }
    jmethodID asInterface = env->GetStaticMethodID(stubClass, "asInterface", "(Landroid/os/IBinder;)Landroid/location/ILocationManager;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); asInterface = nullptr; }
    if (asInterface) {
        jobject service = env->CallStaticObjectMethod(stubClass, asInterface, binder);
        if (env->ExceptionCheck()) { env->ExceptionClear(); service = nullptr; }
        if (service) {
            if (invokeLocationManager(env, service, c)) {
                LOGE("Country location applied in system_server: %s (%.6f, %.6f)", c.iso.c_str(), c.latitude, c.longitude);
            } else {
                LOGE("Country location could not be applied in system_server");
            }
            env->DeleteLocalRef(service);
        }
    }
    env->DeleteLocalRef(stubClass);
    env->DeleteLocalRef(binder);
    env->DeleteLocalRef(smClass);
}

class COPGVDModule : public zygisk::ModuleBase {
private:
    zygisk::Api* api = nullptr;
    JNIEnv* env = nullptr;
    JavaVM* jvm = nullptr;

    // Value-initialized: setInt/setLong only skip a field when it is 0, so an
    // indeterminate int here would be written straight into Build.TIME.
    DeviceInfo spoof_info{};

    void spoofDevice() {
        jclass buildClass = env->FindClass("android/os/Build");
        if (!buildClass) {
            env->ExceptionClear();
            return;
        }

        auto getField = [this](jclass cls, const char* name, const char* sig) -> jfieldID {
            jfieldID id = env->GetStaticFieldID(cls, name, sig);
            if (env->ExceptionCheck()) env->ExceptionClear();
            return id;
        };

        jfieldID build_modelField = getField(buildClass, "MODEL", "Ljava/lang/String;");
        jfieldID build_brandField = getField(buildClass, "BRAND", "Ljava/lang/String;");
        jfieldID build_deviceField = getField(buildClass, "DEVICE", "Ljava/lang/String;");
        jfieldID build_manufacturerField = getField(buildClass, "MANUFACTURER", "Ljava/lang/String;");
        jfieldID build_fingerprintField = getField(buildClass, "FINGERPRINT", "Ljava/lang/String;");
        jfieldID build_productField = getField(buildClass, "PRODUCT", "Ljava/lang/String;");
        jfieldID build_boardField = getField(buildClass, "BOARD", "Ljava/lang/String;");
        jfieldID build_bootloaderField = getField(buildClass, "BOOTLOADER", "Ljava/lang/String;");
        jfieldID build_hardwareField = getField(buildClass, "HARDWARE", "Ljava/lang/String;");
        jfieldID build_idField = getField(buildClass, "ID", "Ljava/lang/String;");
        jfieldID build_displayField = getField(buildClass, "DISPLAY", "Ljava/lang/String;");
        jfieldID build_hostField = getField(buildClass, "HOST", "Ljava/lang/String;");
        jfieldID build_odm_skuField = getField(buildClass, "ODM_SKU", "Ljava/lang/String;");
        jfieldID build_skuField = getField(buildClass, "SKU", "Ljava/lang/String;");
        jfieldID build_tagsField = getField(buildClass, "TAGS", "Ljava/lang/String;");
        jfieldID build_timeField = getField(buildClass, "TIME", "J");
        jfieldID build_typeField = getField(buildClass, "TYPE", "Ljava/lang/String;");
        jfieldID build_userField = getField(buildClass, "USER", "Ljava/lang/String;");
        jfieldID build_serialField = getField(buildClass, "SERIAL", "Ljava/lang/String;");
        jfieldID build_soc_manufacturerField = getField(buildClass, "SOC_MANUFACTURER", "Ljava/lang/String;");
        jfieldID build_soc_modelField = getField(buildClass, "SOC_MODEL", "Ljava/lang/String;");

        jclass versionClass = env->FindClass("android/os/Build$VERSION");
        jfieldID build_version_releaseField = nullptr;
        jfieldID build_version_sdk_intField = nullptr;
        jfieldID build_version_codenameField = nullptr;
        jfieldID build_version_incrementalField = nullptr;
        jfieldID build_version_sdkField = nullptr;
        jfieldID build_version_sdk_int_fullField = nullptr;
        jfieldID build_version_security_patchField = nullptr;
        jfieldID build_version_release_or_codenameField = nullptr;
        jfieldID build_version_release_or_preview_displayField = nullptr;

        if (versionClass) {
            build_version_releaseField = getField(versionClass, "RELEASE", "Ljava/lang/String;");
            build_version_sdk_intField = getField(versionClass, "SDK_INT", "I");
            build_version_codenameField = getField(versionClass, "CODENAME", "Ljava/lang/String;");
            build_version_incrementalField = getField(versionClass, "INCREMENTAL", "Ljava/lang/String;");
            build_version_sdkField = getField(versionClass, "SDK", "Ljava/lang/String;");
            build_version_sdk_int_fullField = getField(versionClass, "SDK_INT_FULL", "I");
            build_version_security_patchField = getField(versionClass, "SECURITY_PATCH", "Ljava/lang/String;");
            build_version_release_or_codenameField = getField(versionClass, "RELEASE_OR_CODENAME", "Ljava/lang/String;");
            build_version_release_or_preview_displayField = getField(versionClass, "RELEASE_OR_PREVIEW_DISPLAY", "Ljava/lang/String;");
        }

        std::ifstream file(config_file);
        if (!file.is_open()) {
            ERROR_LOG("Failed to open: %s", config_file.c_str());
            env->DeleteLocalRef(buildClass);
            if (versionClass) env->DeleteLocalRef(versionClass);
            return;
        }

        try {
            json config = json::parse(file);
            json device = readEffectiveDeviceConfig();
            if (!device.is_object() || device.empty()) {
                if (config.contains(std::string(LOG_TAG)) && config[LOG_TAG].is_object()) device = config[LOG_TAG];
            }

            if (device.is_object() && !device.empty()) {

                spoof_info.brand = device.value("BRAND", "");
                spoof_info.baseband = trim(device.value("BASEBAND", device.value("RADIO", std::string())));
                spoof_info.device = device.value("DEVICE", "");
                spoof_info.manufacturer = device.value("MANUFACTURER", "");
                spoof_info.model = device.value("MODEL", "");
                spoof_info.fingerprint = device.value("FINGERPRINT", "");
                spoof_info.product = device.value("PRODUCT", "");
                spoof_info.board = device.value("BOARD", "");
                spoof_info.bootloader = device.value("BOOTLOADER", "");
                spoof_info.hardware = device.value("HARDWARE", "");
                spoof_info.id = device.value("ID", "");
                spoof_info.display = device.value("DISPLAY", "");
                spoof_info.host = device.value("HOST", "");
                spoof_info.odm_sku = device.value("ODM_SKU", spoof_info.product);
                spoof_info.sku = device.value("SKU", spoof_info.hardware);
                spoof_info.user = device.value("USER", "");
                spoof_info.serial = device.value("SERIAL", "");
                spoof_info.soc_manufacturer = device.value("SOC_MANUFACTURER", "");
                spoof_info.soc_model = device.value("SOC_MODEL", "");
                if (device.contains("SPOOF_OPTIONS") && device["SPOOF_OPTIONS"].is_object()) {
                    const auto& opts = device["SPOOF_OPTIONS"];
                    if (opts.contains("controls") && opts["controls"].is_object()) {
                        const auto& controls = opts["controls"];
                        if (controls.value("pfSerial", false) && controls.contains("pfSerialVal") && controls["pfSerialVal"].is_string()) {
                            const std::string optionSerial = trim(controls["pfSerialVal"].get<std::string>());
                            if (!optionSerial.empty()) spoof_info.serial = optionSerial;
                        }
                    }
                }
                spoof_info.version_incremental = device.value("INCREMENTAL", "");
                spoof_info.version_security_patch = device.value("SECURITY_PATCH", "");
                if (device.contains("TIMESTAMP")) {
                    const auto& device_timestamp = device["TIMESTAMP"];
                    spoof_info.time = std::stoll(device_timestamp.get<std::string>()) * 1000;
                }

                // --- the version group, and only what the semaphore lets through ---
                const RomVersion rom = readRomVersion();
                const VersionPolicy policy = readVersionPolicy();
                auto allowed = [&rom, policy](const char* field, const std::string& value) {
                    if (policy == VersionPolicy::Force) return true;
                    if (policy == VersionPolicy::Never) return false;
                    // Rom: never above the ROM. Raising the SDK is what makes apps call APIs
                    // the framework does not have; lowering it only makes them ask for less.
                    const std::string f(field);
                    if (f == "SDK_INT" || f == "SDK_FULL") {
                        if (rom.sdk == 0) return false;
                        try { return std::stoi(value) <= rom.sdk; }
                        catch (const std::exception&) { return false; }
                    }
                    if (f == "ANDROID_VERSION") return !rom.release.empty() && value == rom.release;
                    if (f == "CODENAME") return !rom.codename.empty() && value == rom.codename;
                    return false;
                };

                const std::string cfg_codename = device.value("CODENAME", "");
                if (!trim(cfg_codename).empty() && allowed("CODENAME", cfg_codename)) {
                    spoof_info.version_codename = cfg_codename;
                }

                if (device.contains("ANDROID_VERSION")) {
                    const std::string value = device["ANDROID_VERSION"].get<std::string>();
                    if (allowed("ANDROID_VERSION", value)) spoof_info.android_version = value;
                }

                if (device.contains("SDK_INT")) {
                    const std::string value = device["SDK_INT"].get<std::string>();
                    if (allowed("SDK_INT", value)) {
                        spoof_info.version_sdk_int = std::stoi(value);
                        spoof_info.version_sdk = std::to_string(spoof_info.version_sdk_int);
                    }
                }

                if (device.contains("SDK_FULL")) {
                    const std::string value = device["SDK_FULL"].get<std::string>();
                    if (allowed("SDK_FULL", value)) {
                        auto dot_position = value.find('.');
                        int major = std::stoi(dot_position == std::string::npos ? value : value.substr(0, dot_position));
                        int minor = 0;
                        if (dot_position != std::string::npos) {
                            minor = std::stoi(value.substr(dot_position + 1));
                        }
                        spoof_info.version_sdk_int_full = major * 100000 + minor;
                    }
                }
                if (!spoof_info.version_sdk_int_full && spoof_info.version_sdk_int) {
                    spoof_info.version_sdk_int_full = spoof_info.version_sdk_int * 100000;
                }

                // Derived from what actually got through, by the AOSP rule. Left empty when the
                // version is not spoofed at all, so the framework keeps its own correct values.
                if (!spoof_info.version_codename.empty() || !spoof_info.android_version.empty()) {
                    const std::string cod = spoof_info.version_codename.empty()
                                          ? rom.codename : spoof_info.version_codename;
                    const std::string rel = spoof_info.android_version.empty()
                                          ? rom.release : spoof_info.android_version;
                    spoof_info.version_release_or_codename = releaseOrCodename(cod, rel);
                    spoof_info.version_release_or_preview_display = spoof_info.version_release_or_codename;
                }
            }
        } catch (const std::exception& e) {
            ERROR_LOG("Config error: %s", e.what());
            env->DeleteLocalRef(buildClass);
            if (versionClass) env->DeleteLocalRef(versionClass);
            return;
        }

        auto setStr = [this](jclass thisClass, jfieldID field, const std::string& value) {
            if (!field || trim(value).empty()) return;
            jstring js = env->NewStringUTF(value.c_str());
            if (!js || env->ExceptionCheck()) {
                env->ExceptionClear();
                return;
            }
            env->SetStaticObjectField(thisClass, field, js);
            env->DeleteLocalRef(js);
            if (env->ExceptionCheck()) env->ExceptionClear();
        };

        auto setInt = [this](jclass thisClass, jfieldID field, int value) {
            if (!field || value == 0) return;
            env->SetStaticIntField(thisClass, field, value);
            if (env->ExceptionCheck()) env->ExceptionClear();
        };

        auto setLong = [this](jclass thisClass, jfieldID field, int64_t value) {
            if (!field || value == 0) return;
            env->SetStaticLongField(thisClass, field, value);
            if (env->ExceptionCheck()) env->ExceptionClear();
        };

        setStr(buildClass, build_modelField, spoof_info.model);
        setStr(buildClass, build_brandField, spoof_info.brand);
        setStr(buildClass, build_deviceField, spoof_info.device);
        setStr(buildClass, build_manufacturerField, spoof_info.manufacturer);
        setStr(buildClass, build_fingerprintField, spoof_info.fingerprint);
        setStr(buildClass, build_productField, spoof_info.product);
        setStr(buildClass, build_boardField, spoof_info.board);
        setStr(buildClass, build_bootloaderField, spoof_info.bootloader);
        setStr(buildClass, build_hardwareField, spoof_info.hardware);
        setStr(buildClass, build_idField, spoof_info.id);
        setStr(buildClass, build_displayField, spoof_info.display);
        setStr(buildClass, build_hostField, spoof_info.host);
        setStr(buildClass, build_odm_skuField, spoof_info.odm_sku);
        setStr(buildClass, build_skuField, spoof_info.sku);
        setStr(buildClass, build_userField, spoof_info.user);
        setStr(buildClass, build_serialField, spoof_info.serial);
        setStr(buildClass, build_soc_manufacturerField, spoof_info.soc_manufacturer);
        setStr(buildClass, build_soc_modelField, spoof_info.soc_model);
        setLong(buildClass, build_timeField, spoof_info.time);
        setStr(buildClass, build_tagsField, "release-keys");
        setStr(buildClass, build_typeField, "user");

        if (versionClass) {
            setStr(versionClass, build_version_codenameField, spoof_info.version_codename);
            setStr(versionClass, build_version_incrementalField, spoof_info.version_incremental);
            setStr(versionClass, build_version_sdkField, spoof_info.version_sdk);
            setInt(versionClass, build_version_sdk_int_fullField, spoof_info.version_sdk_int_full);
            setStr(versionClass, build_version_security_patchField, spoof_info.version_security_patch);
            setStr(versionClass, build_version_releaseField, spoof_info.android_version);
            setInt(versionClass, build_version_sdk_intField, spoof_info.version_sdk_int);
            setStr(versionClass, build_version_release_or_codenameField, spoof_info.version_release_or_codename);
            setStr(versionClass, build_version_release_or_preview_displayField, spoof_info.version_release_or_preview_display);
        }

        env->DeleteLocalRef(buildClass);
        if (versionClass) env->DeleteLocalRef(versionClass);
    }

    // Baseband hook: system_server owns the framework-facing telephony/baseband properties.
    // The boot service also mirrors these values with resetprop so the read-only ro.baseband
    // property is available before Android services start.
    static bool readBasebandConfig(std::string& out) {
        out = effectiveDeviceValue("BASEBAND");
        if (out.empty()) out = readGlobalDeviceProperty("ro.baseband");
        if (out.empty()) out = readConfigString("COPG", "BASEBAND");
        if (out.empty() || out == "Undefined") {
            LOGE("No usable BASEBAND/RADIO value found in selected device profile, global-device.props, or legacy COPG config");
            return false;
        }
        LOGE("Resolved baseband override from active device profile: %s", out.c_str());
        return true;
    }

    static void applyBasebandHook(JNIEnv* env, const std::string& baseband) {
        if (!env || baseband.empty() || baseband == "Undefined") return;
        // Keep both paths for old and new releases. SystemProperties covers direct property
        // reads while TelephonyProperties covers framework-generated telephony accessors.
        callTelephonyStringListSetter(env, "basebandVersion", baseband);
        setSystemProperty(env, "gsm.version.baseband", baseband);
        setSystemProperty(env, "ro.baseband", baseband);
    }

    void startCountryWatcher() {
        JavaVM* vm = jvm;
        if (!vm) return;
        std::thread([vm]() {
            std::string lastSignature;
            std::string lastBaseband;
            for (;;) {
                CountryInfo country;
                bool valid = readCountryConfig(country);
                std::string baseband;
                bool basebandValid = readBasebandConfig(baseband);
                std::string signature;
                if (valid) {
                    signature = (country.iso_enabled ? "1" : "0") + std::string("|") +
                                (country.mccmnc_enabled ? "1" : "0") + "|" +
                                (country.operator_name_enabled ? "1" : "0") + "|" +
                                (country.operator_numeric_enabled ? "1" : "0") + "|" +
                                country.iso + "|" + country.mccmnc + "|" +
                                country.operator_name + "|" + country.operator_numeric + "|" +
                                std::to_string(country.latitude) + "|" + std::to_string(country.longitude);
                }
                if ((valid && signature != lastSignature) || (basebandValid && baseband != lastBaseband)) {
                    JNIEnv* threadEnv = nullptr;
                    #if defined(__ANDROID__)
                    if (vm->AttachCurrentThread(&threadEnv, nullptr) == JNI_OK && threadEnv) {
#else
                    if (vm->AttachCurrentThread(reinterpret_cast<void**>(&threadEnv), nullptr) == JNI_OK && threadEnv) {
#endif
                        if (valid && signature != lastSignature) {
                            applyCountryProperties(threadEnv, country);
                            if (country.iso_enabled && !country.iso.empty()) {
                                applyCountryLocation(threadEnv, country);
                            }
                        }
                        if (basebandValid && baseband != lastBaseband) {
                            applyBasebandHook(threadEnv, baseband);
                        }
                        vm->DetachCurrentThread();
                    }
                    if (valid) lastSignature = signature;
                    if (basebandValid) lastBaseband = baseband;
                }
                sleep(5);
            }
        }).detach();
    }

private:
    void installSystemPropertiesHook(JNIEnv* env) {
        if (!api || !env) return;
        JNINativeMethod methods[] = {
            {"native_get", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", (void*) hookSystemPropertiesNativeGet},
            {"native_get", "(J)Ljava/lang/String;", (void*) hookSystemPropertiesNativeGetHandle}
        };
        api->hookJniNativeMethods(env, "android/os/SystemProperties", methods, 2);
        g_originalSystemPropertiesNativeGet = reinterpret_cast<SystemPropertiesNativeGetFn>(methods[0].fnPtr);
        g_originalSystemPropertiesNativeGetHandle = reinterpret_cast<SystemPropertiesNativeGetHandleFn>(methods[1].fnPtr);
    }

    void applyRuntimeTelephony(JNIEnv* env) {
        if (!env) return;
        CountryInfo country;
        if (readCountryConfig(country)) applyCountryProperties(env, country);
        std::string baseband;
        if (readBasebandConfig(baseband)) applyBasebandHook(env, baseband);
    }

public:
    void onLoad(zygisk::Api* api, JNIEnv* env) override {
        this->api = api;
        g_zygiskApi = api;
        this->env = env;
        env->GetJavaVM(&this->jvm);

        spoofDevice();
    }

    void postServerSpecialize(const zygisk::ServerSpecializeArgs* /*args*/) override {
        installSystemPropertiesHook(env);
        applyRuntimeTelephony(env);
        if (!spoof_info.baseband.empty() && spoof_info.baseband != "Undefined") {
            applyBasebandHook(env, spoof_info.baseband);
        }
        startCountryWatcher();
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs* /*args*/) override {
        // Re-apply the framework-facing values inside each app process. This is important for
        // TelephonyManager callers because some releases resolve the generated TelephonyProperties
        // in-process and therefore can otherwise retain the original value after boot.
        installSystemPropertiesHook(env);
        applyRuntimeTelephony(env);
        startSubscriptionInfoWatcher(jvm);
    }
};

REGISTER_ZYGISK_MODULE(COPGVDModule)
