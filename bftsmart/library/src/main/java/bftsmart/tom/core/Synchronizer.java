/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */
package bftsmart.tom.core;

import bftsmart.communication.ServerCommunicationSystem;
import bftsmart.consensus.Decision;
import bftsmart.consensus.Epoch;
import bftsmart.consensus.Consensus;
import bftsmart.consensus.TimestampValuePair;
import bftsmart.consensus.messages.ConsensusMessage;
import bftsmart.consensus.messages.MessageFactory;
import bftsmart.consensus.roles.Acceptor;
import bftsmart.reconfiguration.ServerViewController;
import bftsmart.statemanagement.StateManager;
import bftsmart.tom.core.messages.TOMMessage;
import bftsmart.tom.leaderchange.RequestsTimer;
import bftsmart.tom.leaderchange.CollectData;
import bftsmart.tom.leaderchange.LCManager;
import bftsmart.tom.leaderchange.LCMessage;
import bftsmart.tom.leaderchange.CertifiedDecision;
import bftsmart.tom.util.BatchBuilder;
import bftsmart.tom.util.BatchReader;
import bftsmart.tom.util.TOMUtil;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.ObjectInputStream;
import java.io.ObjectOutputStream;
import java.security.MessageDigest;
import java.security.SignedObject;
import java.util.Arrays;
import java.util.HashSet;
import java.util.LinkedList;
import java.util.List;
import java.util.Set;
import java.util.Timer;
import java.util.TimerTask;
import bftsmart.communication.V2V.SimulationClock;

import org.apache.commons.codec.binary.Base64;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

/**
 *
 * This class implements the synchronization phase described in
 * Joao Sousa's 'From Byzantine Consensus to BFT state machine replication: a latency-optimal transformation' (May 2012)
 * 
 * This class implements all optimizations described at the end of the paper
 * 
 * @author joao
 */
public class Synchronizer {
    
    private Logger logger = LoggerFactory.getLogger(this.getClass());

    // out of context messages related to the leader change are stored here
    private final HashSet<LCMessage> outOfContextLC;
    // Regency values for which the new leader has already emitted SYNC.
    // Duplicate STOPDATAs can keep re-entering catch_up() after quorum is
    // reached; without this guard the leader keeps rebroadcasting the same
    // SYNC and adds avoidable load to the shared 802.11p channel.
    private final HashSet<Integer> syncSentRegencies;

    // Manager of the leader change
    private final LCManager lcManager;
    
    //Total order layer
    private final TOMLayer tom;
    
    // Stuff from TOMLayer that this object needs
    private final RequestsTimer requestsTimer;
    private final ExecutionManager execManager;
    private final ServerViewController controller;
    private final BatchBuilder bb;
    private final ServerCommunicationSystem communication;
    private final StateManager stateManager;
    private final Acceptor acceptor;
    private final MessageDigest md;
            
    // Attributes to temporarely store synchronization info
    // if state transfer is required for synchronization
    private int tempRegency = -1;
    private CertifiedDecision tempLastHighestCID = null;
    private HashSet<SignedObject> tempSignedCollects = null;
    private byte[] tempPropose = null;
    private int tempBatchSize = -1;
    private boolean tempIAmLeader = false;

    
    public Synchronizer(TOMLayer tom) {
        
        this.tom = tom;
        
        this.requestsTimer = this.tom.requestsTimer;
        this.execManager = this.tom.execManager;
        this.controller = this.tom.controller;
        this.bb = this.tom.bb;
        this.communication = this.tom.getCommunication();
        this.stateManager = this.tom.stateManager;
        this.acceptor = this.tom.acceptor;
        this.md = this.tom.md;
        
        this.outOfContextLC = new HashSet<>();
        this.syncSentRegencies = new HashSet<>();
	this.lcManager = new LCManager(this.tom,this.controller, this.md);
    }

    public LCManager getLCManager() {
        return lcManager;
    }
    
    /**
     * This method is called when there is a timeout and the request has already
     * been forwarded to the leader
     *
     * @param requestList List of requests that the replica wanted to order but
     * didn't manage to
     */
    public void triggerTimeout(List<TOMMessage> requestList) {

        ObjectOutputStream out = null;
        ByteArrayOutputStream bos = new ByteArrayOutputStream();

        int regency = lcManager.getNextReg();
        
        requestsTimer.stopTimer();
        requestsTimer.Enabled(false);

	// still not in the leader change phase?
        if (lcManager.getNextReg() == lcManager.getLastReg()) {

            // Single choke point for LC escalation. Both RequestsTimer.run_lc_protocol
            // (honest wall-clock retry after a client-request timeout) and
            // Acceptor.acceptReceived's null-propose branch funnel here. The
            // Acceptor path in particular bypasses any debounce in RequestsTimer —
            // and re-opens the moment startSynchronization() flips
            // requestsTimer.Enabled(true) at Phase 2 install (line ~544), which
            // happens BEFORE SYNC for reg=r propagates. Without this guard,
            // replicas that crossed Phase 2 locally escalated to reg=r+1 within
            // a sim-second of installing reg=r, splitting the cluster's STOP
            // senders across two regencies and permanently starving the new
            // leader of the 2f+1 STOPs it still needs for reg=r. See
            // LC_INVESTIGATION.md, "Stage 2, round 3" for the full evidence.
            if (!requestsTimer.tryClaimLCEpoch()) {
                logger.info("triggerTimeout: escalation to regency "
                        + (lcManager.getLastReg() + 1)
                        + " suppressed (LC epoch for regency " + regency
                        + " still in flight)");
                processOutOfContextSTOPs(regency);
                startSynchronization(regency);
                return;
            }

            lcManager.setNextReg(lcManager.getLastReg() + 1); // define next timestamp

            regency = lcManager.getNextReg(); // update variable 

            // store messages to be ordered
            lcManager.setCurrentRequestTimedOut(requestList);

            // store information about messages that I'm going to send
            lcManager.addStop(regency, this.controller.getStaticConf().getProcessId());

            //execManager.stop(); // stop consensus execution

            //Get requests that timed out and the requests received in STOP messages
            //and add those STOPed requests to the client manager
            addSTOPedRequestsToClientManager();
            List<TOMMessage> messages = getRequestsToRelay();

            try { // serialize content to send in STOP message
                out = new ObjectOutputStream(bos);

                if (messages != null && messages.size() > 0) {

					//TODO: If this is null, then there was no timeout nor STOP messages.
                    //What to do?
                    byte[] serialized = bb.makeBatch(messages, 0, 0, controller.getStaticConf().getUseSignatures() == 1);
                    out.writeBoolean(true);
                    out.writeObject(serialized);
                } else {
                    out.writeBoolean(false);
                    logger.warn("Strange... did not include any request in my STOP message for regency " + regency);
                }

                out.flush();
                bos.flush();

                byte[] payload = bos.toByteArray();

                out.close();
                bos.close();

                // send STOP-message
                logger.info("Sending STOP message to install regency " + regency + " with " + (messages != null ? messages.size() : 0) + " request(s) to relay");
                System.out.println("[VIEW-CHANGE] Replica " + this.controller.getStaticConf().getProcessId()
                        + " TIMEOUT — sending STOP for regency " + regency
                        + " (new leader will be replica " + (regency % this.controller.getCurrentViewN()) + ")");

                LCMessage stop = new LCMessage(this.controller.getStaticConf().getProcessId(), TOMUtil.STOP, regency, payload);
                requestsTimer.setSTOP(regency, stop); // make replica re-transmit the stop message until a new regency is installed
                communication.send(this.controller.getCurrentViewOtherAcceptors(), stop);

            } catch (IOException ex) {
                logger.error("Could not serialize STOP message", ex);
            } finally {
                try {
                    out.close();
                    bos.close();
                } catch (IOException ex) {
                    logger.error("Could not serialize STOP message", ex);
                }
            }

        }

        processOutOfContextSTOPs(regency); // the replica might have received STOPs
                                           // that were out of context at the time they
                                           // were received, but now can be processed
        
        startSynchronization(regency); // evaluate STOP messages
                
    }

