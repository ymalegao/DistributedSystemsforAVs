//
// V2VJVMLifecycle.cc
// Extracted JVM lifecycle and JNI bridge helpers for V2VProxyModule
//

#include "veins/modules/bftsmart/V2VProxyModule.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>

#define XXH_INLINE_ALL
#include "xxhash.h"

using namespace veins;
namespace fs = std::filesystem;

namespace {

std::string getEnvVar(const char* name)
{
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

fs::path resolveCurrentWorkingDirectory()
{
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    return ec ? fs::path() : cwd;
}

fs::path resolveBftsmartRoot()
{
    const std::string envRoot = getEnvVar("BFTSMART_ROOT");
    if (!envRoot.empty()) {
        return fs::path(envRoot);
    }

    const fs::path cwd = resolveCurrentWorkingDirectory();
    if (cwd.empty()) {
        return fs::path();
    }

    const std::vector<fs::path> candidates = {
        cwd.parent_path() / "bftsmart",
        cwd / "bftsmart",
    };
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::exists(candidate, ec) && !ec) {
            return candidate;
        }
    }
    return fs::path();
}

bool directoryExists(const fs::path& path)
{
    std::error_code ec;
    return fs::is_directory(path, ec) && !ec;
}

std::vector<fs::path> collectJarFiles(const fs::path& libDir)
{
    std::vector<fs::path> jars;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(libDir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() == ".jar") {
            jars.push_back(entry.path());
        }
    }
    std::sort(jars.begin(), jars.end());
    return jars;
}

std::string buildClasspath(
    const fs::path& classesDir,
    const fs::path& resourcesDir,
    const std::vector<fs::path>& jars)
{
    std::vector<std::string> entries;
    entries.push_back(classesDir.string());
    if (directoryExists(resourcesDir)) {
        entries.push_back(resourcesDir.string());
    }
    for (const auto& jar : jars) {
        entries.push_back(jar.string());
    }

    std::string classpath;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) {
            classpath += ":";
        }
        classpath += entries[i];
    }
    return classpath;
}

} // namespace


bool V2VProxyModule::zombieFilter() {
    if (isDeparted) {
        std::cout << "[V2VProxy " << replicaId << "] ZOMBIE: Not executing action (departed)" << "\n";
        return true;
    }
    return false;
}
Define_Module(veins::V2VProxyModule);

// Static member initialization
std::map<int, V2VProxyModule*> V2VProxyModule::replicaProxyMap;
std::mutex V2VProxyModule::registryMutex;
JavaVM* V2VProxyModule::sharedJVM = nullptr;
std::mutex V2VProxyModule::jvmMutex;


// XXHash32 wrapper for deterministic hashing across C++ and Java
int32_t computeXXHash32(const std::string& str) {
    // Use seed = 0 to match Java: xxhash.hash(dataBytes, 0, dataBytes.length, 0)
    return XXH32(str.c_str(), str.length(), 0);
}


cMessage* resumeMsg = nullptr;

V2VProxyModule* V2VProxyModule::getProxyForReplica(int replicaId)
{
    std::lock_guard<std::mutex> lock(registryMutex);
    auto it = replicaProxyMap.find(replicaId);
    if (it != replicaProxyMap.end()) {
        return it->second;
    }
    return nullptr;
}

// Called from JNI when Java wants to send a message

