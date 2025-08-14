#include <android/log.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <string_view>

#include "zygisk.hpp"

#define JSON_NOEXCEPTION 1
#define JSON_NO_IO 1
#include "json.hpp"

#define LOG_TAG "CombinedSpoofModule"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define CONFIG_PATH "/data/adb/modules/COPG/config.json"

#include <sys/system_properties.h>

// -----------------------------------------------------------
// Safe read/write functions
// -----------------------------------------------------------
static ssize_t xread(int fd, void *buffer, size_t count) {
    ssize_t total = 0;
    char *buf = (char *) buffer;
    while (count > 0) {
        ssize_t ret = TEMP_FAILURE_RETRY(read(fd, buf, count));
        if (ret < 0) return -1;
        buf += ret;
        total += ret;
        count -= ret;
    }
    return total;
}

static ssize_t xwrite(int fd, const void *buffer, size_t count) {
    ssize_t total = 0;
    char *buf = (char *) buffer;
    while (count > 0) {
        ssize_t ret = TEMP_FAILURE_RETRY(write(fd, buf, count));
        if (ret < 0) return -1;
        buf += ret;
        total += ret;
        count -= ret;
    }
    return total;
}

// -----------------------------------------------------------
// Device configuration structure
// -----------------------------------------------------------
struct DeviceConfig {
    std::string brand;
    std::string device;
    std::string manufacturer;
    std::string model;
    std::string fingerprint;
    std::string product;
};

