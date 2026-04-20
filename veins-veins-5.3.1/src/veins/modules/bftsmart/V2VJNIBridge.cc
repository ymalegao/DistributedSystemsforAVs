//
// V2VJNIBridge.cc
// JNI implementation that bridges Java BFT-SMaRt to OMNeT++ V2V
//

#include "veins/modules/bftsmart/bftsmart_communication_V2V_V2VNativeBridge.h"
#include "veins/modules/bftsmart/V2VProxyModule.h"
#include <iostream>
#include <cstring>
#include <set>
#include <string>
#include <sstream>

using namespace veins;

// Forward declaration for JNI_OnLoad (defined below notifyVehicleCanGo)
extern "C" JNIEXPORT void JNICALL Java_bftsmart_demo_intersection_IntersectionServer_notifyOrderDecided
    (JNIEnv*, jobject, jint, jstring);
extern "C" JNIEXPORT void JNICALL Java_bftsmart_demo_intersection_IntersectionServer_notifyProposeAllConsensusMetric
    (JNIEnv*, jobject, jint, jint, jdouble);
extern "C" JNIEXPORT void JNICALL Java_bftsmart_demo_intersection_IntersectionServer_notifyWipeComplete
    (JNIEnv*, jobject, jint);
extern "C" JNIEXPORT jobject JNICALL Java_bftsmart_demo_intersection_IntersectionServer_nativeGetCertSnapshot
    (JNIEnv*, jobject, jint);
extern "C" JNIEXPORT jstring JNICALL Java_bftsmart_demo_intersection_IntersectionServer_nativeGetFreshProposePayload
    (JNIEnv*, jobject, jint);

static std::vector<uint8_t> jbyteArrayToVector(JNIEnv* env, jbyteArray array) {
    int len = env->GetArrayLength(array);
    jbyte* jbytes = env->GetByteArrayElements(array, nullptr);
    
    // Copy directly from JNI buffer to Vector
    std::vector<uint8_t> vec(jbytes, jbytes + len);
    
    // Release JNI buffer
    env->ReleaseByteArrayElements(array, jbytes, JNI_ABORT);
    
    return vec;
}

/*
 * Class:     bftsmart_communication_V2V_V2VNativeBridge
 * Method:    nativeInit
 * Signature: (I)V
 * 
 * Called when Java V2VNativeBridge is constructed
 * Registers the Java callback object with the corresponding OMNeT++ proxy
 */
JNIEXPORT void JNICALL Java_bftsmart_communication_V2V_V2VNativeBridge_nativeInit
  (JNIEnv* env, jobject javaObj, jint replicaId)
{
    std::cout << "[JNI] nativeInit called for replica " << replicaId << std::endl;
    
    // Find the corresponding V2VProxyModule in OMNeT++
    V2VProxyModule* proxy = V2VProxyModule::getProxyForReplica(replicaId);
    
    if (!proxy) {
        std::cerr << "[JNI] ERROR: No V2VProxyModule found for replica " << replicaId << std::endl;
        std::cerr << "[JNI] Make sure OMNeT++ simulation is running and module is initialized!" << std::endl;
        return;
    }
    
    // Register Java callback with the proxy
    proxy->registerJavaCallback(env, javaObj);
    
    std::cout << "[JNI] Successfully initialized V2V bridge for replica " << replicaId << std::endl;
}

/*
 * Class:     bftsmart_communication_V2V_V2VNativeBridge
 * Method:    nativeSendMessage
 * Signature: (II[B)Z
 * 
 * Called when Java wants to send a message via V2V
 * Returns true if message was successfully queued
 */