void V2VProxyModule::notifyJavaRadioReady()
{
    std::lock_guard<std::mutex> lock(jvmMutex);

    if (!sharedJVM || !javaCallbackObject) return;

    JNIEnvGuard jniGuard(sharedJVM);
    if (!jniGuard.valid()) return;
    JNIEnv* env = jniGuard.env;

    if (!onRadioReadyMethod) {
        jclass cls = env->GetObjectClass(javaCallbackObject);
        onRadioReadyMethod = env->GetMethodID(cls, "onRadioReady", "()V");
        if (!onRadioReadyMethod) { env->ExceptionClear(); return; }
    }

    env->CallVoidMethod(javaCallbackObject, onRadioReadyMethod);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

bool V2VProxyModule::triggerJoinViaJNI(const std::string& request )
{
    std::cout << "[V2VProxy " << replicaId << "] triggerJoinViaJNI('" << request << "') called at t=" << simTime() << "\n";

    // Note: Removed joinTriggered flag check - in TPWC we need multiple consensus rounds
    // (VIEW, ORDER, then VIEW again for next batch, etc.)

    std::lock_guard<std::mutex> lock(jvmMutex);
    
    if (!sharedJVM) {
        std::cerr << "[ERROR V2VProxy " << replicaId << "] No JVM available for triggerJoin" << "\n";
        return false;
    }
    
    JNIEnvGuard jniGuard(sharedJVM);
    if (!jniGuard.valid()) {
        std::cerr << "[ERROR V2VProxy " << replicaId << "] Could not acquire JNIEnv for triggerJoin" << "\n";
        return false;
    }
    JNIEnv* env = jniGuard.env;

    jclass serverRunnerClass = env->FindClass("bftsmart/demo/intersection/ServerRunner");
    if (!serverRunnerClass) {
        std::cerr << "[ERROR V2VProxy " << replicaId << "] Failed to find ServerRunner class" << "\n";
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }

    // Check barrier status (logged occasionally to avoid spam)
    jmethodID barrierMethod = env->GetStaticMethodID(serverRunnerClass, "getBarrierStatus", "()Ljava/lang/String;");
    if (barrierMethod) {
        jstring barrierStr = (jstring) env->CallStaticObjectMethod(serverRunnerClass, barrierMethod);
        if (barrierStr) {
            const char* barrierChars = env->GetStringUTFChars(barrierStr, nullptr);
            std::string barrierStatus(barrierChars);
            env->ReleaseStringUTFChars(barrierStr, barrierChars);
            env->DeleteLocalRef(barrierStr);
            static int pollCount = 0;
            if (++pollCount % 10 == 1) {
                std::cout << "[V2VProxy " << replicaId << "] Barrier: " << barrierStatus << "\n";
            }
        }
    }

    jmethodID statusMethod = env->GetStaticMethodID(serverRunnerClass, "getStatus", "(I)Ljava/lang/String;");
    if (statusMethod) {
        jstring statusStr = (jstring) env->CallStaticObjectMethod(serverRunnerClass, statusMethod, replicaId);
        if (!statusStr) {
            std::cerr << "[V2VProxy " << replicaId << "] ERROR: getStatus returned null" << "\n";
            return false;
        }
        const char* statusChars = env->GetStringUTFChars(statusStr, nullptr);
        std::string status(statusChars);
        env->ReleaseStringUTFChars(statusStr, statusChars);
        env->DeleteLocalRef(statusStr);
        if (status != "READY") {
            std::cout << "[V2VProxy " << replicaId << "] FAILED: Server Status = '" << status
                      << "' (expected 'READY') at t=" << simTime() << "\n";
            return false;
        }
        std::cout << "[V2VProxy " << replicaId << "] PASSED: Server Status = 'READY'" << "\n";
    }

    jmethodID readyMethod = env->GetStaticMethodID(serverRunnerClass, "isReplicaReady", "(I)Z");
    if (readyMethod) {
        jboolean isReady = env->CallStaticBooleanMethod(serverRunnerClass, readyMethod, replicaId);
        if (!isReady) {
            std::cout << "[V2VProxy " << replicaId << "] FAILED: isReplicaReady() = false at t=" << simTime() << "\n";
            return false;
        }
        std::cout << "[V2VProxy " << replicaId << "] PASSED: isReplicaReady() = true" << "\n";
    } else {
        std::cerr << "[ERROR] Could not find isReplicaReady method!" << "\n";
        return false;
    }

    jmethodID triggerMethod = env->GetStaticMethodID(serverRunnerClass, "triggerJoinForReplica", "(ILjava/lang/String;)V");
    if (!triggerMethod) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Could not find triggerJoinForReplica(I, String) method" << "\n";
        return false;
    }

    jstring jRequest = env->NewStringUTF(request.c_str());
    env->CallStaticVoidMethod(serverRunnerClass, triggerMethod, replicaId, jRequest);
    env->DeleteLocalRef(jRequest);

    if (env->ExceptionCheck()) {
        std::cerr << "[V2VProxy " << replicaId << "] Exception calling triggerJoinForReplica" << "\n";
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }

    std::cout << "[V2VProxy " << replicaId << "] SUCCESS: Triggered consensus request '"
              << request << "' at t=" << simTime() << "\n";
    return true;
}

