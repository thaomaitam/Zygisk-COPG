#include <android/log.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

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
    
    // Additional properties for comprehensive spoofing
    std::string board;
    std::string hardware;
    std::string serial;
    
    bool isEmpty() const {
        return brand.empty() && device.empty() && manufacturer.empty() && 
               model.empty() && fingerprint.empty() && product.empty();
    }
    
    void clear() {
        brand.clear();
        device.clear();
        manufacturer.clear();
        model.clear();
        fingerprint.clear();
        product.clear();
        board.clear();
        hardware.clear();
        serial.clear();
    }
};

// -----------------------------------------------------------
// System property spoofing utilities
// -----------------------------------------------------------
class PropertySpoofManager {
public:
    static void spoofProperty(const std::string& propName, const std::string& value) {
        if (value.empty()) return;
        
        int result = __system_property_set(propName.c_str(), value.c_str());
        if (result == 0) {
            LOGD("Successfully set property '%s' = '%s'", propName.c_str(), value.c_str());
        } else {
            LOGE("Failed to set property '%s' = '%s' (error: %d)", 
                 propName.c_str(), value.c_str(), result);
        }
    }
    
    static void spoofComprehensiveProperties(const DeviceConfig& config) {
        LOGD("Initiating comprehensive property spoofing for: %s", config.model.c_str());
        
        // Core product properties
        spoofProperty("ro.product.brand", config.brand);
        spoofProperty("ro.product.device", config.device);
        spoofProperty("ro.product.manufacturer", config.manufacturer);
        spoofProperty("ro.product.model", config.model);
        spoofProperty("ro.product.name", config.product);
        
        // Build properties
        spoofProperty("ro.build.fingerprint", config.fingerprint);
        spoofProperty("ro.build.product", config.product);
        
        // Additional hardware properties
        if (!config.board.empty()) {
            spoofProperty("ro.product.board", config.board);
        }
        if (!config.hardware.empty()) {
            spoofProperty("ro.hardware", config.hardware);
        }
        if (!config.serial.empty()) {
            spoofProperty("ro.serialno", config.serial);
        }
        
        // Vendor-specific properties for comprehensive spoofing
        spoofProperty("ro.product.vendor.brand", config.brand);
        spoofProperty("ro.product.vendor.device", config.device);
        spoofProperty("ro.product.vendor.manufacturer", config.manufacturer);
        spoofProperty("ro.product.vendor.model", config.model);
        spoofProperty("ro.product.vendor.name", config.product);
        
        // System properties
        spoofProperty("ro.product.system.brand", config.brand);
        spoofProperty("ro.product.system.device", config.device);
        spoofProperty("ro.product.system.manufacturer", config.manufacturer);
        spoofProperty("ro.product.system.model", config.model);
        spoofProperty("ro.product.system.name", config.product);
        
        LOGD("Property spoofing completed successfully");
    }
};

// -----------------------------------------------------------
// Build field manipulation utilities
// -----------------------------------------------------------
class BuildFieldManager {
private:
    JNIEnv* env;
    jclass buildClass;
    jclass versionClass;
    bool initialized;

public:
    BuildFieldManager(JNIEnv* environment) : env(environment), buildClass(nullptr), 
                                           versionClass(nullptr), initialized(false) {
        if (!env) {
            LOGE("Invalid JNIEnv provided to BuildFieldManager");
            return;
        }
        
        buildClass = env->FindClass("android/os/Build");
        versionClass = env->FindClass("android/os/Build$VERSION");
        
        if (!buildClass || !versionClass) {
            LOGE("Critical error: Failed to find Build classes");
            initialized = false;
        } else {
            initialized = true;
        }
    }
    
    ~BuildFieldManager() {
        if (buildClass && env) env->DeleteLocalRef(buildClass);
        if (versionClass && env) env->DeleteLocalRef(versionClass);
    }
    