    // Processes STOP messages that were not process upon reception, because they were
    // ahead of the replica's expected regency
    private void processOutOfContextSTOPs(int regency) {

        logger.debug("Checking if there are out of context STOPs for regency " + regency);

        Set<LCMessage> stops = getOutOfContextLC(TOMUtil.STOP, regency);

        if (stops.size() > 0) {
            logger.info("Processing " + stops.size() + " out of context STOPs for regency " + regency);
        } else {
            logger.debug("No out of context STOPs for regency " + regency);
        }

        for (LCMessage m : stops) {
            TOMMessage[] requests = deserializeTOMMessages(m.getPayload());

            // store requests that came with the STOP message
            lcManager.addRequestsFromSTOP(requests);

            // store information about the STOP message
            lcManager.addStop(regency, m.getSender());
        }
    }

    // Processes STOPDATA messages that were not process upon reception, because they were
    // ahead of the replica's expected regency
    private void processSTOPDATA(LCMessage msg, int regency) {

        //TODO: It is necessary to verify the proof of the last decided consensus and the signature of the state of the current consensus!
        CertifiedDecision lastData = null;
        SignedObject signedCollect = null;

        int last = -1;
        byte[] lastValue = null;
        Set<ConsensusMessage> proof = null;

        ByteArrayInputStream bis;
        ObjectInputStream ois;

        try { // deserialize the content of the message

            bis = new ByteArrayInputStream(msg.getPayload());
            ois = new ObjectInputStream(bis);

            if (ois.readBoolean()) { // content of the last decided cid

                last = ois.readInt();

                lastValue = (byte[]) ois.readObject();
                proof = (Set<ConsensusMessage>) ois.readObject();

                //TODO: Proof is missing!
            }

            lastData = new CertifiedDecision(msg.getSender(), last, lastValue, proof);

            lcManager.addLastCID(regency, lastData);

            signedCollect = (SignedObject) ois.readObject();

            ois.close();
            bis.close();

            lcManager.addCollect(regency, signedCollect);

            int bizantineQuorum = (controller.getCurrentViewN() + controller.getCurrentViewF()) / 2;
            int cftQuorum = (controller.getCurrentViewN()) / 2;

            // Did I already got messages from a Byzantine/Crash quorum,
            // related to the last cid as well as for the current?
            boolean conditionBFT = (controller.getStaticConf().isBFT() && lcManager.getLastCIDsSize(regency) > bizantineQuorum
                    && lcManager.getCollectsSize(regency) > bizantineQuorum);

            boolean conditionCFT = (lcManager.getLastCIDsSize(regency) > cftQuorum && lcManager.getCollectsSize(regency) > cftQuorum);

            System.out.println("[SYNC-DEBUG] processSTOPDATA regency=" + regency
                    + " lastCIDsSize=" + lcManager.getLastCIDsSize(regency)
                    + " collectsSize=" + lcManager.getCollectsSize(regency)
                    + " byzantineQuorum=" + bizantineQuorum
                    + " conditionBFT=" + conditionBFT + " conditionCFT=" + conditionCFT);
            if (conditionBFT || conditionCFT) {
                System.out.println("[SYNC-DEBUG] Calling catch_up for regency=" + regency);
                catch_up(regency);
            }

        } catch (IOException | ClassNotFoundException ex) {
            logger.error("Could not deserialize STOPDATA message",ex);
        }

    }

    // Processes SYNC messages that were not process upon reception, because they were
    // ahead of the replica's expected regency
    private void processSYNC(byte[] payload, int regency) {
        
        CertifiedDecision lastHighestCID = null;
        int currentCID = -1;
        HashSet<SignedObject> signedCollects = null;
        byte[] propose = null;
        int batchSize = -1;

        ByteArrayInputStream bis;
        ObjectInputStream ois;

        try { // deserialization of the message content

            bis = new ByteArrayInputStream(payload);
            ois = new ObjectInputStream(bis);

            lastHighestCID = (CertifiedDecision) ois.readObject();
            signedCollects = (HashSet<SignedObject>) ois.readObject();
            propose = (byte[]) ois.readObject();
            batchSize = ois.readInt();

            lcManager.setCollects(regency, signedCollects);
            
            currentCID = lastHighestCID.getCID() + 1;

            // Is the predicate "sound" true? Is the certificate for LastCID valid?
            if (lcManager.sound(lcManager.selectCollects(regency, currentCID)) && (!controller.getStaticConf().isBFT() || lcManager.hasValidProof(lastHighestCID))) {

                finalise(regency, lastHighestCID, signedCollects, propose, batchSize, false);
            }

            ois.close();
            bis.close();

        } catch (IOException | ClassNotFoundException ex) {
            logger.error("Could not deserialize SYNC message", ex);
        }
    }

