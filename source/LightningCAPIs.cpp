// Copyright (c) Microsoft Open Technologies, Inc.  All rights reserved.  
// Licensed under the BSD 2-Clause License.  
// See License.txt in the project root for license information.

#include "pch.h"

#include "LightningCAPIs.h"
#include "BtI2cController.h"
#include <memory>
#include <vector>

HRESULT __cdecl MBMVerifyPinFunction(unsigned int mappedPin, unsigned int function, unsigned int lockAction)
{
    auto res = g_pins.verifyPinFunction(mappedPin, function, static_cast<BoardPinsClass::FUNC_LOCK_ACTION>(lockAction));
    return res;
}

HRESULT __cdecl MBMSetPinMode(unsigned int mappedPin, unsigned int mode, bool pullUp)
{
    auto res = g_pins.setPinMode(mappedPin, mode, pullUp);
    return res;
}

HRESULT __cdecl MBMSetPinState(unsigned int mappedPin, unsigned int state)
{
    auto res = g_pins.setPinState(mappedPin, state);
    return res;
}

HRESULT __cdecl MBMGetPinState(unsigned int mappedPin, unsigned int *state)
{
    ULONG value = 0;
    auto res = g_pins.getPinState(mappedPin, value);
    *state = value;
    return res;
}

struct I2cState
{
    std::unique_ptr<BtI2cControllerClass> controller;
    std::unique_ptr< I2cTransactionClass> transaction;

    bool is_empty() const
    {
        return !controller;
    }

    void clear()
    {
        controller.reset(nullptr);
        transaction.reset(nullptr);
    }
};

std::vector<I2cState> i2cStates;

HRESULT __cdecl MBMI2cInit(unsigned int busNumber, int slaveAddress, bool fastMode, int *handle)
{
    *handle = -1;

    BoardPinsClass::BOARD_TYPE board;
    HRESULT hr = g_pins.getBoardType(board);

    if (FAILED(hr)) { return hr; }
    if (board != BoardPinsClass::BOARD_TYPE::MBM_BARE) { return E_NOTIMPL; }

    I2cState i2cState;

    i2cState.controller.reset(new BtI2cControllerClass());
    hr = i2cState.controller->configurePins(BARE_MBM_PIN_I2C_DAT, BARE_MBM_PIN_I2C_CLK);
    if (FAILED(hr)) { return hr; }

    hr = i2cState.controller->begin(busNumber);
    if (FAILED(hr)) { return hr; }

    i2cState.transaction.reset(new I2cTransactionClass());
    hr = i2cState.transaction->setAddress(slaveAddress);
    if (FAILED(hr)) { return hr; }

    if (fastMode)
    {
        i2cState.transaction->useHighSpeed();
    }

    // find available handle
    int i = 0;
    while (i < i2cStates.size() && !i2cStates[i].is_empty())
    {
        ++i;
    }
    if (i < i2cStates.size())
    {
        i2cStates[i] = std::move(i2cState);
    }
    else
    {
        i2cStates.push_back(std::move(i2cState));
    }
    *handle = i;

    return S_OK;
}

HRESULT __cdecl MBMI2cClose(int handle)
{
    if (handle < 0 || handle >= i2cStates.size())
    {
        // we assume the handle was released already...
        return S_OK;
    }
    auto &i2c = i2cStates[handle];

    if (i2c.controller)
    {
        i2c.controller->end();
    }
    i2c.clear();

    return S_OK;
}

HRESULT __cdecl MBMI2cWrite(int handle, unsigned char *buffer, int length)
{
    if (handle < 0 || handle >= i2cStates.size())
    {
        return E_HANDLE;
    }
    auto &i2c = i2cStates[handle];

    i2c.transaction->reset();

    HRESULT hr = i2c.transaction->queueWrite(buffer, length);
    if (FAILED(hr))
    {
        // REVIEW: should we clean out the transaction?
        // original comment from Lightning provider:
        //      Clean out the transaction so it can be used again in the future.
        return hr;
    }

    hr = i2c.transaction->execute(i2c.controller.get());
    if (FAILED(hr)) { return hr; }
    if (i2c.transaction->isIncomplete())
    {
        return E_ABORT;
    }

    return S_OK;
}

enum TransferStatus: unsigned int {
    FullTransfer = 0,
    PartialTransfer = 1,
    SlaveAddressNotAcknowledged = 2
};

HRESULT __cdecl MBMI2cWritePartial(int handle, unsigned char *buffer, int length, unsigned int *bytesTransferred, unsigned int *status)
{
    if (handle < 0 || handle >= i2cStates.size())
    {
        return E_HANDLE;
    }
    auto &i2c = i2cStates[handle];

    *bytesTransferred = 0;
    *status = TransferStatus::PartialTransfer;

    i2c.transaction->reset();

    HRESULT hr = i2c.transaction->queueWrite(buffer, length);
    if (FAILED(hr)) { return hr; }

    hr = i2c.transaction->execute(i2c.controller.get());

    if (SUCCEEDED(hr) && !i2c.transaction->isIncomplete())
    {
        *status = TransferStatus::FullTransfer;
        *bytesTransferred = length;
    }
    else if (FAILED(hr))
    {
        if (i2c.transaction->getError() == I2cTransactionClass::ERROR_CODE::ADR_NACK)
        {
            *status = TransferStatus::SlaveAddressNotAcknowledged;
        }
        else if (i2c.transaction->getError() == I2cTransactionClass::ERROR_CODE::DATA_NACK)
        {
            *status = TransferStatus::PartialTransfer;
        }
        else
        {
            // REVIEW (alecont): looks like we're 'eating' a possible HR failure code...
            *status = TransferStatus::PartialTransfer;
        }
    }
    else if (i2c.transaction->isIncomplete())
    {
        *status = TransferStatus::PartialTransfer;
    }

    return S_OK;
}