JNIEXPORT jboolean JNICALL Java_bftsmart_communication_V2V_V2VNativeBridge_nativeSendMessage
  (JNIEnv* env, jobject javaObj, jint fromReplicaId, jint toReplicaId, jbyteArray data)
{
    std::cout << "[JNI] nativeSendMessage: " << fromReplicaId << " -> " << toReplicaId << std::endl;
    
    // Find the proxy module
    V2VProxyModule* proxy = V2VProxyModule::getProxyForReplica(fromReplicaId);
    
    if (!proxy) {
        std::cerr << "[JNI] ERROR: No V2VProxyModule found for replica " << fromReplicaId << std::endl;
        return JNI_FALSE;
    }

    std::vector<uint8_t> payload = jbyteArrayToVector(env, data);
    std::cout << "[JNI] Sending " << payload.size() << " bytes (" << fromReplicaId << "->" << toReplicaId << ")" << std::endl;
    
    // Send through proxy
    bool success = proxy->sendMessageToReplica(fromReplicaId, toReplicaId, payload.data(), payload.size());
    
    if (success) {
        std::cout << "[JNI] Message queued successfully" << std::endl;
    } else {
        std::cerr << "[JNI] Failed to queue message" << std::endl;
    }
    
    return success ? JNI_TRUE : JNI_FALSE;
}

/*
 * Class:     bftsmart_communication_V2V_V2VNativeBridge
 * Method:    nativeShutdown
 * Signature: (I)V
 * 
 * Called when Java V2VNativeBridge is being shut down
 */
 
JNIEXPORT void JNICALL Java_bftsmart_communication_V2V_V2VNativeBridge_nativeShutdown
  (JNIEnv* env, jobject javaObj, jint replicaId)
{
    std::cout << "[JNI] nativeShutdown called for replica " << replicaId << std::endl;
    
    // Find the proxy module
    V2VProxyModule* proxy = V2VProxyModule::getProxyForReplica(replicaId);
    
    if (!proxy) {
        std::cerr << "[JNI] WARNING: No V2VProxyModule found for replica " << replicaId << std::endl;
        return;
    }
    
    // Note: Don't delete the proxy - OMNeT++ manages its lifecycle
    // Just log that shutdown was called
    std::cout << "[JNI] V2V bridge shutdown complete for replica " << replicaId << std::endl;
}

extern "C" {

/**
 * JNI callback: notifyViewAgreed (NEW for 3-phase consensus)
 *
 * Called by Java when VIEW consensus completes (Phase 1c → Phase 2 transition)
 */
JNIEXPORT void JNICALL Java_bftsmart_demo_intersection_IntersectionServer_notifyViewAgreed
  (JNIEnv* env, jobject javaObj, jint replicaId, jstring viewMembers)
{
    const char* viewStr = env->GetStringUTFChars(viewMembers, nullptr);
    std::cout << "[JNI] notifyViewAgreed: replica " << replicaId
              << ", view=" << viewStr << std::endl;

    // Find the proxy module for this replica
    V2VProxyModule* proxy = V2VProxyModule::getProxyForReplica(replicaId);

    if (!proxy) {
        std::cerr << "[JNI] ERROR: No V2VProxyModule found for replica " << replicaId << std::endl;
        env->ReleaseStringUTFChars(viewMembers, viewStr);
        return;
    }

    // Parse view members from comma-separated string
    std::set<std::string> view;
    std::string viewString(viewStr);
    std::istringstream ss(viewString);
    std::string carId;
    while (std::getline(ss, carId, ',')) {
        if (!carId.empty()) {
            view.insert(carId);
        }
    }
    
    env->ReleaseStringUTFChars(viewMembers, viewStr);

    // Notify the proxy that view consensus is complete
    proxy->onViewAgreed(view);
}

/**
 * JNI callback: notifyVehicleCanGo
 *
 * Called by Java when consensus completes and the vehicle can resume movement
 */
JNIEXPORT void JNICALL Java_bftsmart_demo_intersection_IntersectionServer_notifyVehicleCanGo
  (JNIEnv* env, jobject javaObj, jint replicaId, jdouble delaySeconds)
{
    std::cout << "\n[JNI-NOTIFY] *** Java called notifyVehicleCanGo() ***" << std::endl;
    std::cout << "[JNI-NOTIFY]   Replica ID: " << replicaId << std::endl;
    std::cout << "[JNI-NOTIFY]   Delay: " << delaySeconds << "s" << std::endl;

    // Find the proxy module for this replica
    V2VProxyModule* proxy = V2VProxyModule::getProxyForReplica(replicaId);

    if (!proxy) {
        std::cerr << "[JNI-NOTIFY] *** ERROR: No V2VProxyModule found for replica " << replicaId << " ***" << std::endl;
        return;
    }

    std::cout << "[JNI-NOTIFY]   Found proxy, calling resumeVehicle(" << delaySeconds << ")" << std::endl;
    // Tell the proxy to resume the vehicle after the assigned delay
    proxy->resumeVehicle(delaySeconds);
    std::cout << "[JNI-NOTIFY]   resumeVehicle() returned" << std::endl;
}

}  // extern "C"