// -----------------------------------------------------------
// Main Module Class
// -----------------------------------------------------------
class CombinedSpoofModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
        LOGD("CombinedSpoofModule onLoad => module loaded!");
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        if (!args) {
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        // Extract package name from app_data_dir
        auto dir = env->GetStringUTFChars(args->app_data_dir, nullptr);
        if (!dir) {
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }
        std::string dataDir(dir);
        env->ReleaseStringUTFChars(args->app_data_dir, dir);

        size_t pos = dataDir.rfind('/');
        if (pos != std::string::npos && pos + 1 < dataDir.size()) {
            packageName = dataDir.substr(pos + 1);
        }

        // Remove subprocess suffix if present
        pos = packageName.find(':');
        if (pos != std::string::npos) {
            packageName = packageName.substr(0, pos);
        }

        LOGD("preAppSpecialize => packageName = %s", packageName.c_str());

        // Connect to companion and receive JSON configuration
        int fd = api->connectCompanion();
        int jsonSize = 0;
        xread(fd, &jsonSize, sizeof(jsonSize));

        std::string jsonStr;
        if (jsonSize > 0) {
            jsonStr.resize(jsonSize);
            xread(fd, jsonStr.data(), jsonSize);
            
            // Parse JSON with error checking (no exceptions)
            configJson = nlohmann::json::parse(jsonStr, nullptr, false, true);
            if (configJson.is_discarded()) {
                LOGE("Failed to parse JSON configuration");
                api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
                return;
            }
        }

        // Parse configuration and check if package is targeted
        if (!parseConfiguration()) {
            LOGD("Package [%s] not found in configuration => close module", packageName.c_str());
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        // Force unmount DenyList
        api->setOption(zygisk::FORCE_DENYLIST_UNMOUNT);

        LOGD("preAppSpecialize => keeping module for package: %s", packageName.c_str());
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        // Update Build fields (Java)
        updateBuildFields();

        // Spoof native system properties
        spoofSystemProperties();

        // Cleanup
        configJson.clear();
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs *args) override {
        LOGD("preServerSpecialize => DLCLOSE");
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

private:
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;
    std::string packageName;
    nlohmann::json configJson;
    DeviceConfig deviceConfig;

    // Parse new JSON structure: "PACKAGES_EXAMPLE": [], "PACKAGES_EXAMPLE_DEVICE": {...}
    bool parseConfiguration() {
        if (!configJson.is_object()) {
            LOGE("Configuration JSON is not an object");
            return false;
        }

        // Find which device group this package belongs to
        std::string deviceGroup;
        for (auto& [key, value] : configJson.items()) {
            if (value.is_array() && key.find("PACKAGES_") == 0) {
                for (const auto& pkg : value) {
                    if (pkg.is_string() && pkg.get<std::string>() == packageName) {
                        // Extract device group name (remove "PACKAGES_" prefix)
                        deviceGroup = key.substr(9); // "PACKAGES_".length() = 9
                        break;
                    }
                }
                if (!deviceGroup.empty()) break;
            }
        }

        if (deviceGroup.empty()) {
            LOGD("Package %s not found in any package list", packageName.c_str());
            return false;
        }

        // Look for corresponding device configuration
        std::string deviceConfigKey = "PACKAGES_" + deviceGroup + "_DEVICE";
        if (!configJson.contains(deviceConfigKey)) {
            LOGE("Device configuration %s not found", deviceConfigKey.c_str());
            return false;
        }

        auto deviceConfigNode = configJson[deviceConfigKey];
        if (!deviceConfigNode.is_object()) {
            LOGE("Device configuration %s is not an object", deviceConfigKey.c_str());
            return false;
        }

        // Parse device configuration
        parseDeviceConfig(deviceConfigNode);
        
        LOGD("Package %s matched to device group: %s", packageName.c_str(), deviceGroup.c_str());
        return true;
    }

    void parseDeviceConfig(const nlohmann::json& config) {
        // Parse device properties
        if (config.contains("BRAND") && config["BRAND"].is_string()) {
            deviceConfig.brand = config["BRAND"].get<std::string>();
        }
        if (config.contains("DEVICE") && config["DEVICE"].is_string()) {
            deviceConfig.device = config["DEVICE"].get<std::string>();
        }
        if (config.contains("MANUFACTURER") && config["MANUFACTURER"].is_string()) {
            deviceConfig.manufacturer = config["MANUFACTURER"].get<std::string>();
        }
        if (config.contains("MODEL") && config["MODEL"].is_string()) {
            deviceConfig.model = config["MODEL"].get<std::string>();
        }
        if (config.contains("FINGERPRINT") && config["FINGERPRINT"].is_string()) {
            deviceConfig.fingerprint = config["FINGERPRINT"].get<std::string>();
        }
        if (config.contains("PRODUCT") && config["PRODUCT"].is_string()) {
            deviceConfig.product = config["PRODUCT"].get<std::string>();
        }

        LOGD("Device config loaded - Brand: %s, Model: %s, Device: %s", 
             deviceConfig.brand.c_str(), deviceConfig.model.c_str(), deviceConfig.device.c_str());
    }

    void updateBuildFields() {
        jclass buildClass = env->FindClass("android/os/Build");
        jclass versionClass = env->FindClass("android/os/Build$VERSION");

        if (!buildClass || !versionClass) {
            LOGE("Failed to find Build classes");
            return;
        }

        // Update Build fields with device config
        setBuildField(buildClass, versionClass, "BRAND", deviceConfig.brand);
        setBuildField(buildClass, versionClass, "DEVICE", deviceConfig.device);
        setBuildField(buildClass, versionClass, "MANUFACTURER", deviceConfig.manufacturer);
        setBuildField(buildClass, versionClass, "MODEL", deviceConfig.model);
        setBuildField(buildClass, versionClass, "FINGERPRINT", deviceConfig.fingerprint);
        setBuildField(buildClass, versionClass, "PRODUCT", deviceConfig.product);

        env->DeleteLocalRef(buildClass);
        env->DeleteLocalRef(versionClass);
    }

    void setBuildField(jclass buildClass, jclass versionClass, const char* fieldName, const std::string& value) {
        if (value.empty()) return;

        jfieldID fieldID = env->GetStaticFieldID(buildClass, fieldName, "Ljava/lang/String;");
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            fieldID = env->GetStaticFieldID(versionClass, fieldName, "Ljava/lang/String;");
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                return;
            }
        }

        if (fieldID != nullptr) {
            jstring jValue = env->NewStringUTF(value.c_str());
            env->SetStaticObjectField(buildClass, fieldID, jValue);  // Use buildClass for setting, as fields are in Build
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                env->DeleteLocalRef(jValue);
                return;
            }
            LOGD("Set Java '%s' to '%s'", fieldName, value.c_str());
            env->DeleteLocalRef(jValue);
        }
    }

    void spoofSystemProperties() {
        LOGD("Spoofing system properties for: %s", deviceConfig.model.c_str());
        if (!deviceConfig.brand.empty()) __system_property_set("ro.product.brand", deviceConfig.brand.c_str());
        if (!deviceConfig.device.empty()) __system_property_set("ro.product.device", deviceConfig.device.c_str());
        if (!deviceConfig.manufacturer.empty()) __system_property_set("ro.product.manufacturer", deviceConfig.manufacturer.c_str());
        if (!deviceConfig.model.empty()) __system_property_set("ro.product.model", deviceConfig.model.c_str());
        if (!deviceConfig.fingerprint.empty()) __system_property_set("ro.build.fingerprint", deviceConfig.fingerprint.c_str());
        if (!deviceConfig.product.empty()) __system_property_set("ro.product.product", deviceConfig.product.c_str());  // Note: Typical prop is ro.product.name or similar, but matching Module A
    }
};

// -----------------------------------------------------------
// File reading function
// -----------------------------------------------------------
static std::vector<uint8_t> readFile(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        LOGE("Failed to open file: %s", path);
        return {};
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (size <= 0) {
        fclose(file);
        return {};
    }

    std::vector<uint8_t> vector(size);
    size_t read = fread(vector.data(), 1, size, file);
    fclose(file);

    if (read != static_cast<size_t>(size)) {
        LOGE("Failed to read complete file: %s", path);
        return {};
    }

    return vector;
}

// Companion function - sends JSON to module
static void companion(int fd) {
    std::vector<uint8_t> json = readFile(CONFIG_PATH);
    int jsonSize = static_cast<int>(json.size());

    LOGD("Companion sending JSON size: %d", jsonSize);

    xwrite(fd, &jsonSize, sizeof(jsonSize));
    if (jsonSize > 0) {
        xwrite(fd, json.data(), jsonSize);
    }
}

// Register Zygisk module and companion
REGISTER_ZYGISK_MODULE(CombinedSpoofModule)
REGISTER_ZYGISK_COMPANION(companion)