    bool updateAllFields(const DeviceConfig& config) {
        if (!initialized) {
            LOGE("BuildFieldManager not properly initialized");
            return false;
        }
        
        if (config.isEmpty()) {
            LOGE("Device configuration is empty, skipping Build field updates");
            return false;
        }
        
        LOGD("Updating Build fields with device configuration");
        
        setBuildField("BRAND", config.brand);
        setBuildField("DEVICE", config.device);
        setBuildField("MANUFACTURER", config.manufacturer);
        setBuildField("MODEL", config.model);
        setBuildField("FINGERPRINT", config.fingerprint);
        setBuildField("PRODUCT", config.product);
        
        // Extended fields
        setBuildField("BOARD", config.board);
        setBuildField("HARDWARE", config.hardware);
        setBuildField("SERIAL", config.serial);
        
        LOGD("Build field updates completed");
        return true;
    }

private:
    void setBuildField(const char* fieldName, const std::string& value) {
        if (!initialized || value.empty()) {
            if (value.empty()) {
                LOGD("Skipping empty field: %s", fieldName);
            }
            return;
        }

        jfieldID fieldID = env->GetStaticFieldID(buildClass, fieldName, "Ljava/lang/String;");
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            fieldID = env->GetStaticFieldID(versionClass, fieldName, "Ljava/lang/String;");
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                LOGD("Field '%s' not found in Build or VERSION classes", fieldName);
                return;
            }
        }

        if (fieldID != nullptr) {
            jstring jValue = env->NewStringUTF(value.c_str());
            if (!jValue) {
                LOGE("Failed to create jstring for field '%s'", fieldName);
                return;
            }
            
            env->SetStaticObjectField(buildClass, fieldID, jValue);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                LOGE("Failed to set field '%s'", fieldName);
                env->DeleteLocalRef(jValue);
                return;
            }
            
            LOGD("Successfully set Java field '%s' = '%s'", fieldName, value.c_str());
            env->DeleteLocalRef(jValue);
        }
    }
};

