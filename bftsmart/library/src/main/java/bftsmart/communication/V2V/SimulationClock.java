package bftsmart.communication.V2V;

public class SimulationClock {
    private static volatile long currentSimTimeMillis = 0;

    // Called by JNI from V2VProxyModule.cc
    public static void updateTime(double simTimeSeconds) {
        currentSimTimeMillis = (long) (simTimeSeconds * 1000.0);
    }

    public static long currentTimeMillis() {
        return currentSimTimeMillis;
    }
}