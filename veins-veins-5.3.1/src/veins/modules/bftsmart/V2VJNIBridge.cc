//
// V2VJNIBridge.cc
// JNI implementation that bridges Java BFT-SMaRt to OMNeT++ V2V
//

#include "veins/modules/bftsmart/bftsmart_communication_V2V_V2VNativeBridge.h"
#include "veins/modules/bftsmart/V2VProxyModule.h"
#include <iostream>
#include <cstring>

using namespace veins;

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

/*
 * Class:     bftsmart_demo_intersection_IntersectionServer
 * Method:    notifyVehicleCanGo
 * Signature: (ID)V
 *
 * Called by Java when consensus completes and the vehicle can resume movement
 */
JNIEXPORT void JNICALL Java_bftsmart_demo_intersection_IntersectionServer_notifyVehicleCanGo
  (JNIEnv* env, jobject javaObj, jint replicaId, jdouble delaySeconds)
{
    std::cout << "[JNI] notifyVehicleCanGo: replica " << replicaId
              << " can go after " << delaySeconds << "s delay" << std::endl;

    // Find the proxy module for this replica
    V2VProxyModule* proxy = V2VProxyModule::getProxyForReplica(replicaId);

    if (!proxy) {
        std::cerr << "[JNI] ERROR: No V2VProxyModule found for replica " << replicaId << std::endl;
        return;
    }

    // Tell the proxy to resume the vehicle after the assigned delay
    proxy->resumeVehicle(delaySeconds);
}


extern "C" {
JNIEXPORT void JNICALL Java_bftsmart_communication_V2V_V2VNativeBridge_nativeWarmupPing(JNIEnv* env, jclass cls)
{
    std::cout << "[JNI] nativeWarmupPing called" << std::endl;

    // Do nothing, just return. This ensures the JNI link overhead is paid.
    return;
}
}

// JNI Library initialization (optional but useful for debugging)
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved)
{
    std::cout << "[JNI] V2V JNI library loaded successfully" << std::endl;
    return JNI_VERSION_1_8;
}

// JNI Library cleanup (optional)
JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved)
{
    std::cout << "[JNI] V2V JNI library unloaded" << std::endl;
}








