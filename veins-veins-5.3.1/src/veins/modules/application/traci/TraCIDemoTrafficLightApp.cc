//
// Copyright (C) 2018 Tobias Hardes <hardes@ccs-labs.org>
//
// Documentation for these modules is at http://veins.car2x.org/
//
// SPDX-License-Identifier: GPL-2.0-or-later
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//

#include "veins/modules/application/traci/TraCIDemoTrafficLightApp.h"

#include "veins/modules/messages/DemoSafetyMessage_m.h"

using veins::TraCIDemoTrafficLightApp;

Define_Module(TraCIDemoTrafficLightApp);

TraCIDemoTrafficLightApp::TraCIDemoTrafficLightApp()
{
}

TraCIDemoTrafficLightApp::~TraCIDemoTrafficLightApp()
{
    // Defensive teardown: in some embedded/JNI shutdown paths this module's
    // inherited periodic-event pointers may already be invalid by the time
    // base destructors run. Null them so DemoBaseApplLayer won't touch them.
    sendBeaconEvt = nullptr;
    sendWSAEvt = nullptr;
}

void TraCIDemoTrafficLightApp::initialize(int stage)
{
    // Ensure DemoBaseApplLayer allocates and manages its self-messages.
    DemoBaseApplLayer::initialize(stage);
}

void TraCIDemoTrafficLightApp::onBSM(DemoSafetyMessage* bsm)
{
    delete bsm;
}

void TraCIDemoTrafficLightApp::handleLowerMsg(cMessage* msg)
{
    delete msg;
}

void TraCIDemoTrafficLightApp::handleMessage(cMessage* msg)
{
    delete msg;
}
