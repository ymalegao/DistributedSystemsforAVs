package bftsmart.communication.V2V;

public class V2VNativeBridge {
    private static boolean libaryLoaded = false;
    private final int replicaId;
    private MessageReceiverCallback callback;
    private RadioReadyCallback radioReadyCallback;
    private boolean initialized = false;


    static {
        // Check if we're running in embedded mode (JVM created by OMNeT++)
        String embeddedMode = System.getProperty("bftsmart.jni.embedded", "false");

        if ("true".equalsIgnoreCase(embeddedMode)) {
            System.out.println("V2VNativeBridge: Running in EMBEDDED mode - library already loaded by OMNeT++");
            libaryLoaded = true;
        } else {
            // Standalone mode - load library normally
            try{
                System.out.println("V2VNativeBridge: [1/3] Starting to load v2vjni library...");
                System.loadLibrary("v2vjni");
                System.out.println("V2VNativeBridge: [2/3] Library loaded, checking status...");
                libaryLoaded = true;
                System.out.println("V2VNativeBridge: [3/3] Library loaded successfully");
            } catch (UnsatisfiedLinkError e) {
                System.out.println("V2VNativeBridge: Library not found");
                e.printStackTrace();
                libaryLoaded = false;
            } catch (Throwable t) {
                System.out.println("V2VNativeBridge: UNEXPECTED ERROR loading library: " + t.getClass().getName());
                t.printStackTrace();
                libaryLoaded = false;
            }
        }
    }


    public interface MessageReceiverCallback {
        void onMessageReceived(int fromReplicaId, byte[] messageData);
    }
    
    // Callback interface for reactive yield pattern
    public interface RadioReadyCallback {
        void onRadioReady();
    }

    public V2VNativeBridge(int replicaId, MessageReceiverCallback callback) {
        this.replicaId = replicaId;
        this.callback = callback;

        if (libaryLoaded) {
            System.out.println("V2VNativeBridge" + replicaId + ": Created, initializing immediately...");
            // CRITICAL FIX: Initialize immediately to register C++ → Java callback
            // If we wait until first send, incoming messages will be dropped!
            try {
                nativeInit(replicaId);
                initialized = true;
                System.out.println("V2VNativeBridge" + replicaId + ": Initialized successfully, callback registered");
            } catch (Exception e) {
                System.err.println("V2VNativeBridge" + replicaId + ": FAILED to initialize: " + e.getMessage());
                e.printStackTrace();
            }
        } else {
            System.out.println("V2VNativeBridge" + replicaId + ": Library not loaded");
        }
    }
    
    private void ensureInitialized() {
        if (!initialized && libaryLoaded) {
            nativeInit(replicaId);
            initialized = true;
            System.out.println("V2VNativeBridge" + replicaId + ": Initialized on first use");
        }
    }
    /**
     * Send a message to a target replica via V2V
     * This will be called by V2VReplicaConnection
     *
     * @param targetReplicaId Target replica ID
     * @param messageData Serialized message data
     * @return true if message was queued successfully
     */
    public boolean sendMessage(int targetReplicaId, byte[] messageData) {
        if (!libaryLoaded) {
            System.out.println("V2VNativeBridge" + replicaId + ": Library not loaded");
            return false;
        }
        
        // Lazy initialization on first send (called from OMNeT++ thread context)
        ensureInitialized();
        
        System.out.println("V2VNativeBridge" + replicaId + ": Sending message to " + targetReplicaId  + " with length " + messageData.length);
        System.out.flush();
        System.out.println("V2VNativeBridge" + replicaId + ": Calling nativeSendMessage...");
        System.out.flush();
        boolean result = nativeSendMessage(replicaId, targetReplicaId, messageData);
        System.out.println("V2VNativeBridge" + replicaId + ": nativeSendMessage returned: " + result);
        System.out.flush();
        return result;
    }

    //this is called by JNI when mesasge is recived from the simulation - c++ code
    @SuppressWarnings("unused")
    private void deliverMessage(int fromReplicaId, byte[] messageData){
        System.out.println("V2VNativeBridge" + replicaId + ": Received message from " + fromReplicaId + " with length " + messageData.length);
        if (callback != null) {
            callback.onMessageReceived(fromReplicaId, messageData);
        }else{
            System.out.println("V2VNativeBridge" + replicaId + ": No callback registered");
        }

    }

    public void shutdown() {
        if (libaryLoaded) {
            nativeShutdown(replicaId);
            System.out.println("V2VNativeBridge" + replicaId + ": Shutdown successfully");
        } else {
            System.out.println("V2VNativeBridge" + replicaId + ": Library not loaded");
        }
    }
    
    // Reactive yield pattern: check if radio is busy
    public boolean isRadioBusy() {
        if (!libaryLoaded || !initialized) return false;
        return nativeIsRadioBusy(replicaId);
    }
    
    // Set callback for when radio becomes ready
    public void setRadioReadyCallback(RadioReadyCallback callback) {
        this.radioReadyCallback = callback;
    }
    
    // Called by C++ via JNI when radio transmission completes
    @SuppressWarnings("unused")
    private void onRadioReady() {
        if (radioReadyCallback != null) {
            radioReadyCallback.onRadioReady();
        }
    }

    public static void warmupJVM() {
        System.out.println("[Java] Starting JVM Warmup (Crypto & JNI)...");
        try {
            // 1. Warm up Crypto (The expensive part!)
            byte[] dummyData = new byte[1024];
            new java.util.Random().nextBytes(dummyData);
  
            java.security.Signature sign = java.security.Signature.getInstance("SHA256withRSA");
            java.security.KeyPairGenerator kpg = java.security.KeyPairGenerator.getInstance("RSA");
            kpg.initialize(2048);
            java.security.KeyPair kp = kpg.generateKeyPair();
  
            // Loop to trigger JIT compilation (HotSpot usually kicks in after ~10k calls)
            for (int i = 0; i < 500; i++) {
                sign.initSign(kp.getPrivate());
                sign.update(dummyData);
                byte[] signature = sign.sign();
  
                sign.initVerify(kp.getPublic());
                sign.update(dummyData);
                sign.verify(signature);
            }

            // for (int i = 0; i < 2000; i++) {
            //     java.io.ByteArrayOutputStream bos = new java.io.ByteArrayOutputStream();
            //     java.io.ObjectOutputStream oos = new java.io.ObjectOutputStream(bos);
            //     oos.writeObject(new String("DUMMY_PROPOSAL_DATA_FOR_WARMUP"));
            //     oos.flush();
            //     byte[] bytes = bos.toByteArray();
                
            //     java.io.ByteArrayInputStream bis = new java.io.ByteArrayInputStream(bytes);
            //     java.io.ObjectInputStream ois = new java.io.ObjectInputStream(bis);
            //     ois.readObject();
            // }

            System.out.println("[Warmup] Serialization Warmup Done");
  
            // 2. Warm up JNI
            if (libaryLoaded) {
                nativeWarmupPing();
            }
  
            System.out.println("[Java] JVM Warmup complete!");
        } catch (Exception e) {
            System.err.println("[Java] JVM Warmup failed: " + e.getMessage());
            e.printStackTrace();
        }
    }

    //these are the native methods that are implemented in the c++ code
    private native void nativeInit(int replicaId);
    private native boolean nativeSendMessage(int fromReplicaId, int toReplicaId, byte[] data);
    private native void nativeShutdown(int replicaId);
    private native boolean nativeIsRadioBusy(int replicaId);  // Reactive yield pattern
    public static boolean isLibraryLoaded() {
        return libaryLoaded;
    }
    private static native void nativeWarmupPing();



    
}
