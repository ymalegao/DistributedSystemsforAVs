package bftsmart.demo.intersection;

import bftsmart.reconfiguration.VMServices;

/**
 * Trusted Third Party (TTP) that triggers reconfiguration.
 * This removes replica 0 and adds replica 4 to demonstrate dynamic membership.
 */
public class ReconfigurationTrigger {
    
    public static void main(String[] args) {
        if (args.length < 1) {
            System.out.println("Usage: ReconfigurationTrigger <action>");
            System.out.println("  Actions: remove0, add4, replace");
            System.exit(-1);
        }
        
        String action = args[0];
        
        try {
            VMServices vmServices = new VMServices();
            
            switch (action) {
                case "remove0":
                    System.out.println("[TTP] Removing replica 0 from the view...");
                    vmServices.removeServer(0);
                    System.out.println("[TTP] Replica 0 removed successfully!");
                    break;
                    
                case "add4":
                    System.out.println("[TTP] Adding replica 4 to the view...");
                    // From hosts.config: 4 127.0.0.1 11040 11041
                    vmServices.addServer(4, "127.0.0.1", 11040, 11041);
                    System.out.println("[TTP] Replica 4 added successfully!");
                    break;
                    
                case "replace":
                    System.out.println("[TTP] Replacing replica 0 with replica 4...");
                    System.out.println("[TTP] Step 1: Removing replica 0...");
                    vmServices.removeServer(0);
                    System.out.println("[TTP] Replica 0 removed. Waiting 3 seconds...");
                    Thread.sleep(3000);
                    
                    System.out.println("[TTP] Step 2: Adding replica 4...");
                    vmServices.addServer(4, "127.0.0.1", 11040, 11041);
                    System.out.println("[TTP] Replacement complete! View now has replicas 1,2,3,4");
                    break;
                    
                default:
                    System.err.println("[TTP] Unknown action: " + action);
                    System.exit(-1);
            }
            
        } catch (Exception e) {
            System.err.println("[TTP] Error during reconfiguration: " + e.getMessage());
            e.printStackTrace();
            System.exit(-1);
        }
        
        // Force exit after a short delay to ensure message delivery
        try {
            Thread.sleep(1000);
        } catch (InterruptedException e) {
            // ignore
        }
        System.exit(0);
    }
}

