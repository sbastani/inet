/*
 * Copyright (C) 2003 Andras Varga; CTIE, Monash University, Australia
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <string.h>
#include <omnetpp.h>
#include "EtherMAC.h"
#include "Ieee802Ctrl_m.h"
#include "IPassiveQueue.h"



static std::ostream& operator<< (std::ostream& out, cMessage *msg)
{
    out << "(" << msg->getClassName() << ")" << msg->getFullName();
    return out;
}


Define_Module( EtherMAC );

EtherMAC::EtherMAC()
{
    frameBeingReceived = NULL;
    endJammingMsg = endRxMsg = endBackoffMsg = NULL;
}

EtherMAC::~EtherMAC()
{
    delete frameBeingReceived;
    cancelAndDelete(endRxMsg);
    cancelAndDelete(endBackoffMsg);
    cancelAndDelete(endJammingMsg);
}

void EtherMAC::initialize()
{
    EtherMACBase::initialize();

    endRxMsg = new cMessage("EndReception", ENDRECEPTION);
    endBackoffMsg = new cMessage("EndBackoff", ENDBACKOFF);
    endJammingMsg = new cMessage("EndJamming", ENDJAMMING);

    duplexMode = par("duplexEnabled");
    calculateParameters();

    // initialize state info
    backoffs = 0;
    numConcurrentTransmissions = 0;

    WATCH(backoffs);
    WATCH(numConcurrentTransmissions);

    // initialize statistics
    totalCollisionTime = 0.0;
    totalSuccessfulRxTxTime = 0.0;
    numCollisions = numBackoffs = 0;

    WATCH(numCollisions);
    WATCH(numBackoffs);

    numCollisionsVector.setName("collisions");
    numBackoffsVector.setName("backoffs");
}

void EtherMAC::initializeTxrate()
{
}

void EtherMAC::handleMessage (cMessage *msg)
{
    if (disabled)
    {
        EV << "MAC is disabled -- dropping message " << msg << "\n";
        delete msg;
        return;
    }

    printState();
    // some consistency check
    if (!duplexMode && transmitState==TRANSMITTING_STATE && receiveState!=RX_IDLE_STATE)
        error("Inconsistent state -- transmitting and receiving at the same time");

    if (!msg->isSelfMessage())
    {
        // either frame from upper layer, or frame/jam signal from the network
        if (msg->getArrivalGate() == gate("upperLayerIn"))
            processFrameFromUpperLayer(check_and_cast<EtherFrame *>(msg));
        else
            processMsgFromNetwork(PK(msg));
    }
    else
    {
        // Process different self-messages (timer signals)
        EV << "Self-message " << msg << " received\n";
        switch (msg->getKind())
        {
            case ENDIFG:
                handleEndIFGPeriod();
                break;

            case ENDTRANSMISSION:
                handleEndTxPeriod();
                break;

            case ENDRECEPTION:
                handleEndRxPeriod();
                break;

            case ENDBACKOFF:
                handleEndBackoffPeriod();
                break;

            case ENDJAMMING:
                handleEndJammingPeriod();
                break;

            case ENDPAUSE:
                handleEndPausePeriod();
                break;

            default:
                error("self-message with unexpected message kind %d", msg->getKind());
        }
    }
    printState();

    if (ev.isGUI())
        updateDisplayString();
}


void EtherMAC::processFrameFromUpperLayer(EtherFrame *frame)
{
    EtherMACBase::processFrameFromUpperLayer(frame);

    if ((duplexMode || receiveState==RX_IDLE_STATE) && transmitState==TX_IDLE_STATE)
    {
        EV << "No incoming carrier signals detected, frame clear to send, wait IFG first\n";
        scheduleEndIFGPeriod();
    }
}


void EtherMAC::processMsgFromNetwork(cPacket *msg)
{
    EtherMACBase::processMsgFromNetwork(msg);

    simtime_t endRxTime = simTime() + msg->getDuration();

    if (!duplexMode && transmitState==TRANSMITTING_STATE)
    {
        // since we're halfduplex, receiveState must be RX_IDLE_STATE (asserted at top of handleMessage)
        if (dynamic_cast<EtherJam*>(msg) != NULL)
            error("Stray jam signal arrived while transmitting (usual cause is cable length exceeding allowed maximum)");

        EV << "Transmission interrupted by incoming frame, handling collision\n";
        cancelEvent(endTxMsg);

        EV << "Transmitting jam signal\n";
        sendJamSignal(); // backoff will be executed when jamming finished

        // set receive state and schedule end of reception
        receiveState = RX_COLLISION_STATE;
        numConcurrentTransmissions++;

        cPacket jam;
        jam.setByteLength(JAM_SIGNAL_BYTES);
        simtime_t endJamTime = simTime() + transmissionChannel->calculateDuration(&jam);
        scheduleAt(endRxTime < endJamTime ? endJamTime : endRxTime, endRxMsg);
        delete msg;

        numCollisions++;
        numCollisionsVector.record(numCollisions);
    }
    else if (receiveState==RX_IDLE_STATE)
    {
        if (dynamic_cast<EtherJam*>(msg) != NULL)
            error("Stray jam signal arrived (usual cause is cable length exceeding allowed maximum)");

        EV << "Start reception of frame\n";
        numConcurrentTransmissions++;
        if (frameBeingReceived)
            error("frameBeingReceived!=0 in RX_IDLE_STATE");
        frameBeingReceived = (EtherFrame *)msg;
        scheduleEndRxPeriod(msg);
        channelBusySince = simTime();
    }
    else if (receiveState==RECEIVING_STATE
            && dynamic_cast<EtherJam*>(msg)==NULL
            && endRxMsg->getArrivalTime()-simTime() < 1.0/txrate)
    {
        // With the above condition we filter out "false" collisions that may occur with
        // back-to-back frames. That is: when "beginning of frame" message (this one) occurs
        // BEFORE "end of previous frame" event (endRxMsg) -- same simulation time,
        // only wrong order.

        EV << "Back-to-back frames: completing reception of current frame, starting reception of next one\n";

        // complete reception of previous frame
        cancelEvent(endRxMsg);
        EtherFrame *frame = frameBeingReceived;
        frameBeingReceived = NULL;
        frameReceptionComplete(frame);

        // start receiving next frame
        frameBeingReceived = (EtherFrame *)msg;
        scheduleEndRxPeriod(msg);
    }
    else // (receiveState==RECEIVING_STATE || receiveState==RX_COLLISION_STATE)
    {
        // handle overlapping receptions
        if (dynamic_cast<EtherJam*>(msg) != NULL)
        {
            if (numConcurrentTransmissions<=0)
                error("numConcurrentTransmissions=%d on jam arrival (stray jam?)",numConcurrentTransmissions);

            numConcurrentTransmissions--;
            EV << "Jam signal received, this marks end of one transmission\n";

            // by the time jamming ends, all transmissions will have been aborted
            if (numConcurrentTransmissions==0)
            {
                EV << "Last jam signal received, collision will ends when jam ends\n";
                cancelEvent(endRxMsg);
                scheduleAt(endRxTime, endRxMsg);
            }
        }
        else // EtherFrame or EtherPauseFrame
        {
            numConcurrentTransmissions++;
            if (endRxMsg->getArrivalTime() < endRxTime)
            {
                // otherwise just wait until the end of the longest transmission
                EV << "Overlapping receptions -- setting collision state and extending collision period\n";
                cancelEvent(endRxMsg);
                scheduleAt(endRxTime, endRxMsg);
            }
            else
            {
                EV << "Overlapping receptions -- setting collision state\n";
            }
        }

        // delete collided frames: arrived frame as well as the one we're currently receiving
        delete msg;
        if (receiveState==RECEIVING_STATE)
        {
            delete frameBeingReceived;
            frameBeingReceived = NULL;

            numCollisions++;
            numCollisionsVector.record(numCollisions);
        }

        // go to collision state
        receiveState = RX_COLLISION_STATE;
    }
}


void EtherMAC::handleEndIFGPeriod()
{
    EtherMACBase::handleEndIFGPeriod();

    // End of IFG period, okay to transmit, if Rx idle OR duplexMode
    cPacket *frame = (cPacket *)txQueue.front();

    // Perform carrier extension if in Gigabit Ethernet
    if (carrierExtension && frame->getByteLength() < GIGABIT_MIN_FRAME_WITH_EXT)
    {
        EV << "Performing carrier extension of small frame\n";
        frame->setByteLength(GIGABIT_MIN_FRAME_WITH_EXT);
    }

    // send frame to network
    startFrameTransmission();
}

void EtherMAC::startFrameTransmission()
{
    cPacket *origFrame = (cPacket *)txQueue.front();
    EV << "Transmitting a copy of frame " << origFrame << endl;
    cPacket *frame = origFrame->dup();

    // add preamble and SFD (Starting Frame Delimiter), then send out
    frame->addByteLength(PREAMBLE_BYTES+SFD_BYTES);
    if (ev.isGUI())  updateConnectionColor(TRANSMITTING_STATE);
    send(frame, physOutGate);
    transmissionChannel->getTransmissionFinishTime();

    // update burst variables
    if (frameBursting)
    {
        bytesSentInBurst = frame->getByteLength();
        framesSentInBurst++;
    }

    // check for collisions (there might be an ongoing reception which we don't know about, see below)
    if (!duplexMode && receiveState!=RX_IDLE_STATE)
    {
        // During the IFG period the hardware cannot listen to the channel,
        // so it might happen that receptions have begun during the IFG,
        // and even collisions might be in progress.
        //
        // But we don't know of any ongoing transmission so we blindly
        // start transmitting, immediately collide and send a jam signal.
        //
        sendJamSignal();
        // numConcurrentTransmissions stays the same: +1 transmission, -1 jam

        if (receiveState==RECEIVING_STATE)
        {
            delete frameBeingReceived;
            frameBeingReceived = NULL;

            numCollisions++;
            numCollisionsVector.record(numCollisions);
        }
        // go to collision state
        receiveState = RX_COLLISION_STATE;
    }
    else
    {
        // no collision
        scheduleEndTxPeriod(frame);

        // only count transmissions in totalSuccessfulRxTxTime if channel is half-duplex
        if (!duplexMode)
            channelBusySince = simTime();
    }
}

void EtherMAC::handleEndTxPeriod()
{
    EtherMACBase::handleEndTxPeriod();

    // only count transmissions in totalSuccessfulRxTxTime if channel is half-duplex
    if (!duplexMode)
    {
        simtime_t dt = simTime()-channelBusySince;
        totalSuccessfulRxTxTime += dt;
    }

    backoffs = 0;

    // check for and obey received PAUSE frames after each transmission
    if (checkAndScheduleEndPausePeriod())
        return;

    // Gigabit Ethernet: now decide if we transmit next frame right away (burst) or wait IFG
    // FIXME! this is not entirely correct, there must be IFG between burst frames too
    bool burstFrame=false;
    if (frameBursting && !txQueue.empty())
    {
        // check if max bytes for burst not exceeded
        if (bytesSentInBurst<GIGABIT_MAX_BURST_BYTES)
        {
             burstFrame=true;
             EV << "Transmitting next frame in current burst\n";
        }
        else
        {
             EV << "Next frame does not fit in current burst\n";
        }
    }

    if (burstFrame)
        startFrameTransmission();
    else
        beginSendFrames();
}

void EtherMAC::handleEndRxPeriod()
{
    EV << "Frame reception complete\n";
    simtime_t dt = simTime()-channelBusySince;
    if (receiveState==RECEIVING_STATE) // i.e. not RX_COLLISION_STATE
    {
        EtherFrame *frame = frameBeingReceived;
        frameBeingReceived = NULL;
        frameReceptionComplete(frame);
        totalSuccessfulRxTxTime += dt;
    }
    else
    {
        totalCollisionTime += dt;
    }

    receiveState = RX_IDLE_STATE;
    numConcurrentTransmissions = 0;

    if (transmitState==TX_IDLE_STATE && !txQueue.empty())
    {
        EV << "Receiver now idle, can transmit frames in output buffer after IFG period\n";
        scheduleEndIFGPeriod();
    }
}

void EtherMAC::handleEndBackoffPeriod()
{
    if (transmitState != BACKOFF_STATE)
        error("At end of BACKOFF not in BACKOFF_STATE!");
    if (txQueue.empty())
        error("At end of BACKOFF and buffer empty!");

    if (receiveState==RX_IDLE_STATE)
    {
        EV << "Backoff period ended, wait IFG\n";
        scheduleEndIFGPeriod();
    }
    else
    {
        EV << "Backoff period ended but channel not free, idling\n";
        transmitState = TX_IDLE_STATE;
    }
}

void EtherMAC::handleEndJammingPeriod()
{
    if (transmitState != JAMMING_STATE)
        error("At end of JAMMING not in JAMMING_STATE!");
    EV << "Jamming finished, executing backoff\n";
    handleRetransmission();
}

void EtherMAC::sendJamSignal()
{
    cPacket *jam = new EtherJam("JAM_SIGNAL");
    jam->setByteLength(JAM_SIGNAL_BYTES);
    if (ev.isGUI())
        updateConnectionColor(JAMMING_STATE);
    transmissionChannel->forceTransmissionFinishTime(SIMTIME_ZERO);
    send(jam, physOutGate);

    scheduleAt(transmissionChannel->getTransmissionFinishTime(), endJammingMsg);
    transmitState = JAMMING_STATE;
}

void EtherMAC::scheduleEndRxPeriod(cPacket *frame)
{
    scheduleAt(simTime()+frame->getDuration(), endRxMsg);
    receiveState = RECEIVING_STATE;
}

void EtherMAC::handleRetransmission()
{
    if (++backoffs > MAX_ATTEMPTS)
    {
        EV << "Number of retransmit attempts of frame exceeds maximum, cancelling transmission of frame\n";
        delete txQueue.pop();

        transmitState = TX_IDLE_STATE;
        backoffs = 0;
        // no beginSendFrames(), because end of jam signal sending will trigger it automatically
        return;
    }

    EV << "Executing backoff procedure\n";
    int backoffrange = (backoffs>=BACKOFF_RANGE_LIMIT) ? 1024 : (1 << backoffs);
    int slotNumber = intuniform(0,backoffrange-1);
    simtime_t backofftime = slotNumber * slotTime;

    scheduleAt(simTime()+backofftime, endBackoffMsg);
    transmitState = BACKOFF_STATE;

    numBackoffs++;
    numBackoffsVector.record(numBackoffs);
}

void EtherMAC::printState()
{
#define CASE(x) case x: EV << #x; break
    EV << "transmitState: ";
    switch (transmitState) {
        CASE(TX_IDLE_STATE);
        CASE(WAIT_IFG_STATE);
        CASE(TRANSMITTING_STATE);
        CASE(JAMMING_STATE);
        CASE(BACKOFF_STATE);
        CASE(PAUSE_STATE);
    }
    EV << ",  receiveState: ";
    switch (receiveState) {
        CASE(RX_IDLE_STATE);
        CASE(RECEIVING_STATE);
        CASE(RX_COLLISION_STATE);
    }
    EV << ",  backoffs: " << backoffs;
    EV << ",  numConcurrentTransmissions: " << numConcurrentTransmissions;
    EV << ",  queueLength: " << txQueue.length() << endl;
#undef CASE
}

void EtherMAC::finish()
{
    EtherMACBase::finish();

    simtime_t t = simTime();
    simtime_t totalChannelIdleTime = t - totalSuccessfulRxTxTime - totalCollisionTime;
    recordScalar("rx channel idle (%)", 100*(totalChannelIdleTime/t));
    recordScalar("rx channel utilization (%)", 100*(totalSuccessfulRxTxTime/t));
    recordScalar("rx channel collision (%)", 100*(totalCollisionTime/t));
    recordScalar("collisions",     numCollisions);
    recordScalar("backoffs",       numBackoffs);
}

void EtherMAC::updateHasSubcribers()
{
    hasSubscribers = false;  // currently we don't fire any notifications
}