bool V2VProxyModule::triggerGlobalResetViaJNI(const std::vector<int>& departedReplicas)
{
    std::cout << "[V2VProxy " << replicaId << "] triggerGlobalResetViaJNI() called at t=" << simTime() << "\n";

    std::lock_guard<std::mutex> lock(jvmMutex);
    
    if (!sharedJVM) {
        std::cerr << "[ERROR V2VProxy " << replicaId << "] No JVM available for triggerGlobalReset" << "\n";
        return false;
    }
    
    JNIEnv* env;
    bool attached = false;
    if (sharedJVM->GetEnv((void**)&env, JNI_VERSION_1_8) == JNI_EDETACHED) {
        sharedJVM->AttachCurrentThread((void**)&env, nullptr);
        attached = true;
    }

    // Find ReliableV2VMessaging class
    jclass messagingClass = env->FindClass("bftsmart/communication/V2V/ReliableV2VMessaging");
    if (!messagingClass) {
        std::cerr << "[ERROR V2VProxy " << replicaId << "] Failed to find ReliableV2VMessaging class" << "\n";
        env->ExceptionDescribe();
        env->ExceptionClear();
        if (attached) sharedJVM->DetachCurrentThread();
        return false;
    }

    // Get the static globalResetV2V method taking int[]
    jmethodID resetMethod = env->GetStaticMethodID(messagingClass, "globalResetV2V", "([I)V");

    bool result = false;
    if (resetMethod) {
        jintArray jDepartedArray = nullptr;
        if (!departedReplicas.empty()) {
            jDepartedArray = env->NewIntArray(departedReplicas.size());
            env->SetIntArrayRegion(jDepartedArray, 0, departedReplicas.size(), (const jint*)departedReplicas.data());
        }

        env->CallStaticVoidMethod(messagingClass, resetMethod, jDepartedArray);

        if (jDepartedArray) {
            env->DeleteLocalRef(jDepartedArray);
        }

        if (!env->ExceptionCheck()) {
            std::cout << "[V2VProxy " << replicaId << "] SUCCESS: Triggered globalResetV2V via JNI at t=" << simTime() << "\n";
            result = true;
        } else {
            std::cerr << "[V2VProxy " << replicaId << "] Exception calling globalResetV2V" << "\n";
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    } else {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Could not find globalResetV2V([I)V method" << "\n";
    }

    if (attached) sharedJVM->DetachCurrentThread();
    return result;
}

bool V2VProxyModule::warmupJVM(JNIEnv* env)
{
    try {
        jclass bridgeClass = env->FindClass("bftsmart/communication/V2V/V2VNativeBridge");
        if (!bridgeClass) {
            std::cerr << "[V2VProxy] ERROR: Failed to find V2VNativeBridge class" << "\n";
            return false;
        }
        jmethodID warmupMethod = env->GetStaticMethodID(bridgeClass, "nativeWarmupPing", "()V");
        
        if (!warmupMethod) {
            std::cerr << "[V2VProxy] ERROR: Failed to find nativeWarmupPing method" << "\n";
            return false;
    
        }

        env->CallStaticVoidMethod(bridgeClass, warmupMethod);

        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << "[V2VProxy] ERROR: Failed to warm up JVM: " << e.what() << "\n";
        return false;
    }

}

bool V2VProxyModule::createOrAttachJVM()
{
    std::lock_guard<std::mutex> lock(jvmMutex);

    // If JVM already exists, attach this thread
    if (sharedJVM != nullptr) {
        EV_INFO << "Replica " << replicaId << ": Attaching to existing JVM" << "\n";

        JNIEnv* env;
        jint result = sharedJVM->AttachCurrentThread((void**)&env, nullptr);
        if (result != JNI_OK) {
            EV_ERROR << "Failed to attach to JVM: " << result << "\n";
            return false;
        }

        jvm = sharedJVM;

        // Each replica needs its own clockClass/updateTimeMethod so syncTimeToJava()
        // works on non-first replicas. Without this, only the JVM-creating replica
        // (replica 0) can update SimulationClock; after it departs the clock freezes,
        // which makes envelope.timestampMs == now forever and suppresses all reliability
        // retransmissions for subsequent epochs.
        jclass localClockCls = env->FindClass("bftsmart/communication/V2V/SimulationClock");
        if (localClockCls) {
            clockClass = (jclass) env->NewGlobalRef(localClockCls);
            updateTimeMethod = env->GetStaticMethodID(clockClass, "updateTime", "(D)V");
            env->DeleteLocalRef(localClockCls);
        } else {
            std::cerr << "[V2VProxy " << replicaId << "] WARNING: SimulationClock class not found on attach" << "\n";
            env->ExceptionClear();
        }

        // Cache IntersectionServer static callback for type-9 CLIENT_REQUEST_V2V dispatch.
        // Must be done here (attach path) too — registerJNINativeMethods() runs only on
        // replica 0 (the JVM creator); replicas 1..N-1 enter via this attach branch.
        jclass localISCls = env->FindClass("bftsmart/demo/intersection/IntersectionServer");
        if (localISCls) {
            intersectionServerGlobalClass = static_cast<jclass>(env->NewGlobalRef(localISCls));
            env->DeleteLocalRef(localISCls);
            deliverInjectedClientRequestMethod = env->GetStaticMethodID(
                intersectionServerGlobalClass, "deliverInjectedClientRequest", "(I[B)V");
            if (!deliverInjectedClientRequestMethod) {
                std::cerr << "[V2VProxy " << replicaId << "] WARNING: deliverInjectedClientRequest not found on attach\n";
                env->ExceptionClear();
            }
        } else {
            std::cerr << "[V2VProxy " << replicaId << "] WARNING: IntersectionServer class not found on attach\n";
            env->ExceptionClear();
        }

        return true;
    }

    // Create new JVM (first replica only)
    EV_INFO << "Replica " << replicaId << ": Creating new JVM" << "\n";

    const fs::path bftsmartRoot = resolveBftsmartRoot();
    if (bftsmartRoot.empty()) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Could not resolve BFTSMART_ROOT. "
                  << "Export BFTSMART_ROOT or launch via bftsmart/run-omnet-simulation.sh." << "\n";
        return false;
    }

    const fs::path libraryRoot = bftsmartRoot / "library";
    const fs::path classesDir = libraryRoot / "build/classes/java/main";
    const fs::path resourcesDir = libraryRoot / "build/resources/main";
    const fs::path installLibDir = libraryRoot / "build/install/library/lib";
    const fs::path nativeLibDir = libraryRoot / "native/lib";
    const std::string buildHint = "cd " + libraryRoot.string() + " && ./gradlew installDist";

    if (!directoryExists(classesDir)) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Missing compiled Java classes at "
                  << classesDir << ". Run: " << buildHint << "\n";
        return false;
    }
    if (!directoryExists(installLibDir)) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Missing installDist library directory at "
                  << installLibDir << ". Run: " << buildHint << "\n";
        return false;
    }
    if (!directoryExists(nativeLibDir)) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: Missing JNI native library directory at "
                  << nativeLibDir << "\n";
        return false;
    }

    const std::vector<fs::path> jarFiles = collectJarFiles(installLibDir);
    if (jarFiles.empty()) {
        std::cerr << "[V2VProxy " << replicaId << "] ERROR: No JAR files found under "
                  << installLibDir << ". Run: " << buildHint << "\n";
        return false;
    }

    std::cout << "[V2VProxy " << replicaId << "] Using BFTSMART_ROOT=" << bftsmartRoot << "\n";

    std::vector<std::string> optionStrings;
    optionStrings.push_back("-Djava.class.path=" + buildClasspath(classesDir, resourcesDir, jarFiles));
    optionStrings.push_back("-Djava.library.path=" + nativeLibDir.string());

    // Enable verbose output for debugging
    // optionStrings.push_back("-verbose:jni");
    // optionStrings.push_back("-verbose:class");

    // Tell BFTSmart to use V2V communication instead of TCP
    optionStrings.push_back("-Dbftsmart.communication.useV2V=true");

    // Tell V2VNativeBridge we're in embedded mode (library already loaded)
    optionStrings.push_back("-Dbftsmart.jni.embedded=true");

    // Intersection physics parameters for delay calculation
    optionStrings.push_back("-Dintersection.width=" + std::to_string(intersectionWidth));
    optionStrings.push_back("-Dintersection.avgSpeed=" + std::to_string(avgSpeed));
    optionStrings.push_back("-Dintersection.safetyGap=" + std::to_string(safetyGap));

    // Memory settings
    optionStrings.push_back("-Xms256m");
    optionStrings.push_back("-Xmx1024m");

    // CRITICAL FIX: Force non-blocking entropy source for crypto operations
    // Without this, SecureRandom/ECDSA signing can block indefinitely in WSL2/VMs
    // waiting for /dev/random entropy. This uses /dev/urandom instead (non-blocking).
    optionStrings.push_back("-Djava.security.egd=file:/dev/./urandom");

    std::vector<JavaVMOption> options(optionStrings.size());
    for (size_t i = 0; i < optionStrings.size(); ++i) {
        options[i].optionString = const_cast<char*>(optionStrings[i].c_str());
    }

    JavaVMInitArgs vm_args;

    vm_args.version = JNI_VERSION_1_8;
    vm_args.nOptions = static_cast<jint>(options.size());
    vm_args.options = options.data();
    vm_args.ignoreUnrecognized = JNI_FALSE;

    JNIEnv* env;
    jint result = JNI_CreateJavaVM(&sharedJVM, (void**)&env, &vm_args);

    if (result != JNI_OK) {
        EV_ERROR << "Failed to create JVM: " << result << "\n";
        return false;
    }

    jvm = sharedJVM;
    std::cout << "[V2VProxy] JVM created successfully" << "\n";
    
    // Manually register JNI native methods (required in embedded mode)
    std::cout << "[V2VProxy] Registering JNI native methods..." << "\n";
    if (!registerJNINativeMethods(env)) {
        std::cerr << "[V2VProxy] ERROR: Failed to register JNI native methods" << "\n";
        return false;
    }
    std::cout << "[V2VProxy] JNI native methods registered successfully" << "\n";

    std::cout << "[V2VProxy] Warming up JVM (crypto + JNI) before starting replicas..." << "\n";

    if (!warmupJVM(env)) {
        std::cerr << "[V2VProxy] ERROR: Failed to warm up JVM" << "\n";
        return false;
    }
    std::cout << "[V2VProxy] JVM warmed up successfully" << "\n";
    
    return true;
}