    // Fetches synchronization messages that were not process upon reception,
    // because they were ahead of the replica's expected regency
    private Set<LCMessage> getOutOfContextLC(int type, int regency) {

        HashSet<LCMessage> result = new HashSet<>();

        for (LCMessage m : outOfContextLC) {

            if (m.getType() == type && m.getReg() == regency) {
                result.add(m);
            }

        }

        outOfContextLC.removeAll(result); // avoid memory leaks

        return result;
    }

    // Deserializes requests that were included in STOP messages
    private TOMMessage[] deserializeTOMMessages(byte[] playload) {

        ByteArrayInputStream bis;
        ObjectInputStream ois;

        TOMMessage[] requests = null;

        try { // deserialize the content of the STOP message

            bis = new ByteArrayInputStream(playload);
            ois = new ObjectInputStream(bis);

            boolean hasReqs = ois.readBoolean();

            if (hasReqs) {

                // Store requests that the other replica did not manage to order
                //TODO: The requests have to be verified!
                byte[] temp = (byte[]) ois.readObject();
                BatchReader batchReader = new BatchReader(temp,
                        controller.getStaticConf().getUseSignatures() == 1);
                requests = batchReader.deserialiseRequests(controller);
            } else {
                
                requests = new TOMMessage[0];
            }

            ois.close();
            bis.close();

        } catch (IOException | ClassNotFoundException ex) {
            logger.error("Could not serialize requests", ex);
        }

        return requests;

    }

    // Get requests that timed out and the requests received in STOP messages
    private List<TOMMessage> getRequestsToRelay() {

        List<TOMMessage> messages = lcManager.getCurrentRequestTimedOut();

        if (messages == null) {

            messages = new LinkedList<>();
        }

        // Include requests from STOP messages in my own STOP message
        List<TOMMessage> messagesFromSTOP = lcManager.getRequestsFromSTOP();
        if (messagesFromSTOP != null) {

            for (TOMMessage m : messagesFromSTOP) {

                if (!messages.contains(m)) {

                    messages.add(m);
                }
            }
        }

        logger.debug("I need to relay " + messages.size() + " requests");

        return messages;
    }

    //adds requests received via STOP messages to the client manager
    private void addSTOPedRequestsToClientManager() {

        List<TOMMessage> messagesFromSTOP = lcManager.getRequestsFromSTOP();
        if (messagesFromSTOP != null) {

            logger.debug("Adding to client manager the requests contained in STOP messages");

            for (TOMMessage m : messagesFromSTOP) {
                tom.requestReceived(m, false);

            }
        }

    }