extern "C" {
JNIEXPORT void JNICALL Java_bftsmart_communication_V2V_V2VNativeBridge_nativeWarmupPing(JNIEnv* env, jclass cls)
{
    std::cout << "[JNI] nativeWarmupPing called" << std::endl;

    // Do nothing, just return. This ensures the JNI link overhead is paid.
    return;
}
}

// JNI Library initialization - register native methods when library loads
// This ensures notifyOrderDecided etc. are registered even if createOrAttachJVM timing differs
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved)
{
    std::cout << "[JNI] V2V JNI library loaded successfully" << std::endl;

    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_8) != JNI_OK) {
        std::cerr << "[JNI] JNI_OnLoad: GetEnv failed" << std::endl;
        return JNI_ERR;
    }

    // Register IntersectionServer native methods here as fallback (runs when Java loads library)
    jclass intersectionServerClass = env->FindClass("bftsmart/demo/intersection/IntersectionServer");
    if (intersectionServerClass) {
        JNINativeMethod serverMethods[] = {
            {const_cast<char*>("notifyVehicleCanGo"),    const_cast<char*>("(ID)V"),                (void*)&Java_bftsmart_demo_intersection_IntersectionServer_notifyVehicleCanGo},
            {const_cast<char*>("notifyViewAgreed"),      const_cast<char*>("(ILjava/lang/String;)V"),(void*)&Java_bftsmart_demo_intersection_IntersectionServer_notifyViewAgreed},
            {const_cast<char*>("notifyOrderDecided"),    const_cast<char*>("(ILjava/lang/String;)V"),(void*)&Java_bftsmart_demo_intersection_IntersectionServer_notifyOrderDecided},
            {const_cast<char*>("notifyProposeAllConsensusMetric"), const_cast<char*>("(IID)V"),     (void*)&Java_bftsmart_demo_intersection_IntersectionServer_notifyProposeAllConsensusMetric},
            {const_cast<char*>("notifyWipeComplete"),    const_cast<char*>("(I)V"),                 (void*)&Java_bftsmart_demo_intersection_IntersectionServer_notifyWipeComplete},
            {const_cast<char*>("nativeGetCertSnapshot"), const_cast<char*>("(I)Ljava/util/Set;"),  (void*)&Java_bftsmart_demo_intersection_IntersectionServer_nativeGetCertSnapshot}
        };
        if (env->RegisterNatives(intersectionServerClass, serverMethods, 6) == 0) {
            std::cout << "[JNI] JNI_OnLoad: Registered 6 IntersectionServer native methods" << std::endl;
        }
    }

    return JNI_VERSION_1_8;
}

// JNI Library cleanup (optional)
JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved)
{
    std::cout << "[JNI] V2V JNI library unloaded" << std::endl;
}

JNIEXPORT void JNICALL Java_bftsmart_demo_intersection_IntersectionServer_notifyOrderDecided
    (JNIEnv* env, jobject obj, jint replicaId, jstring orderDecision)
  {
      const char* decisionStr = env->GetStringUTFChars(orderDecision, nullptr);
      std::cout << "[JNI-CALLBACK] Replica " << replicaId
                << " ORDER decision: " << decisionStr << std::endl;

      // Find the V2VProxyModule for this replica
      V2VProxyModule* proxy = V2VProxyModule::getProxyForReplica(replicaId);
      if (proxy) {
          proxy->handleOrderDecision(std::string(decisionStr));
      }

      env->ReleaseStringUTFChars(orderDecision, decisionStr);
  }

