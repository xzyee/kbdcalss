/*++

Copyright (c) 1990, 1991, 1992, 1993, 1994 - 1998  Microsoft Corporation

Module Name:

    kbdclass.c

Abstract:

    Keyboard class driver.

Environment:

    Kernel mode only.

Notes:

    NOTES:  (Future/outstanding issues)

    - Consolidate common code into a function, where appropriate.

--*/

#include <stdarg.h>
#include <stdio.h>
#include <ntddk.h>
#include <hidclass.h>

#include <initguid.h>
#include <kbdmou.h> //#define DD_KEYBOARD_CLASS_BASE_NAME_U   L"KeyboardClass" 
#include <kbdlog.h>
#include "kbdclass.h"
#include <poclass.h>
#include <wmistr.h>

#define INITGUID
#include "wdmguid.h"

GLOBALS Globals;


//
// Use the alloc_text pragma to specify the driver initialization routines
// (they can be paged out).
//

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT,DriverEntry)
#pragma alloc_text(INIT,KbdConfiguration)
#pragma alloc_text(PAGE,KeyboardClassPassThrough)
#pragma alloc_text(PAGE,KeyboardQueryDeviceKey)
#pragma alloc_text(PAGE,KbdDeterminePortsServiced)
#pragma alloc_text(PAGE,KbdDeviceMapQueryCallback)
#pragma alloc_text(PAGE,KbdSendConnectRequest)
#pragma alloc_text(PAGE,KeyboardAddDevice)
#pragma alloc_text(PAGE,KeyboardAddDeviceEx)
#pragma alloc_text(PAGE,KeyboardClassDeviceControl)
#pragma alloc_text(PAGE,KeyboardSendIrpSynchronously)
#pragma alloc_text(PAGE,KbdCreateClassObject)
#pragma alloc_text(PAGE,KeyboardClassFindMorePorts)
#pragma alloc_text(PAGE,KeyboardClassGetWaitWakeEnableState)
#pragma alloc_text(PAGE,KeyboardClassEnableGlobalPort)
#pragma alloc_text(PAGE,KeyboardClassPlugPlayNotification)
#pragma alloc_text(PAGE,KeyboardClassSystemControl)
#pragma alloc_text(PAGE,KeyboardClassSetWmiDataItem)
#pragma alloc_text(PAGE,KeyboardClassSetWmiDataBlock)
#pragma alloc_text(PAGE,KeyboardClassQueryWmiDataBlock)
#pragma alloc_text(PAGE,KeyboardClassQueryWmiRegInfo)

#pragma alloc_text(PAGE,KeyboardClassPower)
#pragma alloc_text(PAGE,KeyboardClassCreateWaitWakeIrpWorker)
#pragma alloc_text(PAGE,KeyboardClassCreateWaitWakeIrp)
// #pragma alloc_text(PAGE,KeyboardToggleWaitWakeWorker)
#pragma alloc_text(PAGE,KeyboardClassUnload)
#endif

#define WMI_CLASS_DRIVER_INFORMATION 0
#define WMI_WAIT_WAKE                1

GUID KeyboardClassGuid =  MSKeyboard_ClassInformationGuid;

WMIGUIDREGINFO KeyboardClassWmiGuidList[] =
{
    {
        &KeyboardClassGuid,
        1,
        0 // Keyboard class driver information
    },
    {
        &GUID_POWER_DEVICE_WAKE_ENABLE,
        1,
        0 // wait wake
    }
};



#if DBG
VOID
KbdDebugPrint(
    ULONG DebugPrintLevel,
    PCCHAR DebugMessage,
    ...
    )

/*++

Routine Description:

    Debug print routine.

Arguments:

    Debug print level between 0 and 3, with 3 being the most verbose啰唆的.

Return Value:

    None.

--*/

{
    va_list ap;

    va_start(ap, DebugMessage);

    if (DebugPrintLevel <= Globals.Debug) {

        char buffer[256];

        (VOID) vsprintf(buffer, DebugMessage, ap);

        DbgPrint(buffer);
    }

    va_end(ap);

}
#endif







VOID
KeyboardClassPoRequestComplete (
    IN PDEVICE_OBJECT DeviceObject,
    IN UCHAR MinorFunction,
    IN POWER_STATE PowerState,
    IN PVOID Context,
    IN PIO_STATUS_BLOCK IoStatus
    );