    /**
     * Remove all STOP messages being retransmitted up until
     * the specified regency
     * @param regency The regency up to which STOP retransmission should be canceled
     */
    public void removeSTOPretransmissions(int regency) {

        Set<Integer> timers = requestsTimer.getTimers();

        for (int t : timers) {
            if (t <= regency) {
                requestsTimer.stopSTOP(t);
                requestsTimer.dropRegencyState(t);
            }
        }

        syncSentRegencies.removeIf(r -> r <= regency);

    }
    // this method is called when a timeout occurs or when a STOP message is recevied
    private void startSynchronization(int nextReg) {

        boolean condition;
        ObjectOutputStream out = null;
        ByteArrayOutputStream bos = null;
        
        if (this.controller.getStaticConf().isBFT()) {
            condition = lcManager.getStopsSize(nextReg) > this.controller.getCurrentViewF();
        } else {
            condition = lcManager.getStopsSize(nextReg) > 0;
        }
        
        // Ask to start the synchronizations phase if enough messages have been received already
        if (condition && lcManager.getNextReg() == lcManager.getLastReg()) {
            
            // Same escalation-choke-point guard as Synchronizer.triggerTimeout.
            // When we enter this branch because f+1 STOPs for some regency
            // arrived from peers, we're initiating LC locally — which counts
            // as an LC-epoch claim for the escalation debounce. Without the
            // claim here, a subsequent Acceptor null-propose on the same
            // replica would see lcEpochInFlight == false and happily
            // advance nextReg to r+1 as soon as Phase 2 installs. See
            // triggerTimeout() for the full rationale and LC_INVESTIGATION.md
            // "Stage 2, round 3".
            if (!requestsTimer.tryClaimLCEpoch()) {
                logger.info("startSynchronization: escalation to regency "
                        + (lcManager.getLastReg() + 1)
                        + " suppressed (LC epoch for regency " + nextReg
                        + " still in flight)");
                return;
            }

            logger.debug("Initialize synch phase");
            requestsTimer.Enabled(false);
            requestsTimer.stopTimer();

            lcManager.setNextReg(lcManager.getLastReg() + 1); // define next timestamp

            int regency = lcManager.getNextReg();

            // store information about message I am going to send
            lcManager.addStop(regency, this.controller.getStaticConf().getProcessId());

            //execManager.stop(); // stop execution of consensus

            //Get requests that timed out and the requests received in STOP messages
            //and add those STOPed requests to the client manager
            addSTOPedRequestsToClientManager();
            List<TOMMessage> messages = getRequestsToRelay();

            try { // serialize conent to send in the STOP message
                bos = new ByteArrayOutputStream();
                out = new ObjectOutputStream(bos);

                // Do I have messages to send in the STOP message?
                if (messages != null && messages.size() > 0) {

                    //TODO: If this is null, there was no timeout nor STOP messages.
                    //What shall be done then?
                    out.writeBoolean(true);
                    byte[] serialized = bb.makeBatch(messages, 0, 0, controller.getStaticConf().getUseSignatures() == 1);
                    out.writeObject(serialized);
                } else {
                    out.writeBoolean(false);
                    logger.warn("Strange... did not include any request in my STOP message for regency " + regency);
                }

                out.flush();
                bos.flush();

                byte[] payload = bos.toByteArray();
                out.close();
                bos.close();

                // send message STOP
                logger.info("Sending STOP message to install regency " + regency + " with " + (messages != null ? messages.size() : 0) + " request(s) to relay");

                LCMessage stop = new LCMessage(this.controller.getStaticConf().getProcessId(), TOMUtil.STOP, regency, payload);
                requestsTimer.setSTOP(regency, stop); // make replica re-transmit the stop message until a new regency is installed
                communication.send(this.controller.getCurrentViewOtherAcceptors(), stop);

            } catch (IOException ex) {
                logger.error("Could not deserialize STOP message", ex);
            }
        }
        
        if (this.controller.getStaticConf().isBFT()) {
            condition = lcManager.getStopsSize(nextReg) > (2 * this.controller.getCurrentViewF());
        } else {
            condition = lcManager.getStopsSize(nextReg) > this.controller.getCurrentViewF();
        }
        
        // Did the synchronization phase really started?
        //if (lcManager.getStopsSize(nextReg) > this.reconfManager.getQuorum2F() && lcManager.getNextReg() > lcManager.getLastReg()) {
        if (condition && lcManager.getNextReg() > lcManager.getLastReg()) {
            
            if (!execManager.stopped()) execManager.stop(); // stop consensus execution if more than f replicas sent a STOP message

            logger.debug("Installing regency " + lcManager.getNextReg());
            lcManager.setLastReg(lcManager.getNextReg()); // define last timestamp

            int regency = lcManager.getLastReg();

            // avoid memory leaks
            lcManager.removeStops(nextReg);
            lcManager.clearCurrentRequestTimedOut();
            lcManager.clearRequestsFromSTOP();

            requestsTimer.Enabled(true);
            requestsTimer.setShortTimeout(-1);
            requestsTimer.startTimer();

            //int leader = regency % this.reconfManager.getCurrentViewN(); // new leader
            int leader = lcManager.getNewLeader();
            int in = tom.getInExec(); // cid to execute
            int last = tom.getLastExec(); // last cid decided

            execManager.setNewLeader(leader);

            // If I am not the leader, I have to send a STOPDATA message to the elected leader
            if (leader != this.controller.getStaticConf().getProcessId()) {

                try { // serialize content of the STOPDATA message

                    bos = new ByteArrayOutputStream();
                    out = new ObjectOutputStream(bos);
                    
                    Consensus cons = null;
                    
                    // content of the last decided CID
                    if (last > -1) cons = execManager.getConsensus(last);

                    //Do I have info on my last executed consensus?
                    if (cons != null && cons.getDecisionEpoch() != null && cons.getDecisionEpoch().propValue != null) {
                        
                    out.writeBoolean(true);
                    out.writeInt(last);
                    //byte[] decision = exec.getLearner().getDecision();

                    byte[] decision = cons.getDecisionEpoch().propValue;
                    Set<ConsensusMessage> proof = cons.getDecisionEpoch().getProof();

                    out.writeObject(decision);
                    out.writeObject(proof);
                    // TODO: WILL BE NECESSARY TO ADD A PROOF!!!

                } else {
                    out.writeBoolean(false);
                    
                    ////// THIS IS TO CATCH A BUG!!!!!
                    if (last > -1) {
                        logger.debug("[DEBUG INFO FOR LAST CID #1]");

                        if (cons == null) {
                            if (last > -1) logger.debug("No consensus instance for cid " + last);

                        }
                        else if (cons.getDecisionEpoch() == null) {
                            logger.debug("No decision epoch for cid " + last);
                        } else {
                            logger.debug("epoch for cid: " + last + ": " + cons.getDecisionEpoch().toString());

                            if (cons.getDecisionEpoch().propValue == null) {
                                logger.debug("No propose for cid " + last);
                            } else {
                                logger.debug("Propose hash for cid " + last + ": " + Base64.encodeBase64String(tom.computeHash(cons.getDecisionEpoch().propValue)));
                            }
                        }
                    }

                }

                    if (in > -1) { // content of cid in execution

                        cons = execManager.getConsensus(in);

                        //cons.incEts(); // make the consensus advance to the next epoch
                        cons.setETS(regency); // make the consensus advance to the next epoch

                        //int ets = cons.getEts();
                        //cons.createEpoch(ets, controller);
                        cons.createEpoch(regency, controller);
                        //Logger.println("(Synchronizer.startSynchronization) incrementing ets of consensus " + cons.getId() + " to " + ets);
                        logger.debug("Incrementing ets of consensus " + cons.getId() + " to " + regency);

                        TimestampValuePair quorumWrites;
                        if (cons.getQuorumWrites() != null) {

                            quorumWrites = cons.getQuorumWrites();

                        } else {

                            quorumWrites = new TimestampValuePair(0, new byte[0]);
                        }

                        HashSet<TimestampValuePair> writeSet = cons.getWriteSet();

                        //CollectData collect = new CollectData(this.controller.getStaticConf().getProcessId(), in, ets, quorumWrites, writeSet);
                        CollectData collect = new CollectData(this.controller.getStaticConf().getProcessId(), in, regency, quorumWrites, writeSet);

                        SignedObject signedCollect = tom.sign(collect);

                        out.writeObject(signedCollect);

                    } else {

                        cons = execManager.getConsensus(last + 1);

                        //cons.incEts(); // make the consensus advance to the next epoch
                        cons.setETS(regency); // make the consensus advance to the next epoch
                        
                        //int ets = cons.getEts();
                        //cons.createEpoch(ets, controller);
                        cons.createEpoch(regency, controller);
                        //Logger.println("(Synchronizer.startSynchronization) incrementing ets of consensus " + cons.getId() + " to " + ets);
                        logger.debug("Incrementing ets of consensus " + cons.getId() + " to " + regency);

                        //CollectData collect = new CollectData(this.controller.getStaticConf().getProcessId(), last + 1, ets, new TimestampValuePair(0, new byte[0]), new HashSet<TimestampValuePair>());
                        CollectData collect = new CollectData(this.controller.getStaticConf().getProcessId(), last + 1, regency, new TimestampValuePair(0, new byte[0]), new HashSet<TimestampValuePair>());
                        
                        SignedObject signedCollect = tom.sign(collect);

                        out.writeObject(signedCollect);

                    }

                    out.flush();
                    bos.flush();

                    byte[] payload = bos.toByteArray();
                    out.close();
                    bos.close();

                    int[] b = new int[1];
                    b[0] = leader;

                    logger.info("Sending STOPDATA of regency " + regency);
                    // send message SYNC to the new leader
                    final LCMessage stopdataMsg = new LCMessage(
                            this.controller.getStaticConf().getProcessId(), TOMUtil.STOPDATA, regency, payload);
                    communication.send(b, stopdataMsg);

                    // Retransmit STOPDATA at 200ms sim cadence until SYNC is installed.
                    // STOPDATA is a unicast to the leader; a single lost frame means the
                    // leader never calls catch_up, so we keep resending until lastReg > regency.
                    final int retxRegency = regency;
                    final int[] retxDest = b;
                    final long retxSimMs = 200L;
                    final long retxWallMs = 200L;
                    // byzantineQuorum mirrors the processSTOPDATA check: (N+f)/2.
                    final int retxByzQuorum =
                            (controller.getCurrentViewN() + controller.getCurrentViewF()) / 2;
                    final Timer stopdataRetxTimer = new Timer("STOPDATA-retx-" + regency);
                    stopdataRetxTimer.schedule(new TimerTask() {
                        private long lastEmitSimMs = SimulationClock.currentTimeMillis();
                        @Override public void run() {
                            // Stop when a later regency is installed (escalation)
                            // or when this leader already has a Byzantine quorum
                            // of STOPDATAs (catch_up will fire soon / has fired).
                            // Without the quorum check the timer fires indefinitely
                            // after a successful LC (lastReg stays at retxRegency=1),
                            // causing processSTOPDATA to call catch_up repeatedly.
                            boolean escalated = lcManager.getLastReg() > retxRegency;
                            boolean quorumReached =
                                    lcManager.getLastCIDsSize(retxRegency) > retxByzQuorum;
                            if (escalated || quorumReached) {
                                stopdataRetxTimer.cancel();
                                return;
                            }
                            long nowSimMs = SimulationClock.currentTimeMillis();
                            if (nowSimMs - lastEmitSimMs >= retxSimMs) {
                                logger.info("Re-transmitting STOPDATA of regency " + retxRegency);
                                communication.send(retxDest, stopdataMsg);
                                lastEmitSimMs = nowSimMs;
                            }
                        }
                    }, retxWallMs, retxWallMs);

		//TODO: Turn on timeout again?
                } catch (IOException ex) {
                    logger.error("Could not deserialize STOPDATA message", ex);
                }

                // the replica might have received a SYNC that was out of context at the time it was received, but now can be processed
                Set<LCMessage> sync = getOutOfContextLC(TOMUtil.SYNC, regency);

                logger.debug("Checking if there are out of context SYNC for regency " + regency);

                if (sync.size() > 0) {
                    logger.info("Processing out of context SYNC for regency " + regency);
                } else {
                    logger.info("No out of context SYNC for regency " + regency);
                }

                for (LCMessage m : sync) {
                    if (m.getSender() == execManager.getCurrentLeader()) {
                        processSYNC(m.getPayload(), regency);
                        return; // makes no sense to continue, since there is only one SYNC message
                    }
                }

            } else { // If leader, I will store information that I would send in a SYNC message

                logger.debug("I'm the leader for this new regency");
                CertifiedDecision lastDec = null;
                CollectData collect = null;

                Consensus cons = null;
                
                //Content of the last decided CID
                if (last > -1) cons = execManager.getConsensus(last);
                        
                //Do I have info on my last executed consensus?
                if (cons != null && cons.getDecisionEpoch() != null && cons.getDecisionEpoch().propValue != null) { 
                    //byte[] decision = exec.getLearner().getDecision();


                    byte[] decision = cons.getDecisionEpoch().propValue;
                    Set<ConsensusMessage> proof = cons.getDecisionEpoch().getProof();

                    lastDec = new CertifiedDecision(this.controller.getStaticConf().getProcessId(), last, decision, proof);
                    // TODO: WILL BE NECESSARY TO ADD A PROOF!!!??

                } else {                    
                    lastDec = new CertifiedDecision(this.controller.getStaticConf().getProcessId(), last, null, null);

                    ////// THIS IS TO CATCH A BUG!!!!!
                    if (last > -1) {
                        logger.debug("[DEBUG INFO FOR LAST CID #2]");

                        if (cons == null) {
                            if (last > -1) logger.debug("No consensus instance for cid " + last);

                        }
                        else if (cons.getDecisionEpoch() == null) {
                            logger.debug("No decision epoch for cid " + last);
                        } else {
                            logger.debug("epoch for cid: " + last + ": " + cons.getDecisionEpoch().toString());
                        }
                        if (cons.getDecisionEpoch().propValue == null) {
                            logger.debug("No propose for cid " + last);
                        } else {
                            logger.debug("Propose hash for cid " + last + ": " + Base64.encodeBase64String(tom.computeHash(cons.getDecisionEpoch().propValue)));
                        }
                    }
                    
                }
                lcManager.addLastCID(regency, lastDec);

                if (in > -1) { // content of cid being executed
                    cons = execManager.getConsensus(in);

                    //cons.incEts(); // make the consensus advance to the next epoch
                    cons.setETS(regency); // make the consensus advance to the next epoch

                    //int ets = cons.getEts();
                    //cons.createEpoch(ets, controller);
                    cons.createEpoch(regency, controller);
                    //Logger.println("(Synchronizer.startSynchronization) incrementing ets of consensus " + cons.getId() + " to " + ets);
                    logger.debug("Incrementing ets of consensus " + cons.getId() + " to " + regency);

                    TimestampValuePair quorumWrites;

                    if (cons.getQuorumWrites() != null) {

                        quorumWrites = cons.getQuorumWrites();
                    } else {
                        quorumWrites = new TimestampValuePair(0, new byte[0]);
                    }

                    HashSet<TimestampValuePair> writeSet = cons.getWriteSet();

                    //collect = new CollectData(this.controller.getStaticConf().getProcessId(), in, ets, quorumWrites, writeSet);
                    collect = new CollectData(this.controller.getStaticConf().getProcessId(), in, regency, quorumWrites, writeSet);

                } else {

                    cons = execManager.getConsensus(last + 1);

                    //cons.incEts(); // make the consensus advance to the next epoch
                    cons.setETS(regency); // make the consensus advance to the next epoch

                    //int ets = cons.getEts();
                    //cons.createEpoch(ets, controller);
                    cons.createEpoch(regency, controller);
                    //Logger.println("startSynchronization) incrementing ets of consensus " + cons.getId() + " to " + ets);
                    logger.debug("Incrementing ets of consensus " + cons.getId() + " to " + regency);

                    //collect = new CollectData(this.controller.getStaticConf().getProcessId(), last + 1, ets, new TimestampValuePair(0, new byte[0]), new HashSet<TimestampValuePair>());
                    collect = new CollectData(this.controller.getStaticConf().getProcessId(), last + 1, regency, new TimestampValuePair(0, new byte[0]), new HashSet<TimestampValuePair>());
                }

                SignedObject signedCollect = tom.sign(collect);

                lcManager.addCollect(regency, signedCollect);

                // the replica might have received STOPDATAs that were out of context at the time they were received, but now can be processed
                Set<LCMessage> stopdatas = getOutOfContextLC(TOMUtil.STOPDATA, regency);

                logger.debug("Checking if there are out of context STOPDATAs for regency " + regency);
                if (stopdatas.size() > 0) {
                    logger.debug("Processing " + stopdatas.size() + " out of context STOPDATAs for regency " + regency);
                } else {
                    logger.debug("No out of context STOPDATAs for regency " + regency);
                }

                for (LCMessage m : stopdatas) {
                    processSTOPDATA(m, regency);
                }

            }

        }
    }

