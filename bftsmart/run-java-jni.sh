#!/bin/bash
# Wrapper to run Java with JNI in Nix environment
cd "$(dirname "$0")/library/src/main/java"
nix-shell -p jdk11 --run "java -Djava.library.path=/home/yash/omnetpp/omnetpp-6.2.0/bftsmart/library/native/lib $*"
