# BFT-SMaRt V2V Integration for Veins/OMNeT++

This module bridges BFT-SMaRt Java replicas with Veins V2V communication using JNI.

## Architecture

```
Java (BFT-SMaRt)                    C++ (OMNeT++/Veins)
┌─────────────────────┐             ┌──────────────────────┐
│ V2VNativeBridge     │◄───JNI────►│ V2VJNIBridge.cc      │
│ (Java class)        │             │ (JNI implementation) │
└─────────────────────┘             └──────────────────────┘
         │                                     │
         │                                     ▼
         │                          ┌──────────────────────┐
         │                          │ V2VProxyModule       │
         │                          │ (OMNeT++ module)     │
         │                          └──────────────────────┘
         │                                     │
         └───deliverMessage callback───────────┘
                    (received messages)
```

## Files Created

1. **BFTMessage.msg** - OMNeT++ message definition for BFT protocol data
2. **V2VProxyModule.ned** - OMNeT++ module interface definition
3. **V2VProxyModule.h** - C++ header with JNI integration
4. **V2VProxyModule.cc** - OMNeT++ module implementation
5. **V2VJNIBridge.cc** - JNI native method implementations
6. **bftsmart_communication_V2V_V2VNativeBridge.h** - Generated JNI header
7. **package.ned** - Package definition

## Build Instructions

### 1. Build OMNeT++ Message Files

From the veins directory:

```bash
cd /home/yash/veins-veins-5.3.1
opp_msgc src/veins/modules/bftsmart/BFTMessage.msg
```

### 2. Build Veins with JNI Support

Make sure your environment has Java development headers:

```bash
# Find your Java include paths
JAVA_HOME=$(dirname $(dirname $(readlink -f $(which javac))))
echo $JAVA_HOME

# Build Veins with JNI flags
cd /home/yash/veins-veins-5.3.1
./configure
make clean
make MODE=release \
  CFLAGS="-I$JAVA_HOME/include -I$JAVA_HOME/include/linux" \
  LDFLAGS="-L$JAVA_HOME/lib/server -ljvm"
```

### 3. Build the Shared Library for Java

The v2vjni library needs to be accessible to Java:

```bash
# The library will be in:
# /home/yash/veins-veins-5.3.1/out/gcc-release/src/libveins.so

# Create a symlink or copy it where Java can find it
cd /home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library
mkdir -p native/lib
ln -s /home/yash/veins-veins-5.3.1/out/gcc-release/src/libveins.so native/lib/libv2vjni.so
```

### 4. Set Java Library Path

When running BFT-SMaRt, add the library path:

```bash
java -Djava.library.path=/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/native/lib \
     -cp ... bftsmart.demo.YourApplication
```

## Usage in OMNeT++ Simulation

### Network Configuration (omnetpp.ini)

```ini
[Config BFTSmartV2V]
network = YourV2VNetwork

# BFT Replica vehicles
*.node[0].appl.typename = "V2VProxyModule"
*.node[0].appl.replicaId = 0

*.node[1].appl.typename = "V2VProxyModule"
*.node[1].appl.replicaId = 1

*.node[2].appl.typename = "V2VProxyModule"
*.node[2].appl.replicaId = 2

*.node[3].appl.typename = "V2VProxyModule"
*.node[3].appl.replicaId = 3
```

### Network Definition (.ned)

```ned
import veins.modules.bftsmart.V2VProxyModule;

network BFTNetwork {
    parameters:
        int numReplicas = default(4);
    submodules:
        replica[numReplicas]: Car {
            parameters:
                applType = "V2VProxyModule";
        }
}
```

## Workflow

1. **Start OMNeT++ simulation** with V2VProxyModule instances
2. **Start BFT-SMaRt Java replicas** with V2VNativeBridge
3. **JNI initialization** connects Java to corresponding OMNeT++ modules
4. **Messages flow:**
   - Java calls `sendMessage()` → JNI → V2VProxyModule → V2V broadcast
   - V2V receive → V2VProxyModule → JNI callback → Java `deliverMessage()`

## Debugging

Enable verbose JNI output:

```bash
# In Java
java -verbose:jni -Djava.library.path=...

# In OMNeT++
*.*.appl.debug = true
```

Check JNI initialization:
- JNI prints `[JNI] nativeInit called for replica X`
- OMNeT++ logs "Java callback registered for replica X"

## Troubleshooting

### UnsatisfiedLinkError
- Check `java.library.path` includes the directory with `libv2vjni.so`
- Verify symlink: `ls -la native/lib/libv2vjni.so`
- Check library dependencies: `ldd libv2vjni.so`

### No V2VProxyModule found
- Ensure OMNeT++ simulation starts **before** Java replicas
- Check replicaId matches between Java and OMNeT++ config
- Verify modules are properly initialized in simulation

### Messages not delivered
- Check JNI callback registration succeeded
- Verify V2V channel configuration matches
- Enable debug logging in both Java and OMNeT++