    /**
     * This method is called by the MessageHandler each time it received
     * messages related to the leader change
     *
     * @param msg Message received from the other replica
     */
    public void deliverTimeoutRequest(LCMessage msg) {

        switch (msg.getType()) {
            case TOMUtil.STOP: { // message STOP

                logger.info("Last regency: " + lcManager.getLastReg() + ", next regency: " + lcManager.getNextReg());

                // this message is for the next leader change?
                if (msg.getReg() == lcManager.getLastReg() + 1) {

                    logger.debug("Received regency change request");

                    TOMMessage[] requests = deserializeTOMMessages(msg.getPayload());

                    // store requests that came with the STOP message
                    lcManager.addRequestsFromSTOP(requests);

                    // store information about the message STOP
                    lcManager.addStop(msg.getReg(), msg.getSender());

                    processOutOfContextSTOPs(msg.getReg()); // the replica might have received STOPs
                                                            // that were out of context at the time they
                                                            // were received, but now can be processed

                    startSynchronization(msg.getReg()); // evaluate STOP messages

                } else if (msg.getReg() > lcManager.getLastReg()) { // send STOP to out of context if
                                                                    // it is for a future regency
                    logger.debug("Keeping STOP message as out of context for regency " + msg.getReg());
                    outOfContextLC.add(msg);

                } else {
                    logger.debug("Discarding STOP message");
                }
            }
            break;
            case TOMUtil.STOPDATA: { // STOPDATA messages

                int regency = msg.getReg();

                logger.info("Last regency: " + lcManager.getLastReg() + ", next regency: " + lcManager.getNextReg());

                // Am I the new leader, and am I expecting this messages?
                if (regency == lcManager.getLastReg()
                        && this.controller.getStaticConf().getProcessId() == execManager.getCurrentLeader()/*(regency % this.reconfManager.getCurrentViewN())*/) {

                    logger.debug("I'm the new leader and I received a STOPDATA");
                    processSTOPDATA(msg, regency);
                } else if (msg.getReg() > lcManager.getLastReg()) { // send STOPDATA to out of context if
                                                                    // it is for a future regency

                    logger.debug("Keeping STOPDATA message as out of context for regency " + msg.getReg());
                    outOfContextLC.add(msg);

                } else {
                    logger.debug("Discarding STOPDATA message");
                }
            }
            break;
            case TOMUtil.SYNC: { // message SYNC

                int regency = msg.getReg();

                logger.info("Last regency: " + lcManager.getLastReg() + ", next regency: " + lcManager.getNextReg());

                // I am expecting this sync?
                boolean isExpectedSync = (regency == lcManager.getLastReg() && regency == lcManager.getNextReg());

                // Is this sync what I wanted to get in the previous iteration of the synchoronization phase?
                boolean islateSync = (regency == lcManager.getLastReg() && regency == (lcManager.getNextReg() - 1));

                //Did I already sent a stopdata in this iteration?
                boolean sentStopdata = (lcManager.getStopsSize(lcManager.getNextReg()) == 0); //if 0, I already purged the stops,
                                                                                              //which I only do when I am about to
                                                                                              //send the stopdata

                // I am (or was) waiting for this message, and did I received it from the new leader?
                if ((isExpectedSync || // Expected case
                        (islateSync && !sentStopdata)) && // might happen if I timeout before receiving the SYNC
                        (msg.getSender() == execManager.getCurrentLeader())) {

                //if (msg.getReg() == lcManager.getLastReg() &&
                //		msg.getReg() == lcManager.getNextReg() && msg.getSender() == lm.getCurrentLeader()/*(regency % this.reconfManager.getCurrentViewN())*/) {
                    processSYNC(msg.getPayload(), regency);

                } else if (msg.getReg() > lcManager.getLastReg()) { // send SYNC to out of context if
                    // it is for a future regency
                    logger.debug("Keeping SYNC message as out of context for regency " + msg.getReg());
                    outOfContextLC.add(msg);

                } else {
                    logger.debug("Discarding SYNC message");
                }
            }
            break;

        }

    }