NTSTATUS
KeyboardClassPowerComplete (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context
    );









VOID
KeyboardToggleWaitWakeWorker(
    IN PDEVICE_OBJECT DeviceObject,
    PKEYBOARD_WORK_ITEM_DATA ItemData
    )
/*++

Routine Description:

--*/
{
    PDEVICE_EXTENSION   data;
    PIRP                waitWakeIrp = NULL;
    KIRQL               irql;
    BOOLEAN             wwState = ItemData->WaitWakeState ? TRUE : FALSE;
    BOOLEAN             toggled = FALSE;

    //
    // Can't be paged b/c we are using spin locks
    //
    // PAGED_CODE ();

    data = ItemData->Data;

    KeAcquireSpinLock (&data->WaitWakeSpinLock, &irql);

    if (wwState != data->WaitWakeEnabled) {
        toggled = TRUE;
        if (data->WaitWakeEnabled) {
            waitWakeIrp = (PIRP)
                InterlockedExchangePointer (&data->WaitWakeIrp, NULL);
        }

        data->WaitWakeEnabled = wwState;
    }

    KeReleaseSpinLock (&data->WaitWakeSpinLock, irql);

    if (toggled) {
        UNICODE_STRING strEnable;
        HANDLE         devInstRegKey;
        ULONG          tmp = wwState;

        //
        // write the value out to the registry
        //
        if ((NT_SUCCESS(IoOpenDeviceRegistryKey (data->PDO,
                                                 PLUGPLAY_REGKEY_DEVICE,
                                                 STANDARD_RIGHTS_ALL,
                                                 &devInstRegKey)))) {
            RtlInitUnicodeString (&strEnable, KEYBOARD_WAIT_WAKE_ENABLE);

            ZwSetValueKey (devInstRegKey,
                           &strEnable,
                           0,
                           REG_DWORD,
                           &tmp,
                           sizeof(tmp));

            ZwClose (devInstRegKey);
        }
    }

    if (toggled && wwState) {
        //
        // wwState is our new state, so WW was just turned on
        //
        KeyboardClassCreateWaitWakeIrp (data);
    }

    //
    // If we have an IRP, then WW has been toggled off, otherwise, if toggled is
    // TRUE, we need to save this in the reg and, perhaps, send down a new WW irp
    //
    if (waitWakeIrp) {
        IoCancelIrp (waitWakeIrp);
    }

    IoReleaseRemoveLock (&data->RemoveLock, KeyboardToggleWaitWakeWorker);
    IoFreeWorkItem (ItemData->Item);
    ExFreePool (ItemData);
}

NTSTATUS
KeyboardToggleWaitWake(
    PDEVICE_EXTENSION Data,
    BOOLEAN           WaitWakeState
    )
{
    NTSTATUS       status;
    PKEYBOARD_WORK_ITEM_DATA itemData;

    status = IoAcquireRemoveLock (&Data->RemoveLock, KeyboardToggleWaitWakeWorker);
    if (!NT_SUCCESS (status)) {
        //
        // Device has gone away, just silently exit
        //
        return status;
    }

    itemData = (PKEYBOARD_WORK_ITEM_DATA)
        ExAllocatePool(NonPagedPool, sizeof(KEYBOARD_WORK_ITEM_DATA));
    if (itemData) {
        itemData->Item = IoAllocateWorkItem(Data->Self);
        if (itemData->Item == NULL) {
            IoReleaseRemoveLock (&Data->RemoveLock, KeyboardToggleWaitWakeWorker);
        }
        else {
            itemData->Data = Data;
            itemData->WaitWakeState = WaitWakeState;

            if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
                //
                // We are safely at PASSIVE_LEVEL, call callback directly to perform
                // this operation immediately.
                //
				//好机会，直接执行好了，不用worker了
                KeyboardToggleWaitWakeWorker (Data->Self, itemData);

            } else {
                //
                // We are not at PASSIVE_LEVEL, so queue a workitem to handle this
                // at a later time.
                // 只能用worker了
                IoQueueWorkItem (itemData->Item,
                                 KeyboardToggleWaitWakeWorker,//工作函数
                                 DelayedWorkQueue,//普通线程，优先级=12
                                 itemData);//工作函数的参数
            }
        }
    }
    else {
        IoReleaseRemoveLock (&Data->RemoveLock, KeyboardToggleWaitWakeWorker);
    }

    return STATUS_SUCCESS;
}