HRESULT __cdecl MBMI2cRead(int handle, unsigned char *buffer, int length)
{
    if (handle < 0 || handle >= i2cStates.size())
    {
        return E_HANDLE;
    }
    auto &i2c = i2cStates[handle];

    i2c.transaction->reset();

    HRESULT hr = i2c.transaction->queueRead(buffer, length);
    if (FAILED(hr)) { return hr; }

    hr = i2c.transaction->execute(i2c.controller.get());
    if (FAILED(hr)) { return hr; }
    if (i2c.transaction->isIncomplete())
    {
        return E_ABORT;
    }

    return S_OK;
}

HRESULT __cdecl MBMI2cReadPartial(int handle, unsigned char *buffer, int length, unsigned int *bytesTransferred, unsigned int *status)
{
    if (handle < 0 || handle >= i2cStates.size())
    {
        return E_HANDLE;
    }
    auto &i2c = i2cStates[handle];

    *bytesTransferred = 0;
    *status = TransferStatus::PartialTransfer;

    i2c.transaction->reset();

    HRESULT hr = i2c.transaction->queueRead(buffer, length);
    if (FAILED(hr)) { return hr; }

    hr = i2c.transaction->execute(i2c.controller.get());

    if (SUCCEEDED(hr) && !i2c.transaction->isIncomplete())
    {
        *status = TransferStatus::FullTransfer;
        *bytesTransferred = length;
    }
    else if (FAILED(hr))
    {
        if (i2c.transaction->getError() == I2cTransactionClass::ERROR_CODE::ADR_NACK)
        {
            *status = TransferStatus::SlaveAddressNotAcknowledged;
        }
        else if (i2c.transaction->getError() == I2cTransactionClass::ERROR_CODE::DATA_NACK)
        {
            *status = TransferStatus::PartialTransfer;
        }
        else
        {
            // REVIEW (alecont): looks like we're 'eating' a possible HR failure code...
            *status = TransferStatus::PartialTransfer;
        }
    }
    else if (i2c.transaction->isIncomplete())
    {
        *status = TransferStatus::PartialTransfer;
    }

    return S_OK;
}

HRESULT __cdecl MBMI2cWriteRead(int handle, unsigned char *writeBuffer, int writeBufferLength, unsigned char *readBuffer, int readBufferLength)
{
    if (handle < 0 || handle >= i2cStates.size())
    {
        return E_HANDLE;
    }
    auto &i2c = i2cStates[handle];

    i2c.transaction->reset();

    HRESULT hr = i2c.transaction->queueWrite(writeBuffer, writeBufferLength);
    if (FAILED(hr)) { return hr; }

    hr = i2c.transaction->queueRead(readBuffer, readBufferLength);
    if (FAILED(hr)) { return hr; }

    hr = i2c.transaction->execute(i2c.controller.get());
    if (FAILED(hr)) { return hr; }
    if (i2c.transaction->isIncomplete())
    {
        return E_ABORT;
    }

    return S_OK;
}

HRESULT __cdecl MBMI2cWriteReadPartial(int handle, unsigned char *writeBuffer, int writeBufferLength, unsigned char *readBuffer, int readBufferLength,
    unsigned int *bytesTransferred, unsigned int *status)
{
    if (handle < 0 || handle >= i2cStates.size())
    {
        return E_HANDLE;
    }
    auto &i2c = i2cStates[handle];

    *bytesTransferred = 0;
    *status = TransferStatus::PartialTransfer;

    i2c.transaction->reset();

    HRESULT hr = i2c.transaction->queueWrite(writeBuffer, writeBufferLength);
    if (FAILED(hr)) { return hr; }

    hr = i2c.transaction->queueRead(readBuffer, readBufferLength);
    if (FAILED(hr)) { return hr; }

    hr = i2c.transaction->execute(i2c.controller.get());

    if (SUCCEEDED(hr) && !i2c.transaction->isIncomplete())
    {
        *status = TransferStatus::FullTransfer;
        *bytesTransferred = writeBufferLength + readBufferLength;
    }
    else if (FAILED(hr))
    {
        if (i2c.transaction->getError() == I2cTransactionClass::ERROR_CODE::ADR_NACK)
        {
            *status = TransferStatus::SlaveAddressNotAcknowledged;
        }
        else if (i2c.transaction->getError() == I2cTransactionClass::ERROR_CODE::DATA_NACK)
        {
            *status = TransferStatus::PartialTransfer;
        }
        else
        {
            // REVIEW (alecont): looks like we're 'eating' a possible HR failure code...
            *status = TransferStatus::PartialTransfer;
        }
    }
    else if (i2c.transaction->isIncomplete())
    {
        *status = TransferStatus::PartialTransfer;
    }

    return S_OK;
}
