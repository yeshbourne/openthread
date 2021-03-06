/*
 *    Copyright (c) 2016, Nest Labs, Inc.
 *    All rights reserved.
 *
 *    Redistribution and use in source and binary forms, with or without
 *    modification, are permitted provided that the following conditions are met:
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *    3. Neither the name of the copyright holder nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 *    ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 *    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
 *    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 *    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 *   This file implements a SPI interface to the OpenThread stack.
 */

#include <common/code_utils.hpp>
#include <common/new.hpp>
#include <ncp/ncp.h>
#include <ncp/ncp_spi.hpp>
#include <platform/spi-slave.h>
#include <core/openthread-core-config.h>

#define SPI_RESET_FLAG          0x80

namespace Thread {

static otDEFINE_ALIGNED_VAR(sNcpRaw, sizeof(NcpSpi), uint64_t);
static NcpSpi *sNcpSpi;

extern "C" void otNcpInit(void)
{
    sNcpSpi = new(&sNcpRaw) NcpSpi;
}

static void spi_header_set_flag_byte(uint8_t *header, uint8_t value)
{
    header[0] = value;
}

static void spi_header_set_accept_len(uint8_t *header, uint16_t len)
{
    header[1] = ((len >> 0) & 0xFF);
    header[2] = ((len >> 8) & 0xFF);
}

static void spi_header_set_data_len(uint8_t *header, uint16_t len)
{
    header[3] = ((len >> 0) & 0xFF);
    header[4] = ((len >> 8) & 0xFF);
}

static uint8_t spi_header_get_flag_byte(const uint8_t *header)
{
    return header[0];
}

static uint16_t spi_header_get_accept_len(const uint8_t *header)
{
    return ( header[1] + (header[2] << 8) );
}

static uint16_t spi_header_get_data_len(const uint8_t *header)
{
    return ( header[3] + (header[4] << 8) );
}

NcpSpi::NcpSpi():
    NcpBase(),
    mHandleRxFrame(&HandleRxFrame, this),
    mHandleSendDone(&HandleSendDone, this)
{
    memset(mEmptySendFrame, 0, sizeof(SPI_HEADER_LENGTH));
    memset(mSendFrame, 0, sizeof(SPI_HEADER_LENGTH));

    mSending = false;

    spi_header_set_flag_byte(mSendFrame, SPI_RESET_FLAG);
    spi_header_set_flag_byte(mEmptySendFrame, SPI_RESET_FLAG);
    spi_header_set_accept_len(mSendFrame, sizeof(mReceiveFrame) - SPI_HEADER_LENGTH);
    otPlatSpiSlaveEnable(&SpiTransactionComplete, (void*)this);

    // We signal an interrupt on this first transaction to
    // make sure that the host processor knows that our
    // reset flag was set.
    otPlatSpiSlavePrepareTransaction(
        mEmptySendFrame,
        SPI_HEADER_LENGTH,
        mEmptyReceiveFrame,
        SPI_HEADER_LENGTH,
        true
    );
}

void
NcpSpi::SpiTransactionComplete(
    void *aContext,
    uint8_t *anOutputBuf,
    uint16_t anOutputBufLen,
    uint8_t *anInputBuf,
    uint16_t anInputBufLen,
    uint16_t aTransactionLength
)
{
    static_cast<NcpSpi*>(aContext)->SpiTransactionComplete(
        anOutputBuf,
        anOutputBufLen,
        anInputBuf,
        anInputBufLen,
        aTransactionLength
    );
}

void
NcpSpi::SpiTransactionComplete(
    uint8_t *aMISOBuf,
    uint16_t aMISOBufLen,
    uint8_t *aMOSIBuf,
    uint16_t aMOSIBufLen,
    uint16_t aTransactionLength
) {
    // This may be executed from an interrupt context.
    // Must return as quickly as possible.

    uint16_t rx_data_len(0);
    uint16_t rx_accept_len(0);
    uint16_t tx_data_len(0);
    uint16_t tx_accept_len(0);

    if (aTransactionLength >= SPI_HEADER_LENGTH)
    {
        if (aMISOBufLen >= SPI_HEADER_LENGTH)
        {
            rx_accept_len = spi_header_get_accept_len(aMISOBuf);
            tx_data_len = spi_header_get_data_len(aMISOBuf);
        }

        if (aMOSIBufLen >= SPI_HEADER_LENGTH)
        {
            rx_data_len = spi_header_get_data_len(aMOSIBuf);
            tx_accept_len = spi_header_get_accept_len(aMOSIBuf);
        }

        if ( !mHandlingRxFrame
          && (rx_data_len > 0)
          && (rx_data_len <= (aTransactionLength - SPI_HEADER_LENGTH))
          && (rx_data_len <= rx_accept_len)
        ) {
            mHandlingRxFrame = true;
            mHandleRxFrame.Post();
        }

        if ( mSending
          && !mHandlingSendDone
          && (tx_data_len > 0)
          && (tx_data_len <= (aTransactionLength - SPI_HEADER_LENGTH))
          && (tx_data_len <= tx_accept_len)
        ) {
            // Our transmission was successful.
            mHandlingSendDone = true;
            mHandleSendDone.Post();
        }
    }

    if ( (aTransactionLength >= 1)
      && (aMISOBufLen >= 1)
    ) {
        // Clear the reset flag
        spi_header_set_flag_byte(mSendFrame, 0);
        spi_header_set_flag_byte(mEmptySendFrame, 0);
    }

    if (mSending && !mHandlingSendDone)
    {
        aMISOBuf = mSendFrame;
        aMISOBufLen = OutboundFrameSize() + SPI_HEADER_LENGTH;
    }
    else
    {
        aMISOBuf = mEmptySendFrame;
        aMISOBufLen = SPI_HEADER_LENGTH;
    }

    if (mHandlingRxFrame)
    {
        aMOSIBuf = mEmptyReceiveFrame;
        aMOSIBufLen = SPI_HEADER_LENGTH;
        spi_header_set_accept_len(aMISOBuf, 0);
    }
    else
    {
        aMOSIBuf = mReceiveFrame;
        aMOSIBufLen = sizeof(mReceiveFrame);
        spi_header_set_accept_len(aMISOBuf, sizeof(mReceiveFrame) - SPI_HEADER_LENGTH);
    }

    otPlatSpiSlavePrepareTransaction(
        aMISOBuf,
        aMISOBufLen,
        aMOSIBuf,
        aMOSIBufLen,
        mSending && !mHandlingSendDone
    );
}

uint16_t
NcpSpi::OutboundFrameSize(void)
{
    return static_cast<uint16_t>(mSendFrameIter - (mSendFrame + SPI_HEADER_LENGTH));
}

uint16_t
NcpSpi::OutboundFrameGetRemaining(void)
{
    return static_cast<uint16_t>((sizeof(mSendFrame) - SPI_HEADER_LENGTH) - OutboundFrameSize());
}

ThreadError
NcpSpi::OutboundFrameBegin(void)
{
    ThreadError errorCode( kThreadError_None );

    if (mSending)
    {
        errorCode = kThreadError_Busy;
    }
    else
    {
        mSendFrameIter = (mSendFrame + SPI_HEADER_LENGTH);
    }

    return errorCode;
}

ThreadError
NcpSpi::OutboundFrameFeedData(const uint8_t *frame, uint16_t frameLength)
{
    ThreadError errorCode( kThreadError_None );
    uint16_t maxOutLength( OutboundFrameGetRemaining() );

    if (frameLength > maxOutLength)
    {
        errorCode = kThreadError_Failed;
    }
    else
    {
        memcpy(mSendFrameIter, frame, frameLength);
        mSendFrameIter += frameLength;
    }

    return errorCode;
}

ThreadError
NcpSpi::OutboundFrameFeedMessage(Message &message)
{
    ThreadError errorCode( kThreadError_None );
    uint16_t maxOutLength( OutboundFrameGetRemaining() );
    uint16_t frameLength( message.GetLength() );

    if (frameLength > maxOutLength)
    {
        errorCode = kThreadError_Failed;
    }
    else
    {
        message.Read(0, frameLength, mSendFrameIter);
        mSendFrameIter += frameLength;
    }

    return errorCode;
}

ThreadError
NcpSpi::OutboundFrameSend(void)
{
    ThreadError errorCode;
    uint16_t frameLength( OutboundFrameSize() );

    spi_header_set_data_len(mSendFrame, frameLength);

    // Half-duplex to avoid race condition.
    spi_header_set_accept_len(mSendFrame, 0);

    mSending = true;

    errorCode = otPlatSpiSlavePrepareTransaction(
        mSendFrame,
        frameLength + SPI_HEADER_LENGTH,
        mEmptyReceiveFrame,
        sizeof(mEmptyReceiveFrame),
        true
    );

    if (errorCode == kThreadError_Busy)
    {
        // Being busy is OK. We will get the transaction
        // set up properly when the current transaction
        // is completed.
        errorCode = kThreadError_None;
    }

    return errorCode;
}

void NcpSpi::HandleSendDone(void *context)
{
    static_cast<NcpSpi*>(context)->HandleSendDone();
}

void NcpSpi::HandleSendDone(void)
{
    mSending = false;
    mHandlingSendDone = false;

    super_t::HandleSpaceAvailableInTxBuffer();
}

void NcpSpi::HandleRxFrame(void *context)
{
    static_cast<NcpSpi*>(context)->HandleRxFrame();
}

void NcpSpi::HandleRxFrame(void)
{
    uint16_t rx_data_len( spi_header_get_data_len(mReceiveFrame) );
    super_t::HandleReceive(mReceiveFrame + SPI_HEADER_LENGTH, rx_data_len);
    mHandlingRxFrame = false;
}


}  // namespace Thread

