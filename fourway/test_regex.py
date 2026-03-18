import re

lines = [
    "[V2VProxy 11] handleOrderDecision: veh0:GO:0;veh12:GO:1;veh4:GO:2;veh8:GO:3",
    "[RESUME] Replica 3: JNI received GO signal with delay=2.57142857142857s. Queued for main thread (queue size=1)",
    "[HANDLE-SELF-MSG] Replica 3: Message #1574 at t=14.571428571429 msgName=resumeVehicle",
    "[ORDER] NOTIFIED C++ ORDER DECIDED: veh11:GO:0;veh15:GO:3;veh3:GO:1;veh7:GO:2",
    "[ORDER] LATE-NOTIFY C++ ORDER DECIDED (was departed): veh0:GO:0;veh12:GO:1;veh4:GO:2;veh8:GO:3"
]

RE_GO_STRING = re.compile(r'(veh\d+):GO:\d+')
RE_ORDER_DECISION = re.compile(r'\[V2VProxy\s+\d+\] handleOrderDecision: (.*)|\[ORDER\] (?:LATE-)?NOTIFIED C\+\+ ORDER DECIDED(?: \(was departed\))?: (.*)')
RE_RESUME = re.compile(r'\[RESUME\] Replica (\d+): JNI received GO signal')
RE_RESUME_MSG = re.compile(r'\[HANDLE-SELF-MSG\] Replica (\d+): .*msgName=resumeVehicle')

for line in lines:
    m = RE_ORDER_DECISION.search(line)
    if m:
        decision = m.group(1) or m.group(2)
        print("DECISION:", RE_GO_STRING.findall(decision))
    m = RE_RESUME.search(line)
    if m:
        print("RESUME:", m.group(1))
    m = RE_RESUME_MSG.search(line)
    if m:
        print("EXEC:", m.group(1))
