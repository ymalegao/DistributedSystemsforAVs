package bftsmart.communication.V2V;
import java.io.*;
import java.net.Socket;



  /**
   * Represents a connection to another replica via V2V (through Veins bridge).
   * Each replica has a Veins application listening on a specific port.
   */

public class V2VReplicaConnection {
    private final int replicaId;
    private final String host;
    private final int port;

    public V2VReplicaConnection(int replicaId, String host, int port){
        this.replicaId = replicaId;
        this.host = host;
        this.port = port;
    }

    public synchronized void send(V2VMessageEnvelope envelope){
        System.out.println("    [V2VReplicaConnection] Attempting to send to replica " + replicaId);
        System.out.println("        Host: " + host + ", Port: " + port);
        System.out.println("        From: " + envelope.fromReplicaId + ", SeqNum: " + envelope.sequenceNum);
        
        try (Socket socket = new Socket(host, port);
            ObjectOutputStream out = new ObjectOutputStream(socket.getOutputStream())){
                System.out.println("        Socket connected, sending envelope...");
                out.writeObject(envelope);
                out.flush();
                System.out.println("        ✓ Message sent successfully to replica " + replicaId);
        }catch (IOException e){
            System.err.println("        ✗ ERROR sending to replica " + replicaId);
            System.err.println("        Error: " + e.getMessage());
            e.printStackTrace();
        }

            
    }

    public void close(){
        //cleanup if needed
    }
}
