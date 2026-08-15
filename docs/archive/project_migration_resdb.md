---
name: Migration to ResDBIntersectionApp
description: Protocol implementation is in ResDBIntersectionApp.cc, NOT V2VProxyModule. This is the active file.
type: project
---

Protocol is implemented in ResDBIntersectionApp.cc (and .h, .ned), NOT V2VProxyModule.

Path: veins-veins-5.3.1/src/veins/modules/application/resDB/ResDBIntersectionApp.cc

**Why:** Migration to resDB was done several weeks ago (commit "migration to resdb done").
**How to apply:** All future edits for arrival protocol, echo handling, cert collection, view change, etc. go in ResDBIntersectionApp.cc — never in V2VProxyModule.cc / V2VArrivalProtocol.cc.