    // this method is used to verify if the leader can make the message catch-up
    // and also sends the message
    private void catch_up(int regency) {

        logger.debug("Verifying STOPDATA info");
        if (syncSentRegencies.contains(regency)) {
            logger.debug("SYNC for regency {} already sent; skipping duplicate catch_up", regency);
            return;
        }
        ObjectOutputStream out = null;
        ByteArrayOutputStream bos = null;

        CertifiedDecision lastHighestCID = lcManager.getHighestLastCID(regency);

        int currentCID = lastHighestCID.getCID() + 1;
        HashSet<SignedObject> signedCollects = null;
        byte[] propose = null;
        int batchSize = -1;

        // normalize the collects and apply to them the predicate "sound"
        HashSet<bftsmart.tom.leaderchange.CollectData> selColls = lcManager.selectCollects(regency, currentCID);
        boolean soundResult = lcManager.sound(selColls);
        System.out.println("[SYNC-DEBUG] catch_up regency=" + regency + " currentCID=" + currentCID
                + " selectCollects.size=" + (selColls == null ? "null" : selColls.size())
                + " sound=" + soundResult);
        if (soundResult) {

            logger.debug("Sound predicate is true");

            signedCollects = lcManager.getCollects(regency); // all original collects that the replica has received

            Decision dec = new Decision(-1); // the only purpose of this object is to obtain the batchsize,
                                                // using code inside of createPropose()

            // Reset alreadyProposed so that requests whose first propose was
            // dropped by a silent Byzantine leader are eligible for re-proposal.
            tom.clientsManager.resetAlreadyProposed();

            // V2V "Dynamic Reconstruction" hook (EP5 truth recovery): if the
            // application provides one, let it rebuild the pending requests from
            // current ground truth BEFORE createPropose() reads them. This is
            // what prevents the new leader from replaying the censored bytes
            // that triggered the view-change in the first place. No-op when the
            // hook is null (stock BFT-SMaRt demos) or when the app cannot
            // produce a fresh proposal (e.g. JNI unavailable).
            bftsmart.tom.server.ViewChangeRebuildHook rebuildHook = tom.getViewChangeRebuildHook();
            if (rebuildHook != null) {
                try {
                    System.out.println("[VIEW-CHANGE] Invoking ViewChangeRebuildHook for regency=" + regency
                            + " before createPropose");
                    rebuildHook.rebuildPendingProposals(tom.clientsManager, regency);
                } catch (Throwable t) {
                    logger.error("ViewChangeRebuildHook threw; falling back to replay of pending bytes", t);
                    System.err.println("[VIEW-CHANGE] ViewChangeRebuildHook failed: " + t.getMessage()
                            + " — falling back to replay");
                }
            }

            propose = tom.createPropose(dec);
            batchSize = dec.batchSize;
            
            try { // serialization of the CATCH-UP message
                bos = new ByteArrayOutputStream();
                out = new ObjectOutputStream(bos);

                out.writeObject(lastHighestCID);

		//TODO: Missing: serialization of the proof?
                out.writeObject(signedCollects);
                out.writeObject(propose);
                out.writeInt(batchSize);

                out.flush();
                bos.flush();

                byte[] payload = bos.toByteArray();
                out.close();
                bos.close();

                syncSentRegencies.add(regency);
                logger.info("Sending SYNC message for regency " + regency);
                System.out.println("[VIEW-CHANGE] Replica " + this.controller.getStaticConf().getProcessId()
                        + " is NEW LEADER — sending SYNC for regency " + regency);

                // send the CATCH-UP message
                communication.send(this.controller.getCurrentViewOtherAcceptors(),
                        new LCMessage(this.controller.getStaticConf().getProcessId(), TOMUtil.SYNC, regency, payload));

                finalise(regency, lastHighestCID, signedCollects, propose, batchSize, true);

            } catch (IOException ex) {
                logger.error("Could not serialize message", ex);
            }
        }
    }