NTSTATUS
KeyboardClassSetWmiDataItem(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN ULONG GuidIndex,
    IN ULONG InstanceIndex,
    IN ULONG DataItemId,
    IN ULONG BufferSize,
    IN PUCHAR Buffer
    )
/*++

Routine Description:

    This routine is a callback into the driver to set for the contents of
    a data block. When the driver has finished filling the data block it
    must call ClassWmiCompleteRequest to complete the irp. The driver can
    return STATUS_PENDING if the irp cannot be completed immediately.

Arguments:

    DeviceObject is the device whose data block is being queried

    Irp is the Irp that makes this request

    GuidIndex is the index into the list of guids provided when the
        device registered

    InstanceIndex is the index that denotes which instance of the data block
        is being queried.

    DataItemId has the id of the data item being set

    BufferSize has the size of the data item passed

    Buffer has the new values for the data item


Return Value:

    status

--*/
{
    PDEVICE_EXTENSION   data;
    NTSTATUS            status;
    ULONG               size = 0;

    PAGED_CODE ();

    data = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

    switch(GuidIndex) {
    case WMI_CLASS_DRIVER_INFORMATION:
        status = STATUS_WMI_READ_ONLY;
        break;

    case WMI_WAIT_WAKE:

        size = sizeof(BOOLEAN);

        if (BufferSize < size) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        } else if ((1 != DataItemId) || (0 != InstanceIndex)) {
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }

        status = KeyboardToggleWaitWake (data, *(PBOOLEAN) Buffer);
        break;

    default:
        status = STATUS_WMI_GUID_NOT_FOUND;
    }

    status = WmiCompleteRequest (DeviceObject,
                                 Irp,
                                 status,
                                 size,
                                 IO_NO_INCREMENT);

    return status;
}

NTSTATUS
KeyboardClassSetWmiDataBlock(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN ULONG GuidIndex,
    IN ULONG InstanceIndex,
    IN ULONG BufferSize,
    IN PUCHAR Buffer
    )
/*++

Routine Description:

    This routine is a callback into the driver to set the contents of
    a data block. When the driver has finished filling the data block it
    must call ClassWmiCompleteRequest to complete the irp. The driver can
    return STATUS_PENDING if the irp cannot be completed immediately.

Arguments:

    DeviceObject is the device whose data block is being queried

    Irp is the Irp that makes this request

    GuidIndex is the index into the list of guids provided when the
        device registered

    InstanceIndex is the index that denotes which instance of the data block
        is being queried.

    BufferSize has the size of the data block passed

    Buffer has the new values for the data block


Return Value:

    status

--*/
{
    PDEVICE_EXTENSION data;
    NTSTATUS          status;
    ULONG             size = 0;

    PAGED_CODE ();

    data = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

    switch(GuidIndex) {
    case WMI_CLASS_DRIVER_INFORMATION:
        status = STATUS_WMI_READ_ONLY;
        break;

    case WMI_WAIT_WAKE: {
        size = sizeof(BOOLEAN);

        if (BufferSize < size) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        } else if (0 != InstanceIndex) {
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }

        status = KeyboardToggleWaitWake (data, * (PBOOLEAN) Buffer);
        break;
    }

    default:
        status = STATUS_WMI_GUID_NOT_FOUND;
    }

    status = WmiCompleteRequest (DeviceObject,
                                 Irp,
                                 status,
                                 size,
                                 IO_NO_INCREMENT);

    return status;
}

NTSTATUS
KeyboardClassQueryWmiDataBlock(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN ULONG GuidIndex,
    IN ULONG InstanceIndex,
    IN ULONG InstanceCount,
    IN OUT PULONG InstanceLengthArray,
    IN ULONG OutBufferSize,
    OUT PUCHAR Buffer
    )
