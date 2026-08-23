//
// Copyright (C) 2026 Mathesh Kumar
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

//
// @titlepage
// <h1>V2VBFT</h1>
//
// BFT consensus for autonomous vehicles at an unsignalled intersection.
// Vehicles agree on a safe crossing order using PBFT, with an
// arrival-certificate layer that proves a vehicle really is where it claims
// before its entry can enter a proposal.
//
// Built on Veins (802.11p radio, SUMO mobility) and ResilientDB (PBFT).
//

package org.v2vbft;