    //This method is invoked by the state transfer protocol to notify the replica
    // that it can end synchronization
    public void resumeLC() {

        Consensus cons = execManager.getConsensus(tempLastHighestCID.getCID());
        Epoch e = cons.getLastEpoch();

        int ets = cons.getEts();

        if (e == null || e.getTimestamp() != ets) {
            e = cons.createEpoch(ets, controller);
        } else {
            e.clear();
        }

        byte[] hash = tom.computeHash(tempLastHighestCID.getDecision());
        e.propValueHash = hash;
        e.propValue = tempLastHighestCID.getDecision();

        e.deserializedPropValue = tom.checkProposedValue(tempLastHighestCID.getDecision(), false);

        finalise(tempRegency, tempLastHighestCID,
                tempSignedCollects, tempPropose, tempBatchSize, tempIAmLeader);

    }

    // this method is called on all replicas, and serves to verify and apply the
    // information sent in the catch-up message
    private void finalise(int regency, CertifiedDecision lastHighestCID,
            HashSet<SignedObject> signedCollects, byte[] propose, int batchSize, boolean iAmLeader) {

        int currentCID = lastHighestCID.getCID() + 1;
        logger.debug("Final stage of LC protocol");
        int me = this.controller.getStaticConf().getProcessId();
        Consensus cons = null;
        Epoch e = null;

        // V2V "Dynamic Reconstruction" follower-side cleanup: the new leader
        // has already evicted its stale pending request inside catch_up()
        // (and injected a fresh one) before this method ran. Followers,
        // however, reach finalise() via processSYNC() WITHOUT going through
        // catch_up(), so their RequestsTimer is still watching the deposed
        // Byzantine leader's censored PROPOSE_ALL. Without this eviction
        // the timer fires ~T_request later and forces a redundant
        // leader-change to regency+1 even though consensus on the rebuilt
        // proposal already succeeded. Calling evictStaleProposals on the
        // leader is a harmless no-op (its pending list is already empty).
        bftsmart.tom.server.ViewChangeRebuildHook rebuildHook = tom.getViewChangeRebuildHook();
        if (rebuildHook != null) {
            try {
                System.out.println("[VIEW-CHANGE] Replica " + me
                        + " invoking evictStaleProposals (iAmLeader=" + iAmLeader
                        + ", regency=" + regency + ") in finalise");
                rebuildHook.evictStaleProposals(tom.clientsManager, regency);
            } catch (Throwable t) {
                logger.error("evictStaleProposals threw; followers may still trigger a redundant view-change", t);
                System.err.println("[VIEW-CHANGE] evictStaleProposals failed on replica " + me
                        + ": " + t.getMessage());
            }
        }

        if (tom.getLastExec() + 1 < lastHighestCID.getCID()) { // is this a delayed replica?

            logger.info("NEEDING TO USE STATE TRANSFER!! (" + lastHighestCID.getCID() + ")");

            tempRegency = regency;
            tempLastHighestCID = lastHighestCID;
            tempSignedCollects = signedCollects;
            tempPropose = propose;
            tempBatchSize = batchSize;
            tempIAmLeader = iAmLeader;

            execManager.getStoppedMsgs().add(acceptor.getFactory().createPropose(currentCID, 0, propose));
            stateManager.requestAppState(lastHighestCID.getCID());

            return;

        } /*else if (tom.getLastExec() + 1 == lastHighestCID.getCID()) { // Is this replica still executing the last decided consensus?

            System.out.println("(Synchronizer.finalise) I'm still at the CID before the most recent one!!! (" + lastHighestCID.getCID() + ")");

            cons = execManager.getConsensus(lastHighestCID.getCID());
            e = cons.getLastEpoch();
            
            int ets = cons.getEts();
            
            if (e == null || e.getTimestamp() != ets) {
                e = cons.createEpoch(ets, controller);
            } else {
                e.clear();
            }
            
            byte[] hash = tom.computeHash(lastHighestCID.getCIDDecision());
            e.propValueHash = hash;
            e.propValue = lastHighestCID.getCIDDecision();

            e.deserializedPropValue = tom.checkProposedValue(lastHighestCID.getCIDDecision(), false);
            cons.decided(e, true); // pass the decision to the delivery thread
        }*/
        
        // install proof of the last decided consensus
        cons = execManager.getConsensus(lastHighestCID.getCID());
        e = null;
                
        Set<ConsensusMessage> consMsgs = lastHighestCID.getConsMessages();
        if (consMsgs == null) consMsgs = new HashSet();
        
        for (ConsensusMessage cm : consMsgs) {
            
            if (e == null) e = cons.getEpoch(cm.getEpoch(), true, controller);
            if (e.getTimestamp() != cm.getEpoch()) {
                logger.warn("Strange... proof of last decided consensus contains messages from more than just one epoch");
                e = cons.getEpoch(cm.getEpoch(), true, controller);
            }
            e.addToProof(cm);
            
            if (cm.getType() == MessageFactory.ACCEPT) {
                e.setAccept(cm.getSender(), cm.getValue());
            }
            
            else if (cm.getType() == MessageFactory.WRITE) {
                e.setWrite(cm.getSender(), cm.getValue());
            }
            
            
        }
        if (e != null) {

            logger.info("Installed proof of last decided consensus " + lastHighestCID.getCID());
            
            byte[] hash = tom.computeHash(lastHighestCID.getDecision());
            e.propValueHash = hash;
            e.propValue = lastHighestCID.getDecision();
            e.deserializedPropValue = tom.checkProposedValue(lastHighestCID.getDecision(), false);

            // Is this replica still executing the last decided consensus?
            if (tom.getLastExec() + 1 == lastHighestCID.getCID()) {
                
                logger.info("I'm still at the CID before the most recent one!!! (" + lastHighestCID.getCID() + ")");
                cons.decided(e, true);
            }
            else {
                cons.decided(e, false);
            }

        } else {
            logger.info("I did not install any proof of last decided consensus " + lastHighestCID.getCID());
        }
        
        cons = null;
        e = null;
        
        // get a value that satisfies the predicate "bind"
        byte[] tmpval = null;
        HashSet<CollectData> selectedColls = lcManager.selectCollects(signedCollects, currentCID, regency);

        tmpval = lcManager.getBindValue(selectedColls);
        logger.debug("Trying to find a binded value");

        // If such value does not exist, obtain the value written by the new leader
        boolean unboundResult = lcManager.unbound(selectedColls);
        System.out.println("[SYNC-DEBUG] finalise regency=" + regency + " currentCID=" + currentCID
                + " selectedColls.size=" + (selectedColls == null ? "null" : selectedColls.size())
                + " bindValue=" + (tmpval == null ? "null" : "non-null")
                + " unbound=" + unboundResult
                + " propose=" + (propose == null ? "null" : propose.length + "bytes"));
        if (tmpval == null && unboundResult) {
            logger.debug("Did not found a value that might have already been decided");
            tmpval = propose;
        } else {
            logger.debug("Found a value that might have been decided");
        }

        if (tmpval != null) { // did I manage to get some value?

            logger.debug("Resuming normal phase");
            lcManager.removeCollects(regency); // avoid memory leaks

            // stop the re-transmission of the STOP message for all regencies up to this one
            removeSTOPretransmissions(regency);
            
            cons = execManager.getConsensus(currentCID);

            e = cons.getLastEpoch();

            int ets = cons.getEts();

            //Update current consensus with latest ETS. This may be necessary
            //if I 'jumped' to a consensus instance ahead of the one I was executing
                   
            //int currentETS = lcManager.getETS(currentCID, selectedColls);
            //if (currentETS > ets) {
            if (regency > ets) {
                
                //System.out.println("(Synchronizer.finalise) Updating consensus' ETS after SYNC (from " + ets + " to " + currentETS +")");
                logger.debug("Updating consensus' ETS after SYNC (from " + ets + " to " + regency +")");

                /*do {
                    cons.incEts();
                } while (cons.getEts() != currentETS);*/
                
                cons.setETS(regency);
                
                //cons.createEpoch(currentETS, controller);
                cons.createEpoch(regency, controller);
                
                e = cons.getLastEpoch();
            }

            // Make sure the epoch is created
            /*if (e == null || e.getTimestamp() != ets) {
                e = cons.createEpoch(ets, controller);
            } else {
                e.clear();
            }*/
            if (e == null || e.getTimestamp() != regency) {
                e = cons.createEpoch(regency, controller);
            } else {
                e.clear();
            }
            
            /********* LEADER CHANGE CODE ********/
            cons.removeWritten(tmpval);
            cons.addWritten(tmpval);
            /*************************************/

            System.out.println("[VIEW-CHANGE] Replica " + me + " INSTALLED regency=" + regency
                    + " new_leader=" + (regency % this.controller.getCurrentViewN())
                    + " CID=" + currentCID + " — resuming consensus");

            byte[] hash = tom.computeHash(tmpval);
            e.propValueHash = hash;
            e.propValue = tmpval;

            e.deserializedPropValue = tom.checkProposedValue(tmpval, false);

            if (cons.getDecision().firstMessageProposed == null) {
                if (e.deserializedPropValue != null
                        && e.deserializedPropValue.length > 0) {
                    cons.getDecision().firstMessageProposed = e.deserializedPropValue[0];
                } else {
                    cons.getDecision().firstMessageProposed = new TOMMessage(); // to avoid null pointer
                }
            }
            if (this.controller.getStaticConf().isBFT()) {
                e.setWrite(me, hash);
            } else {
                e.setAccept(me, hash);

                /********* LEADER CHANGE CODE ********/
                logger.debug("[CFT Mode] Setting consensus " + currentCID + " QuorumWrite tiemstamp to " + e.getConsensus().getEts() + " and value " + Arrays.toString(hash));
 	        e.getConsensus().setQuorumWrites(hash);
                /*************************************/

            }

            // resume normal operation
            execManager.restart();
            //leaderChanged = true;
            tom.setInExec(currentCID);
            if (iAmLeader) {
                logger.debug("Waking up proposer thread");
                tom.imAmTheLeader();
            } // waik up the thread that propose values in normal operation

            // send a WRITE/ACCEPT message to the other replicas
            if (this.controller.getStaticConf().isBFT()) {
                logger.info("Sending WRITE message for CID " + currentCID + ", timestamp " + e.getTimestamp() + ", value " + Arrays.toString(e.propValueHash));
                communication.send(this.controller.getCurrentViewOtherAcceptors(),
                        acceptor.getFactory().createWrite(currentCID, e.getTimestamp(), e.propValueHash));
                e.writeSent();
            } else {
                logger.info("Sending ACCEPT message for CID " + currentCID + ", timestamp " + e.getTimestamp() + ", value " + Arrays.toString(e.propValueHash));
                communication.send(this.controller.getCurrentViewOtherAcceptors(),
                        acceptor.getFactory().createAccept(currentCID, e.getTimestamp(), e.propValueHash));
                e.acceptSent();
            }
        } else {
            System.out.println("[SYNC-DEBUG] *** SYNC PHASE FAILED for regency=" + regency
                    + " tmpval=" + (tmpval == null ? "null" : "non-null")
                    + " propose=" + (propose == null ? "null" : propose.length + "bytes"));
            logger.warn("Sync phase failed for regency" + regency);
        }
    }

}