// Forward declare JNI functions from V2VJNIBridge.cc
extern "C" {
    JNIEXPORT void JNICALL Java_bftsmart_communication_V2V_V2VNativeBridge_nativeInit(JNIEnv*, jobject, jint);
    JNIEXPORT jboolean JNICALL Java_bftsmart_communication_V2V_V2VNativeBridge_nativeSendMessage(JNIEnv*, jobject, jint, jint, jbyteArray);
    JNIEXPORT void JNICALL Java_bftsmart_communication_V2V_V2VNativeBridge_nativeShutdown(JNIEnv*, jobject, jint);
    JNIEXPORT jboolean JNICALL Java_bftsmart_communication_V2V_V2VNativeBridge_nativeIsRadioBusy(JNIEnv*, jobject, jint);
    JNIEXPORT void JNICALL Java_bftsmart_communication_V2V_V2VNativeBridge_nativeWarmupPing(JNIEnv*, jclass);
}

// Forward declarations for JNI callbacks implemented in V2VJNIBridge.cc
extern "C" {
    JNIEXPORT void JNICALL Java_bftsmart_demo_intersection_IntersectionServer_notifyViewAgreed
        (JNIEnv*, jobject, jint, jstring);
    JNIEXPORT void JNICALL Java_bftsmart_demo_intersection_IntersectionServer_notifyProposeAllConsensusMetric
        (JNIEnv*, jobject, jint, jint, jdouble);
    JNIEXPORT void JNICALL Java_bftsmart_demo_intersection_IntersectionServer_notifyVehicleCanGo
        (JNIEnv*, jobject, jint, jdouble);
    JNIEXPORT void JNICALL Java_bftsmart_demo_intersection_IntersectionServer_notifyWipeComplete
        (JNIEnv*, jobject, jint);
    JNIEXPORT jobject JNICALL Java_bftsmart_demo_intersection_IntersectionServer_nativeGetCertSnapshot
        (JNIEnv*, jobject, jint);
    JNIEXPORT jstring JNICALL Java_bftsmart_demo_intersection_IntersectionServer_nativeGetFreshProposePayload
        (JNIEnv*, jobject, jint);
    JNIEXPORT void JNICALL Java_bftsmart_demo_intersection_IntersectionServer_nativeBroadcastClientRequest
        (JNIEnv*, jobject, jint, jbyteArray);
}