/*++

Routine Description:

    This routine is a callback into the driver to query for the contents of
    a data block. When the driver has finished filling the data block it
    must call ClassWmiCompleteRequest to complete the irp. The driver can
    return STATUS_PENDING if the irp cannot be completed immediately.

Arguments:

    DeviceObject is the device whose data block is being queried

    Irp is the Irp that makes this request

    GuidIndex is the index into the list of guids provided when the
        device registered

    InstanceIndex is the index that denotes which instance of the data block
        is being queried.

    InstanceCount is the number of instnaces expected to be returned for
        the data block.

    InstanceLengthArray is a pointer to an array of ULONG that returns the
        lengths of each instance of the data block. If this is NULL then
        there was not enough space in the output buffer to fufill the request
        so the irp should be completed with the buffer needed.

    BufferAvail on has the maximum size available to write the data
        block.

    Buffer on return is filled with the returned data block


Return Value:

    status

--*/
{
    PDEVICE_EXTENSION   data;
    NTSTATUS    status;
    ULONG       size = 0;
    PMSKeyboard_ClassInformation classInformation;

    PAGED_CODE ();

    data = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

    switch (GuidIndex) {
    case WMI_CLASS_DRIVER_INFORMATION:
        //
        // Only registers 1 instance for this guid
        //
        if ((0 != InstanceIndex) || (1 != InstanceCount)) {
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }
        size = sizeof (MSKeyboard_ClassInformation);

        if (OutBufferSize < size) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        classInformation = (PMSKeyboard_ClassInformation)Buffer;
        classInformation->DeviceId = (ULONGLONG) DeviceObject;
        *InstanceLengthArray = size;
        status = STATUS_SUCCESS;

        break;

    case WMI_WAIT_WAKE:
        //
        // Only registers 1 instance for this guid
        //
        if ((0 != InstanceIndex) || (1 != InstanceCount)) {
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }
        size = sizeof(BOOLEAN);

        if (OutBufferSize < size) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        *(PBOOLEAN) Buffer = (WAITWAKE_ON (data) ? TRUE : FALSE );
        *InstanceLengthArray = size;
        status = STATUS_SUCCESS;
        break;

    default:
        status = STATUS_WMI_GUID_NOT_FOUND;
    }

    status = WmiCompleteRequest (DeviceObject,
                                 Irp,
                                 status,
                                 size,
                                 IO_NO_INCREMENT);

    return status;
}

NTSTATUS
KeyboardClassQueryWmiRegInfo(
    IN PDEVICE_OBJECT DeviceObject,
    OUT ULONG *RegFlags,
    OUT PUNICODE_STRING InstanceName,
    OUT PUNICODE_STRING *RegistryPath,
    OUT PUNICODE_STRING MofResourceName,
    OUT PDEVICE_OBJECT  *Pdo
    )
/*++

Routine Description:

    This routine is a callback into the driver to retrieve information about
    the guids being registered.

    Implementations of this routine may be in paged memory

Arguments:

    DeviceObject is the device whose registration information is needed

    *RegFlags returns with a set of flags that describe all of the guids being
        registered for this device. If the device wants enable and disable
        collection callbacks before receiving queries for the registered
        guids then it should return the WMIREG_FLAG_EXPENSIVE flag. Also the
        returned flags may specify WMIREG_FLAG_INSTANCE_PDO in which case
        the instance name is determined from the PDO associated with the
        device object. Note that the PDO must have an associated devnode. If
        WMIREG_FLAG_INSTANCE_PDO is not set then Name must return a unique
        name for the device. These flags are ORed into the flags specified
        by the GUIDREGINFO for each guid.

    InstanceName returns with the instance name for the guids if
        WMIREG_FLAG_INSTANCE_PDO is not set in the returned *RegFlags. The
        caller will call ExFreePool with the buffer returned.

    *RegistryPath returns with the registry path of the driver. This is
        required

    *MofResourceName returns with the name of the MOF resource attached to
        the binary file. If the driver does not have a mof resource attached
        then this can be returned as NULL.

    *Pdo returns with the device object for the PDO associated with this
        device if the WMIREG_FLAG_INSTANCE_PDO flag is retured in
        *RegFlags.

Return Value:

    status

--*/
{
    PDEVICE_EXTENSION deviceExtension;

    PAGED_CODE ();

    deviceExtension = DeviceObject->DeviceExtension;

    *RegFlags = WMIREG_FLAG_INSTANCE_PDO;
    *RegistryPath = &Globals.RegistryPath;
    *Pdo = deviceExtension->PDO;
    return STATUS_SUCCESS;
}


