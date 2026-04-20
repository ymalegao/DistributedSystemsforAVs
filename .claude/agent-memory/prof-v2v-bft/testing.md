set up the enviornment: 

in /omnet/omnetpp-6.2.0:
 source setenv
 opp_env shell
 source ~/.zshrc

this allows us to use makeveins to rebuild c++ and makebft to rebuild java

then in /Users/yashmalegaonkar/Documents/v2v/fourway/config/hosts.config, 
we set the number of cars for that round. 

For example with n = 4:
0 127.0.0.1 11000 11001
1 127.0.0.1 11010 11011
2 127.0.0.1 11020 11021
3 127.0.0.1 11030 11031

The rest of the cars are already set up there just need to be uncommented. For n = 16, the last car would be #15 127.0.0.1 11150 11151

Then in /Users/yashmalegaonkar/Documents/v2v/fourway/config/system.config

We set the following: 
#Number of servers in the group 
system.servers.num = 4

#Maximum number of faulty replicas
system.servers.f = 1 <- this needs to be 3f+1 = total number of cars. for n=16, we can have 5. 


############################################
###### Reconfiguration Configurations ######
############################################

#Replicas ID for the initial view, separated by a comma.
# The number of replicas in this parameter should be equal to that specified in 'system.servers.num'
system.initial.view = 0,1,2,3

#,4,5,6,7

#,8,9,10,11

#,12,13,14,15

We also set this to be the inital view. 

if we have a byzantine node, Byzantine fault injection: comma-separated list of replica IDs that act as hash-tampering Byzantine nodes.
# Leave empty (or omit) to disable. Replica IDs are 0-indexed.
# n=4,  f=1 example: system.byzantine.maliciousReplicaIds = 2
# n=16, f=4 example: system.byzantine.maliciousReplicaIds = 2,3,4,5
system.byzantine.maliciousReplicaIds = 0 

we set it here. When we test an honest config, its important to make sure that we set 
system.byzantine.maliciousReplicaIds =      (blank) its also important that if we are testing byzantine followers, 0 should not be included, because that would make it a byzantine leader. 

Then in  veins-veins-5.3.1/src/veins/modules/bftsmart/V2VProxyModule.h

we change this to match the config,   static const int BATCH_SIZE = 4; once we change this, we do makeveins

and then in /Users/yashmalegaonkar/Documents/v2v/bftsmart/library/src/main/java/bftsmart/demo/intersection/ServerRunner.java, we change   

private static final int BATCH_SIZE = 4; and do makebft. 

Once that is done, 

we can run 

For ByzLeader n = 4 : runomnetnogui -c FourVehiclesAmbulanceBFT --randomize 4 0 --byzleader 0 --sync-java , and once it finishes (I manually check when all cars have left, press control c to stop the simulation, and then do  fourway % python analyze_log.py --cars 4 --scenario 4 --save-to benchmarks/Priority4cars will save one log. 

For ByzFollowers: runomnetnogui -c FourVehiclesAmbulanceBFT --randomize 4 1 --sync-java, and once it finishes, fourway % python analyze_log.py --cars 4 --scenario 3 --save-to benchmarks/Priority4cars

For Honest Ambulance: runomnetnogui -c FourVehiclesAmbulanceBFT --randomize 4 0 --sync-java

For No Ambulance: runomnetnogui -c FourVehiclesBFTOverV2V --randomize 4 0 --sync-java

FourVehiclesAmbulanceBFT
EightVehiclesAmbulanceBFT
TwelveVehiclesAmbulanceBFT
SixteenVehiclesAmbulanceBFT

The rest of the configs are in /Users/yashmalegaonkar/Documents/v2v/fourway/omnetpp.ini 

