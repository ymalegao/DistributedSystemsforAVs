package bftsmart.communication.server;

import bftsmart.communication.SystemMessage;
import javax.crypto.SecretKey;

/**
 * Interface for server communication layers to enable different transport mechanisms
 * (e.g., TCP/SSL or V2V communication)
 */
public interface ServersCommunicationLayerInterface {
    
    void send(int[] targets, SystemMessage sm, boolean serializeClassHeaders);
    
    void shutdown();
    
    SecretKey getSecretKey(int id);
    
    void joinViewReceived();
    
    void updateConnections();

    void join() throws InterruptedException;

    void start();
}