extern "C" {
    JNIEXPORT void JNICALL Java_bftsmart_demo_intersection_IntersectionServer_notifyOrderDecided
    (JNIEnv*, jobject, jint, jstring);
}

// Implementation of isRadioBusy native method
// NOTE: notifyViewAgreed and notifyVehicleCanGo are implemented in V2VJNIBridge.cc
JNIEXPORT jboolean JNICALL Java_bftsmart_communication_V2V_V2VNativeBridge_nativeIsRadioBusy(JNIEnv* env, jobject obj, jint replicaId)
{
    V2VProxyModule* proxy = V2VProxyModule::getProxyForReplica(replicaId);
    if (proxy) {
        return proxy->isRadioBusy() ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;  // If no proxy, assume not busy
}

bool V2VProxyModule::registerJNINativeMethods(JNIEnv* env)
{
    // Find the V2VNativeBridge class
    jclass bridgeClass = env->FindClass("bftsmart/communication/V2V/V2VNativeBridge");
    if (!bridgeClass) {
        std::cerr << "[V2VProxy] ERROR: Failed to find V2VNativeBridge class" << "\n";
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }

    jclass localClockCls = env->FindClass("bftsmart/communication/V2V/SimulationClock");
    if (!localClockCls) {
        std::cerr << "[V2VProxy] ERROR: Failed to find SimulationClock class" << "\n";
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }
    clockClass = (jclass) env->NewGlobalRef(localClockCls);
    updateTimeMethod = env->GetStaticMethodID(clockClass, "updateTime", "(D)V");

    // Define the native methods to register
    JNINativeMethod methods[] = {
        {const_cast<char*>("nativeInit"), const_cast<char*>("(I)V"), (void*)&Java_bftsmart_communication_V2V_V2VNativeBridge_nativeInit},
        {const_cast<char*>("nativeSendMessage"), const_cast<char*>("(II[B)Z"), (void*)&Java_bftsmart_communication_V2V_V2VNativeBridge_nativeSendMessage},
        {const_cast<char*>("nativeShutdown"), const_cast<char*>("(I)V"), (void*)&Java_bftsmart_communication_V2V_V2VNativeBridge_nativeShutdown},
        {const_cast<char*>("nativeIsRadioBusy"), const_cast<char*>("(I)Z"), (void*)&Java_bftsmart_communication_V2V_V2VNativeBridge_nativeIsRadioBusy},
        {const_cast<char*>("nativeWarmupPing"), const_cast<char*>("()V"), (void*)&Java_bftsmart_communication_V2V_V2VNativeBridge_nativeWarmupPing}

    };

    // Register the methods
    if (env->RegisterNatives(bridgeClass, methods, 5) != 0) {
        std::cerr << "[V2VProxy] ERROR: Failed to register native methods" << "\n";
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }

    std::cout << "[V2VProxy] Successfully registered 4 JNI native methods" << "\n";
    // return true;

    jclass intersectionServerClass = env->FindClass("bftsmart/demo/intersection/IntersectionServer");
    if (!intersectionServerClass) {
        std::cerr << "[V2VProxy] ERROR: Failed to find IntersectionServer class" << "\n";
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }

    JNINativeMethod serverMethods[] = {
        {const_cast<char*>("notifyVehicleCanGo"),    const_cast<char*>("(ID)V"),                (void*)&Java_bftsmart_demo_intersection_IntersectionServer_notifyVehicleCanGo},
        {const_cast<char*>("notifyViewAgreed"),      const_cast<char*>("(ILjava/lang/String;)V"),(void*)&Java_bftsmart_demo_intersection_IntersectionServer_notifyViewAgreed},
        {const_cast<char*>("notifyOrderDecided"),    const_cast<char*>("(ILjava/lang/String;)V"),(void*)&Java_bftsmart_demo_intersection_IntersectionServer_notifyOrderDecided},
        {const_cast<char*>("notifyProposeAllConsensusMetric"), const_cast<char*>("(IID)V"),     (void*)&Java_bftsmart_demo_intersection_IntersectionServer_notifyProposeAllConsensusMetric},
        {const_cast<char*>("notifyWipeComplete"),    const_cast<char*>("(I)V"),                 (void*)&Java_bftsmart_demo_intersection_IntersectionServer_notifyWipeComplete},
        {const_cast<char*>("nativeGetCertSnapshot"), const_cast<char*>("(I)Ljava/util/Set;"),  (void*)&Java_bftsmart_demo_intersection_IntersectionServer_nativeGetCertSnapshot},
        {const_cast<char*>("nativeGetFreshProposePayload"), const_cast<char*>("(I)Ljava/lang/String;"), (void*)&Java_bftsmart_demo_intersection_IntersectionServer_nativeGetFreshProposePayload},
        {const_cast<char*>("nativeBroadcastClientRequest"), const_cast<char*>("(I[B)V"),        (void*)&Java_bftsmart_demo_intersection_IntersectionServer_nativeBroadcastClientRequest}
    };

    if (env->RegisterNatives(intersectionServerClass, serverMethods, 8) != 0) {
        std::cerr << "[V2VProxy] ERROR: Failed to register IntersectionServer native methods" << "\n";
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }

    std::cout << "[V2VProxy] Successfully registered 8 IntersectionServer JNI native methods" << "\n";

    // Cache IntersectionServer.deliverInjectedClientRequest(int, byte[]) for type-9 dispatch.
    intersectionServerGlobalClass = static_cast<jclass>(env->NewGlobalRef(intersectionServerClass));
    deliverInjectedClientRequestMethod = env->GetStaticMethodID(
        intersectionServerGlobalClass, "deliverInjectedClientRequest", "(I[B)V");
    if (!deliverInjectedClientRequestMethod) {
        std::cerr << "[V2VProxy] WARNING: deliverInjectedClientRequest not found — type-9 dispatch will be no-op\n";
        env->ExceptionClear();
    }

    return true;


}

void V2VProxyModule::startBFTSmartReplica()
{
    if (!jvm) {
        EV_ERROR << "Cannot start BFTSmart replica: JVM not initialized" << "\n";
        return;
    }

    JNIEnvGuard jniGuard(jvm);
    if (!jniGuard.valid()) {
        std::cerr << "[V2VProxyModule] ERROR: Could not acquire JNIEnv to start replica" << "\n";
        return;
    }
    JNIEnv* env = jniGuard.env;

    std::cout << "[V2VProxyModule] Starting BFTSmart replica " << replicaId << " in background Java thread" << "\n";

    jclass runnerClass = env->FindClass("bftsmart/demo/intersection/ServerRunner");
    if (!runnerClass) {
        std::cerr << "[V2VProxyModule] ERROR: Failed to find ServerRunner class" << "\n";
        env->ExceptionDescribe();
        return;
    }

    jmethodID runnerCtor = env->GetMethodID(runnerClass, "<init>", "(II)V");
    if (!runnerCtor) {
        std::cerr << "[V2VProxyModule] ERROR: Failed to find ServerRunner constructor" << "\n";
        env->ExceptionDescribe();
        return;
    }

    jobject runnerInstance = env->NewObject(runnerClass, runnerCtor, replicaId, BATCH_SIZE);
    if (!runnerInstance) {
        std::cerr << "[V2VProxyModule] ERROR: Failed to create ServerRunner instance" << "\n";
        env->ExceptionDescribe();
        return;
    }

    jclass threadClass = env->FindClass("java/lang/Thread");
    if (!threadClass) {
        std::cerr << "[V2VProxyModule] ERROR: Failed to find Thread class" << "\n";
        env->ExceptionDescribe();
        return;
    }

    jmethodID threadCtor = env->GetMethodID(threadClass, "<init>", "(Ljava/lang/Runnable;)V");
    if (!threadCtor) {
        std::cerr << "[V2VProxyModule] ERROR: Failed to find Thread constructor" << "\n";
        env->ExceptionDescribe();
        return;
    }

    jobject thread = env->NewObject(threadClass, threadCtor, runnerInstance);
    if (!thread) {
        std::cerr << "[V2VProxyModule] ERROR: Failed to create Thread" << "\n";
        env->ExceptionDescribe();
        return;
    }

    jmethodID startMethod = env->GetMethodID(threadClass, "start", "()V");
    if (!startMethod) {
        std::cerr << "[V2VProxyModule] ERROR: Failed to find Thread.start method" << "\n";
        env->ExceptionDescribe();
        return;
    }

    std::cout << "[V2VProxyModule] Starting Java thread for replica " << replicaId << "\n";
    env->CallVoidMethod(thread, startMethod);
    bftReplicaThread = env->NewGlobalRef(thread);
    std::cout << "[V2VProxyModule] BFTSmart replica " << replicaId << " thread started (non-blocking)" << "\n";

    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

void V2VProxyModule::stopBFTSmartReplica()
{
    if (bftReplicaThread && jvm) {
        JNIEnvGuard jniGuard(jvm);
        if (jniGuard.valid()) {
            jniGuard.env->DeleteGlobalRef(bftReplicaThread);
        }
        bftReplicaThread = nullptr;
        EV_INFO << "BFTSmart replica " << replicaId << " stopped" << "\n";
    }
}

// ============================================================================
// INTERSECTION MANAGEMENT
// ============================================================================