JNIEXPORT void JNICALL Java_bftsmart_demo_intersection_IntersectionServer_notifyProposeAllConsensusMetric
    (JNIEnv* env, jobject obj, jint replicaId, jint epoch, jdouble wallSeconds)
{
    std::cout << "[JNI-CALLBACK] Replica " << replicaId
              << " PROPOSE_ALL consensus epoch=" << epoch
              << " wall=" << wallSeconds << "s" << std::endl;

    V2VProxyModule* proxy = V2VProxyModule::getProxyForReplica(replicaId);
    if (proxy) {
        proxy->recordProposeAllConsensusMetric(epoch, wallSeconds);
    }
}



JNIEXPORT void JNICALL Java_bftsmart_demo_intersection_IntersectionServer_notifyWipeComplete
    (JNIEnv* env, jobject obj, jint replicaId)
  {
      std::cout << "[JNI-CALLBACK] Replica " << replicaId
                << " WIPE COMPLETE — epoch preemption done" << std::endl;
      V2VProxyModule* proxy = V2VProxyModule::getProxyForReplica(replicaId);
      if (proxy) {
          proxy->handleWipeComplete();
      }
  }

/*
 * Class:     bftsmart_demo_intersection_IntersectionServer
 * Method:    nativeGetCertSnapshot
 * Signature: (I)Ljava/util/Set;
 *
 * Returns a Java HashSet<String> of carIds currently in this replica's
 * C++ collectedCerts map.  Called from Java's getCertSnapshot() so that
 * OrderRequestVerifier can check for cert-omission before voting WRITE.
 */
JNIEXPORT jobject JNICALL Java_bftsmart_demo_intersection_IntersectionServer_nativeGetCertSnapshot
    (JNIEnv* env, jobject /*obj*/, jint replicaId)
{
    jclass hsClass = env->FindClass("java/util/HashSet");
    jmethodID ctor = env->GetMethodID(hsClass, "<init>", "()V");
    jmethodID addM = env->GetMethodID(hsClass, "add", "(Ljava/lang/Object;)Z");
    jobject set    = env->NewObject(hsClass, ctor);

    V2VProxyModule* proxy = V2VProxyModule::getProxyForReplica(replicaId);
    if (proxy) {
        for (const std::string& id : proxy->getCertSnapshotKeys()) {
            jstring js = env->NewStringUTF(id.c_str());
            env->CallBooleanMethod(set, addM, js);
            env->DeleteLocalRef(js);
        }
    }
    return set;
}

/*
 * Class:     bftsmart_demo_intersection_IntersectionServer
 * Method:    nativeGetFreshProposePayload
 * Signature: (I)Ljava/lang/String;
 *
 * Returns the "<vehicleStatesStr>:<perCarCerts>" string built from this
 * replica's CURRENT C++ collectedCerts map. Called from Java's
 * getFreshProposePayload() during Synchronizer.catch_up() so the new
 * leader can rebuild a fresh PROPOSE_ALL (instead of replaying the
 * censored bytes the deposed Byzantine leader queued).
 *
 * Returns an empty string when no proxy is registered for this replicaId
 * (e.g. before C++/OMNeT++ startup or in unit-test contexts); Java treats
 * "" as "JNI unavailable, fall back to existing replay behavior".
 */
JNIEXPORT jstring JNICALL Java_bftsmart_demo_intersection_IntersectionServer_nativeGetFreshProposePayload
    (JNIEnv* env, jobject /*obj*/, jint replicaId)
{
    V2VProxyModule* proxy = V2VProxyModule::getProxyForReplica(replicaId);
    if (!proxy) return env->NewStringUTF("");
    return env->NewStringUTF(proxy->buildFreshProposePayload().c_str());
}