// -----------------------------------------------------------
// Main Module Class
// -----------------------------------------------------------
class CombinedSpoofModule : public zygisk::ModuleBase {
public:
    CombinedSpoofModule() : api(nullptr), env(nullptr) {
        // Initialize with empty configuration
        deviceConfig.clear();
    }

    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
        LOGD("CombinedSpoofModule onLoad => module loaded successfully!");
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        if (!args) {
            LOGE("preAppSpecialize: Invalid arguments provided");
            if (api) api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        // Extract package name with enhanced error handling
        if (!extractPackageName(args)) {
            LOGE("Failed to extract package name");
            if (api) api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        LOGD("preAppSpecialize => packageName = %s", packageName.c_str());

        // Load and parse configuration
        if (!loadConfiguration()) {
            LOGE("Failed to load configuration");
            if (api) api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        // Parse configuration and check if package is targeted
        if (!parseConfiguration()) {
            LOGD("Package [%s] not found in configuration => closing module", packageName.c_str());
            if (api) api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        // Force unmount DenyList for comprehensive spoofing
        if (api) api->setOption(zygisk::FORCE_DENYLIST_UNMOUNT);

        LOGD("preAppSpecialize => keeping module active for package: %s", packageName.c_str());
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        LOGD("postAppSpecialize => Beginning spoofing operations");
        
        // Update Build fields (Java layer spoofing)
        if (env) {
            BuildFieldManager buildManager(env);
            if (buildManager.updateAllFields(deviceConfig)) {
                LOGD("Build field spoofing completed successfully");
            } else {
                LOGE("Build field spoofing encountered errors");
            }
        } else {
            LOGE("JNIEnv is null, skipping Build field spoofing");
        }

        // Spoof native system properties
        PropertySpoofManager::spoofComprehensiveProperties(deviceConfig);
        
        LOGD("postAppSpecialize => All spoofing operations completed");

        // Cleanup resources
        configJson.clear();
        deviceConfig.clear();
        packageName.clear();
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs *args) override {
        LOGD("preServerSpecialize => Closing module for system server");
        if (api) api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

private:
    zygisk::Api *api;
    JNIEnv *env;
    std::string packageName;
    nlohmann::json configJson;
    DeviceConfig deviceConfig;

    bool extractPackageName(zygisk::AppSpecializeArgs *args) {
        if (!env || !args || !args->app_data_dir) {
            LOGE("Invalid arguments for package name extraction");
            return false;
        }
        
        auto dir = env->GetStringUTFChars(args->app_data_dir, nullptr);
        if (!dir) {
            LOGE("Failed to get app data directory");
            return false;
        }
        
        std::string dataDir(dir);
        env->ReleaseStringUTFChars(args->app_data_dir, dir);

        size_t pos = dataDir.rfind('/');
        if (pos == std::string::npos || pos + 1 >= dataDir.size()) {
            LOGE("Invalid app data directory format: %s", dataDir.c_str());
            return false;
        }
        
        packageName = dataDir.substr(pos + 1);

        // Remove subprocess suffix if present
        pos = packageName.find(':');
        if (pos != std::string::npos) {
            packageName = packageName.substr(0, pos);
        }
        
        return !packageName.empty();
    }

    bool loadConfiguration() {
        if (!api) {
            LOGE("API not available for companion connection");
            return false;
        }
        
        // Connect to companion and receive JSON configuration
        int fd = api->connectCompanion();
        if (fd < 0) {
            LOGE("Failed to connect to companion process");
            return false;
        }

        int jsonSize = 0;
        if (xread(fd, &jsonSize, sizeof(jsonSize)) != sizeof(jsonSize)) {
            LOGE("Failed to read JSON size from companion");
            close(fd);
            return false;
        }

        std::string jsonStr;
        if (jsonSize > 0) {
            jsonStr.resize(jsonSize);
            if (xread(fd, jsonStr.data(), jsonSize) != jsonSize) {
                LOGE("Failed to read complete JSON data from companion");
                close(fd);
                return false;
            }
            
            // Parse JSON with comprehensive error checking
            configJson = nlohmann::json::parse(jsonStr, nullptr, false, true);
            if (configJson.is_discarded()) {
                LOGE("Failed to parse JSON configuration - invalid format");
                close(fd);
                return false;
            }
        }
        
        close(fd);
        return true;
    }

    // Enhanced configuration parsing with better error handling
    bool parseConfiguration() {
        if (!configJson.is_object()) {
            LOGE("Configuration JSON is not a valid object");
            return false;
        }

        // Find which device group this package belongs to
        std::string deviceGroup = findDeviceGroup();
        if (deviceGroup.empty()) {
            LOGD("Package %s not found in any package list", packageName.c_str());
            return false;
        }

        // Load corresponding device configuration
        return loadDeviceConfiguration(deviceGroup);
    }

    std::string findDeviceGroup() {
        for (auto& [key, value] : configJson.items()) {
            if (!value.is_array() || key.find("PACKAGES_") != 0) {
                continue;
            }
            
            for (const auto& pkg : value) {
                if (pkg.is_string() && pkg.get<std::string>() == packageName) {
                    // Extract device group name (remove "PACKAGES_" prefix)
                    return key.substr(9); // "PACKAGES_".length() = 9
                }
            }
        }
        return "";
    }

    bool loadDeviceConfiguration(const std::string& deviceGroup) {
        std::string deviceConfigKey = "PACKAGES_" + deviceGroup + "_DEVICE";
        
        if (!configJson.contains(deviceConfigKey)) {
            LOGE("Device configuration %s not found", deviceConfigKey.c_str());
            return false;
        }

        auto deviceConfigNode = configJson[deviceConfigKey];
        if (!deviceConfigNode.is_object()) {
            LOGE("Device configuration %s is not a valid object", deviceConfigKey.c_str());
            return false;
        }

        // Parse device configuration with validation
        parseDeviceConfig(deviceConfigNode);
        
        LOGD("Package %s successfully matched to device group: %s", 
             packageName.c_str(), deviceGroup.c_str());
        return true;
    }

    void parseDeviceConfig(const nlohmann::json& config) {
        // Core device properties
        parseConfigField(config, "BRAND", deviceConfig.brand);
        parseConfigField(config, "DEVICE", deviceConfig.device);
        parseConfigField(config, "MANUFACTURER", deviceConfig.manufacturer);
        parseConfigField(config, "MODEL", deviceConfig.model);
        parseConfigField(config, "FINGERPRINT", deviceConfig.fingerprint);
        parseConfigField(config, "PRODUCT", deviceConfig.product);
        
        // Extended properties
        parseConfigField(config, "BOARD", deviceConfig.board);
        parseConfigField(config, "HARDWARE", deviceConfig.hardware);
        parseConfigField(config, "SERIAL", deviceConfig.serial);

        LOGD("Device configuration loaded successfully:");
        LOGD("  Brand: %s, Model: %s, Device: %s", 
             deviceConfig.brand.c_str(), deviceConfig.model.c_str(), deviceConfig.device.c_str());
        LOGD("  Manufacturer: %s, Product: %s", 
             deviceConfig.manufacturer.c_str(), deviceConfig.product.c_str());
    }

    void parseConfigField(const nlohmann::json& config, const char* key, std::string& target) {
        if (config.contains(key) && config[key].is_string()) {
            target = config[key].get<std::string>();
        }
    }
};

// -----------------------------------------------------------
// File reading utilities with enhanced error handling
// -----------------------------------------------------------
static std::vector<uint8_t> readFile(const char *path) {
    if (!path) {
        LOGE("Invalid path provided to readFile");
        return {};
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        LOGE("Failed to open configuration file: %s (error: %s)", path, strerror(errno));
        return {};
    }

    // Get file size
    if (fseek(file, 0, SEEK_END) != 0) {
        LOGE("Failed to seek to end of file: %s", path);
        fclose(file);
        return {};
    }

    long size = ftell(file);
    if (size < 0) {
        LOGE("Failed to get file size: %s", path);
        fclose(file);
        return {};
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        LOGE("Failed to seek to beginning of file: %s", path);
        fclose(file);
        return {};
    }

    if (size == 0) {
        LOGD("Configuration file is empty: %s", path);
        fclose(file);
        return {};
    }

    // Read file contents
    std::vector<uint8_t> buffer(size);
    size_t bytesRead = fread(buffer.data(), 1, size, file);
    fclose(file);

    if (bytesRead != static_cast<size_t>(size)) {
        LOGE("Failed to read complete file: %s (read %zu/%ld bytes)", 
             path, bytesRead, size);
        return {};
    }

    LOGD("Successfully read configuration file: %s (%ld bytes)", path, size);
    return buffer;
}

// Enhanced companion function with better error handling
static void companion(int fd) {
    LOGD("Companion process started, reading configuration");
    
    if (fd < 0) {
        LOGE("Invalid file descriptor provided to companion");
        return;
    }
    
    std::vector<uint8_t> jsonData = readFile(CONFIG_PATH);
    int jsonSize = static_cast<int>(jsonData.size());

    LOGD("Companion sending JSON data (size: %d bytes)", jsonSize);

    // Send size first
    if (xwrite(fd, &jsonSize, sizeof(jsonSize)) != sizeof(jsonSize)) {
        LOGE("Companion failed to send JSON size");
        return;
    }

    // Send data if available
    if (jsonSize > 0) {
        if (xwrite(fd, jsonData.data(), jsonSize) != jsonSize) {
            LOGE("Companion failed to send complete JSON data");
            return;
        }
    }

    LOGD("Companion successfully sent configuration data");
}

// Register Zygisk module and companion
REGISTER_ZYGISK_MODULE(CombinedSpoofModule)
REGISTER_ZYGISK_COMPANION(companion)