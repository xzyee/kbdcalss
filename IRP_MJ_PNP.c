NTSTATUS
KeyboardPnP (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
/*++

Routine Description:

    The plug and play dispatch routines.

    Most of these this filter driver will completely ignore.
    In all cases it must pass on the IRP to the lower driver.

Arguments:

   DeviceObject - pointer to a device object.

   Irp - pointer to an I/O Request Packet.

Return Value:

      NT status code

--*/
{
    PDEVICE_EXTENSION   data;
    PDEVICE_EXTENSION   trueClassData;
    PIO_STACK_LOCATION  stack;
    NTSTATUS            status, startStatus;
    ULONG               i;
    PFILE_OBJECT      * file;
    UINT_PTR            startInformation;
    DEVICE_CAPABILITIES devCaps;
    BOOLEAN             enabled;
    PPORT               port;
    PVOID               notifyHandle;
    KIRQL               oldIrql;
    KIRQL               cancelIrql;

    data = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;
    stack = IoGetCurrentIrpStackLocation (Irp);

    if(!data->PnP) {
        //
        // This irp was sent to the control device object, which knows not
        // how to deal with this IRP.  It is therefore an error.
        //
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        IoCompleteRequest (Irp, IO_NO_INCREMENT);
        return STATUS_NOT_SUPPORTED;

    }

    status = IoAcquireRemoveLock (&data->RemoveLock, Irp);
    if (!NT_SUCCESS (status)) {
        //
        // Someone gave us a pnp irp after a remove.  Unthinkable!
        //
        ASSERT (FALSE);
        Irp->IoStatus.Information = 0;
        Irp->IoStatus.Status = status;
        IoCompleteRequest (Irp, IO_NO_INCREMENT);
        return status;
    }

	//从伪设备扩展找到真设备扩展
    trueClassData = (PDEVICE_EXTENSION) data->TrueClassDevice->DeviceExtension;
    switch (stack->MinorFunction) {
    case IRP_MN_START_DEVICE:

        //
        // The device is starting.
        //
        // We cannot touch the device (send it any non pnp irps) until a
        // start device has been passed down to the lower drivers.
        //
        status = KeyboardSendIrpSynchronously (data->TopPort, Irp, TRUE);

        if (NT_SUCCESS (status) && NT_SUCCESS (Irp->IoStatus.Status)) {
            //
            // As we are successfully now back from our start device
            // we can do work.
            //
            // Get the caps of the device.  Save off pertinent information
            // before mucking about w/the irp
            //
            startStatus = Irp->IoStatus.Status;
            startInformation = Irp->IoStatus.Information;

            Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
            Irp->IoStatus.Information = 0;

            RtlZeroMemory(&devCaps, sizeof (DEVICE_CAPABILITIES));
            devCaps.Size = sizeof (DEVICE_CAPABILITIES);
            devCaps.Version = 1;
            devCaps.Address = devCaps.UINumber = (ULONG)-1;

            stack = IoGetNextIrpStackLocation (Irp);
            stack->MinorFunction = IRP_MN_QUERY_CAPABILITIES;
            stack->Parameters.DeviceCapabilities.Capabilities = &devCaps;

            status = KeyboardSendIrpSynchronously (data->TopPort, Irp, FALSE);

            if (NT_SUCCESS (status) && NT_SUCCESS (Irp->IoStatus.Status)) {
                data->MinDeviceWakeState = devCaps.DeviceWake; //来自上面的查询
                data->MinSystemWakeState = devCaps.SystemWake; //来自上面的查询

                RtlCopyMemory (data->SystemToDeviceState,
                               devCaps.DeviceState,
                               sizeof(DEVICE_POWER_STATE) * PowerSystemHibernate);
            } else {
                ASSERTMSG ("Get Device caps Failed!\n", status);
            }

            //
            // Set everything back to the way it was and continue with the start
            //
            status = STATUS_SUCCESS;
            Irp->IoStatus.Status = startStatus;
            Irp->IoStatus.Information = startInformation;

            data->Started = TRUE;

            if (WAITWAKE_SUPPORTED (data)) {
                //
                // register for the wait wake guid as well
                //
                data->WmiLibInfo.GuidCount = sizeof (KeyboardClassWmiGuidList) /
                                             sizeof (WMIGUIDREGINFO);
                ASSERT (2 == data->WmiLibInfo.GuidCount);

                //
                // See if the user has enabled wait wake for the device
                //
                KeyboardClassGetWaitWakeEnableState (data);
            }
            else {
                data->WmiLibInfo.GuidCount = (sizeof (KeyboardClassWmiGuidList) /
                                              sizeof (WMIGUIDREGINFO)) - 1;
                ASSERT (1 == data->WmiLibInfo.GuidCount);
            }
            data->WmiLibInfo.GuidList = KeyboardClassWmiGuidList;
            data->WmiLibInfo.QueryWmiRegInfo = KeyboardClassQueryWmiRegInfo;
            data->WmiLibInfo.QueryWmiDataBlock = KeyboardClassQueryWmiDataBlock;
            data->WmiLibInfo.SetWmiDataBlock = KeyboardClassSetWmiDataBlock;
            data->WmiLibInfo.SetWmiDataItem = KeyboardClassSetWmiDataItem;
            data->WmiLibInfo.ExecuteWmiMethod = NULL;
            data->WmiLibInfo.WmiFunctionControl = NULL;

            IoWMIRegistrationControl(data->Self,
                                     WMIREG_ACTION_REGISTER
                                     );

            ExAcquireFastMutex (&Globals.Mutex);
            if (Globals.GrandMaster) {
                if (0 < Globals.Opens) {
                    port = &Globals.AssocClassList[data->UnitId];
                    ASSERT (port->Port == data);
                    file = &port->File;
                    enabled = port->Enabled;
                    port->Enabled = TRUE;
                    ExReleaseFastMutex (&Globals.Mutex);

                    if (!enabled) {
                        status = KbdEnableDisablePort (TRUE, Irp, data, file);
                        if (!NT_SUCCESS (status)) {
                            port->Enabled = FALSE;
                            // ASSERT (Globals.AssocClassList[data->UnitId].Enabled);
                        } else {
                            //
                            // This is not the first kb to start, make sure its
                            // lights are set according to the indicators on the
                            // other kbs
                            //
                            PVOID startBuffer;

                            stack = IoGetNextIrpStackLocation (Irp);
                            stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
                            stack->Parameters.DeviceIoControl.IoControlCode =
                                IOCTL_KEYBOARD_SET_INDICATORS;

                            stack->FileObject = *file;
                            stack->Parameters.DeviceIoControl.OutputBufferLength = 0;
                            stack->Parameters.DeviceIoControl.InputBufferLength =
                                sizeof (KEYBOARD_INDICATOR_PARAMETERS);

                            startStatus = Irp->IoStatus.Status;
                            startInformation = Irp->IoStatus.Information;
                            startBuffer = Irp->AssociatedIrp.SystemBuffer;

                            Irp->IoStatus.Information = 0;
                            Irp->AssociatedIrp.SystemBuffer =
                                &Globals.GrandMaster->IndicatorParameters;

                            status =
                                KeyboardSendIrpSynchronously (data->TopPort,
                                                              Irp,
                                                              FALSE);

                            //
                            // We don't care if we succeeded or not...
                            // set everything back to the way it was and
                            // continue with the start
                            //
                            status = STATUS_SUCCESS;
                            Irp->IoStatus.Status = startStatus;
                            Irp->IoStatus.Information = startInformation;
                            Irp->AssociatedIrp.SystemBuffer = startBuffer;
                        }
                    }
                } else {
                    ASSERT (!Globals.AssocClassList[data->UnitId].Enabled);
                    ExReleaseFastMutex (&Globals.Mutex);
                }
            } else {
                ExReleaseFastMutex (&Globals.Mutex);
				//没有大师的时候，本层设备的真设备就是自己
                ASSERT (data->Self == data->TrueClassDevice);
                status=IoSetDeviceInterfaceState(&data->SymbolicLinkName, TRUE);//使能设备接口
            }

            //
            // Start up the Wait Wake Engine if required.
            //
            if (SHOULD_SEND_WAITWAKE (data)) {
                KeyboardClassCreateWaitWakeIrp (data);
            }
        }

        //
        // We must now complete the IRP, since we stopped it in the
        // completetion routine with MORE_PROCESSING_REQUIRED.
        //
        Irp->IoStatus.Status = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest (Irp, IO_NO_INCREMENT);
        break;

    case IRP_MN_STOP_DEVICE:
        //
        // After the start IRP has been sent to the lower driver object, the
        // bus may NOT send any more IRPS down ``touch'' until another START
        // has occured.
        // What ever access is required must be done before the Irp is passed
        // on.
        //

        //
        // Do what ever
        //

        //
        // Stop Device touching the hardware KbdStopDevice(data, TRUE);
        //
        if (data->Started) {
            ExAcquireFastMutex (&Globals.Mutex);
            if (Globals.GrandMaster) {
                if (0 < Globals.Opens) {
                    port = &Globals.AssocClassList[data->UnitId];
                    ASSERT (port->Port == data);
                    file = &(port->File);
                    enabled = port->Enabled;
                    port->Enabled = FALSE;
                    ExReleaseFastMutex (&Globals.Mutex);

                    if (enabled) {
                        notifyHandle = InterlockedExchangePointer (
                                           &data->TargetNotifyHandle,
                                           NULL);

                        if (NULL != notifyHandle) {
                            IoUnregisterPlugPlayNotification (notifyHandle);
                        }

                        status = KbdEnableDisablePort (FALSE, Irp, data, file);
                        ASSERTMSG ("Could not close open port", NT_SUCCESS(status));
                    } else {
                        ASSERT (NULL == data->TargetNotifyHandle);
                    }
                } else {
                    ASSERT (!Globals.AssocClassList[data->UnitId].Enabled);
                    ExReleaseFastMutex (&Globals.Mutex);
                }
            } else {
                ExReleaseFastMutex (&Globals.Mutex);
            }
        }

        data->Started = FALSE;

        //
        // We don't need a completion routine so fire and forget.
        //
        // Set the current stack location to the next stack location and
        // call the next device object.
        //
        IoSkipCurrentIrpStackLocation (Irp);
        status = IoCallDriver (data->TopPort, Irp);
        break;

    case IRP_MN_SURPRISE_REMOVAL:
        //
        // The PlugPlay system has dictacted the removal of this device.
        //
        data->SurpriseRemoved = TRUE;

        //
        // If add device fails, then the buffer will be NULL
        //
        if (data->SymbolicLinkName.Buffer != NULL) {
            IoSetDeviceInterfaceState (&data->SymbolicLinkName, FALSE);//不使能设备接口
        }

        //
        // We don't need a completion routine so fire and forget.
        //
        // Set the current stack location to the next stack location and
        // call the next device object.
        //
        IoSkipCurrentIrpStackLocation (Irp);
        status = IoCallDriver (data->TopPort, Irp);
        break;

    case IRP_MN_REMOVE_DEVICE:
        //
        // The PlugPlay system has dictacted the removal of this device.  We
        // have no choise but to detach and delete the device objecct.
        // (If we wanted to express and interest in preventing this removal,
        // we should have filtered the query remove and query stop routines.)
        //
        KeyboardClassRemoveDevice (data);

        //
        // Here if we had any outstanding requests in a personal queue we should
        // complete them all now.
        //
        // Note, the device is guarenteed stopped, so we cannot send it any non-
        // PNP IRPS.
        //

        //
        // Send on the remove IRP
        //
        IoCopyCurrentIrpStackLocationToNext (Irp);
        status = IoCallDriver (data->TopPort, Irp);

        ExAcquireFastMutex (&Globals.Mutex);
        if (Globals.GrandMaster) {
            ASSERT (Globals.GrandMaster->Self == data->TrueClassDevice);//大师自己也是个真设备啊
            //
            // We must remove ourself from the Assoc List
            //

            if (1 < Globals.NumAssocClass) {
                ASSERT (Globals.AssocClassList[data->UnitId].Port == data);

                Globals.AssocClassList[data->UnitId].Free = TRUE;
                Globals.AssocClassList[data->UnitId].File = NULL;
                Globals.AssocClassList[data->UnitId].Port = NULL;

            } else {
                ASSERT (1 == Globals.NumAssocClass);
                Globals.NumAssocClass = 0;
                ExFreePool (Globals.AssocClassList);
                Globals.AssocClassList = NULL;
            }
            ExReleaseFastMutex (&Globals.Mutex);

        } else {
            //
            // We are removing the one and only port associated with this class
            // device object.
            //
            ExReleaseFastMutex (&Globals.Mutex);
			//没有大师的时候，本层设备的真设备就是自己
            ASSERT (data->TrueClassDevice == data->Self);
            ASSERT (Globals.ConnectOneClassToOnePort);
        }

        IoReleaseRemoveLockAndWait (&data->RemoveLock, Irp);

        IoDetachDevice (data->TopPort);

        //
        // Clean up memory
        //
        RtlFreeUnicodeString (&data->SymbolicLinkName);//用户的责任
        ExFreePool (data->InputData);
        IoDeleteDevice (data->Self);

        return status;

    case IRP_MN_QUERY_PNP_DEVICE_STATE:

        //
        // Set the not disableable bit on the way down so that the stack below
        // has a chance to clear it
        //
        if (data->AllowDisable == FALSE) {

            (PNP_DEVICE_STATE) Irp->IoStatus.Information |=
                PNP_DEVICE_NOT_DISABLEABLE;

            Irp->IoStatus.Status = STATUS_SUCCESS;

            //
            // drop through to the default case
            //              ||  ||
            //              \/  \/
            //
        }

    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_QUERY_DEVICE_RELATIONS:
    case IRP_MN_QUERY_INTERFACE:
    case IRP_MN_QUERY_CAPABILITIES:
    case IRP_MN_QUERY_RESOURCES:
    case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
    case IRP_MN_READ_CONFIG:
    case IRP_MN_WRITE_CONFIG:
    case IRP_MN_EJECT:
    case IRP_MN_SET_LOCK:
    case IRP_MN_QUERY_ID:
    default:
        //
        // Here the filter driver might modify the behavior of these IRPS
        // Please see PlugPlay documentation for use of these IRPs.
        //

        IoCopyCurrentIrpStackLocationToNext (Irp);
        status = IoCallDriver (data->TopPort, Irp);
        break;
    }

    IoReleaseRemoveLock (&data->RemoveLock, Irp);

    return status;
}

//下面函数被两个地方调用，这个函数是个工具函数，主要是同步地完成一个IRP，需要等待一会儿：
// PNP中：IRP_MN_START_DEVICE
// IRP_MJ_CREATE:KbdEnableDisablePort
NTSTATUS
KeyboardSendIrpSynchronously (
    IN PDEVICE_OBJECT   DeviceObject,
    IN PIRP             Irp,
    IN BOOLEAN          CopyToNext
    )
{
    KEVENT      event;
    NTSTATUS    status;

    PAGED_CODE ();

    KeInitializeEvent(&event, SynchronizationEvent, FALSE);

    if (CopyToNext) {
        IoCopyCurrentIrpStackLocationToNext(Irp);
    }

    IoSetCompletionRoutine(Irp,
                           KbdSyncComplete,
                           &event,
                           TRUE,                // on success
                           TRUE,                // on error
                           TRUE                 // on cancel
                           );

    IoCallDriver(DeviceObject, Irp);

    //
    // Wait for lower drivers to be done with the Irp
    //
    KeWaitForSingleObject(&event,
                         Executive,
                         KernelMode,
                         FALSE,
                         NULL
                         );
    status = Irp->IoStatus.Status;

    return status;
}

NTSTATUS
KbdSyncComplete (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context
    )
/*++

Routine Description:
    The pnp IRP is in the process of completing.
    signal

Arguments:
    Context set to the device object in question.

--*/
{
    UNREFERENCED_PARAMETER (DeviceObject);


    KeSetEvent ((PKEVENT) Context, 0, FALSE);//完成函数中释放信号的例子

    return STATUS_MORE_PROCESSING_REQUIRED;
}


//IRP_MN_START_DEVICE时被调用
//输出参数放这里：Data->WaitWakeEnabled
void
KeyboardClassGetWaitWakeEnableState(
    IN PDEVICE_EXTENSION Data
    )
{
    HANDLE hKey;
    NTSTATUS status;
    ULONG tmp;
    BOOLEAN wwEnableFound;

    hKey = NULL;
    wwEnableFound = FALSE;

    status = IoOpenDeviceRegistryKey (Data->PDO,
                                      PLUGPLAY_REGKEY_DEVICE,
                                      STANDARD_RIGHTS_ALL,
                                      &hKey);

    if (NT_SUCCESS (status)) {
        status = KeyboardQueryDeviceKey (hKey,
                                         KEYBOARD_WAIT_WAKE_ENABLE,
                                         &tmp,
                                         sizeof (tmp));
        if (NT_SUCCESS (status)) {
            wwEnableFound = TRUE;
            Data->WaitWakeEnabled = (tmp ? TRUE : FALSE);
        }
        ZwClose (hKey);
        hKey = NULL;
    }

}

//公共函数：AddDevice和Pnp中都使用
NTSTATUS
KeyboardQueryDeviceKey (
    IN  HANDLE  Handle,
    IN  PWCHAR  ValueNameString,
    OUT PVOID   Data,
    IN  ULONG   DataLength
    )
{
    NTSTATUS        status;
    UNICODE_STRING  valueName;
    ULONG           length;
    PKEY_VALUE_FULL_INFORMATION fullInfo;

    PAGED_CODE();

    RtlInitUnicodeString (&valueName, ValueNameString);

    length = sizeof (KEY_VALUE_FULL_INFORMATION)
           + valueName.MaximumLength
           + DataLength;

    fullInfo = ExAllocatePool (PagedPool, length);

    if (fullInfo) {
        status = ZwQueryValueKey (Handle,
                                  &valueName,
                                  KeyValueFullInformation,
                                  fullInfo,
                                  length,
                                  &length);

        if (NT_SUCCESS (status)) {
            ASSERT (DataLength == fullInfo->DataLength);
            RtlCopyMemory (Data,
                           ((PUCHAR) fullInfo) + fullInfo->DataOffset,
                           fullInfo->DataLength);
        }

        ExFreePool (fullInfo);
    } else {
        status = STATUS_NO_MEMORY;
    }

    return status;
}

//IRP_MN_REMOVE_DEVICE时被调用
void
KeyboardClassRemoveDevice(
    IN PDEVICE_EXTENSION Data
    )
{
    PFILE_OBJECT *  file;
    PPORT           port;
    PIRP            waitWakeIrp;
    PVOID           notifyHandle;
    BOOLEAN         enabled;

    //
    // If this is a surprise remove or we got a remove w/out a surprise remove,
    // then we need to clean up
    //
    waitWakeIrp = (PIRP)
        InterlockedExchangePointer(&Data->WaitWakeIrp, NULL);

    if (waitWakeIrp) {
        IoCancelIrp(waitWakeIrp);
    }

    IoWMIRegistrationControl (Data->Self, WMIREG_ACTION_DEREGISTER);

    if (Data->Started) {
        //
        // Stop the device without touching the hardware.
        // MouStopDevice(Data, FALSE);
        //
        // NB sending down the enable disable does NOT touch the hardware
        // it instead sends down a close file.
        //
        ExAcquireFastMutex (&Globals.Mutex);
        if (Globals.GrandMaster) {
            if (0 < Globals.Opens) {
                port = &Globals.AssocClassList[Data->UnitId];
                ASSERT (port->Port == Data);
                file = &(port->File);
                enabled = port->Enabled;
                port->Enabled = FALSE;
                ExReleaseFastMutex (&Globals.Mutex);

                //
                // ASSERT (NULL == Data->notifyHandle);
                //
                // If we have a grand master, that means we did the
                // creation locally and registered for notification.
                // we should have closed the file during
                // TARGET_DEVICE_QUERY_REMOVE, but we will have not
                // gotten rid of the notify handle.
                //
                // Of course if we receive a surprise removal then
                // we should not have received the query cancel.
                // In which case we should have received a
                // TARGET_DEVICE_REMOVE_COMPLETE where we would have
                // both closed the file and removed cleared the
                // notify handle
                //
                // Either way the file should be closed by now.
                //
                ASSERT (!enabled);
                // if (enabled) {
                //     status = MouEnableDisablePort (FALSE, Irp, Data, file);
                //     ASSERTMSG ("Could not close open port", NT_SUCCESS(status));
                // }

                notifyHandle = InterlockedExchangePointer (
                                   &Data->TargetNotifyHandle,
                                   NULL);

                if (NULL != notifyHandle) {
                    IoUnregisterPlugPlayNotification (notifyHandle);
                }
            }
            else {
                ASSERT (!Globals.AssocClassList[Data->UnitId].Enabled);
                ExReleaseFastMutex (&Globals.Mutex);
            }
        }
        else {
            ExReleaseFastMutex (&Globals.Mutex);
			//没有大师的时候，本层设备的真设备就是自己
            ASSERT (Data->TrueClassDevice == Data->Self);
            ASSERT (Globals.ConnectOneClassToOnePort);

            if (!Data->SurpriseRemoved) {
                //
                // If add device fails, then the buffer will be NULL
                //
                if (Data->SymbolicLinkName.Buffer != NULL) {
                    IoSetDeviceInterfaceState (&Data->SymbolicLinkName, FALSE);//不使能设备接口
                }
            }

        }
    }

    //
    // Always drain the queue, no matter if we have received both a surprise
    // remove and a remove
    //
    if (Data->PnP) {
        //
        // Empty the device I/O Queue
        //
        KeyboardClassCleanupQueue (Data->Self, Data, NULL);
    }
}


//allocates a power IRP and sends it to the top driver in the device stack for the specified device.
//不单单是创建一个功率IRP
//还发送出去了，发给顶层的设备还是驱动？
BOOLEAN
KeyboardClassCreateWaitWakeIrp (
    IN PDEVICE_EXTENSION Data
    )
/*++

Routine Description:
    Catch the Wait wake Irp on its way back.

Return Value:

--*/
{
    POWER_STATE powerState;
    BOOLEAN     success = TRUE;
    NTSTATUS    status;
    PIRP        waitWakeIrp;

    PAGED_CODE ();

    powerState.SystemState = Data->MinSystemWakeState;
    status = PoRequestPowerIrp (Data->PDO, //顶层DeviceObject？
                                IRP_MN_WAIT_WAKE,
                                powerState,
                                KeyboardClassWaitWakeComplete,//完成函数
                                Data,//完成函数参数
                                NULL);

    if (status != STATUS_PENDING) {
        success = FALSE;
    }

    return success;
}

VOID
KeyboardClassWaitWakeComplete(
    IN PDEVICE_OBJECT DeviceObject,
    IN UCHAR MinorFunction,
    IN POWER_STATE PowerState,
    IN PVOID Context,
    IN PIO_STATUS_BLOCK IoStatus
    )
/*++

Routine Description:
    Catch the Wait wake Irp on its way back.

Return Value:

--*/
{
    PDEVICE_EXTENSION       data = Context;
    POWER_STATE             powerState;
    NTSTATUS                status;
    PKEYBOARD_WORK_ITEM_DATA    itemData;

    ASSERT (MinorFunction == IRP_MN_WAIT_WAKE);
    //
    // PowerState.SystemState is undefined when the WW irp has been completed
    //
    // ASSERT (PowerState.SystemState == PowerSystemWorking);

    if (InterlockedExchangePointer(&data->ExtraWaitWakeIrp, NULL)) {
        ASSERT(IoStatus->Status == STATUS_INVALID_DEVICE_STATE);
    } else {
        InterlockedExchangePointer(&data->WaitWakeIrp, NULL);
    }

    switch (IoStatus->Status) {
    case STATUS_SUCCESS:
        KbdPrint((1, "KbdClass: Wake irp was completed succeSSfully.\n"));

        //
        //      We need to request a set power to power up the device.
        //
        powerState.DeviceState = PowerDeviceD0;
        status = PoRequestPowerIrp(
                    data->PDO,
                    IRP_MN_SET_POWER,
                    powerState,
                    KeyboardClassWWPowerUpComplete,
                    Context,
                    NULL);

        //
        // We do not notify the system that a user is present because:
        // 1  Win9x doesn't do this and we must maintain compatibility with it
        // 2  The USB PIX4 motherboards sends a wait wake event every time the
        //    machine wakes up, no matter if this device woke the machine or not
        //
        // If we incorrectly notify the system a user is present, the following
        // will occur:
        // 1  The monitor will be turned on
        // 2  We will prevent the machine from transitioning from standby
        //    (to PowerSystemWorking) to hibernate
        //
        // If a user is truly present, we will receive input in the service
        // callback and we will notify the system at that time.
        //
        // PoSetSystemState (ES_USER_PRESENT);

        // fall through to the break

    //
    // We get a remove.  We will not (obviously) send another wait wake
    //
    case STATUS_CANCELLED:

    //
    // This status code will be returned if the device is put into a power state
    // in which we cannot wake the machine (hibernate is a good example).  When
    // the device power state is returned to D0, we will attempt to rearm wait wake
    //
    case STATUS_POWER_STATE_INVALID:
    case STATUS_ACPI_POWER_REQUEST_FAILED:

    //
    // We failed the Irp because we already had one queued, or a lower driver in
    // the stack failed it.  Either way, don't do anything.
    //
    case STATUS_INVALID_DEVICE_STATE:

    //
    // Somehow someway we got two WWs down to the lower stack.
    // Let's just don't worry about it.
    //
    case STATUS_DEVICE_BUSY:
        break;

    default:
        //
        // Something went wrong, disable the wait wake.
        //
        KdPrint(("KBDCLASS:  wait wake irp failed with %x\n", IoStatus->Status));
        KeyboardToggleWaitWake (data, FALSE);
    }

}


VOID
KeyboardClassWWPowerUpComplete(
    IN PDEVICE_OBJECT DeviceObject,
    IN UCHAR MinorFunction,
    IN POWER_STATE PowerState,
    IN PVOID Context,
    IN PIO_STATUS_BLOCK IoStatus
    )
/*++

Routine Description:
    Catch the Wait wake Irp on its way back.

Return Value:

--*/
{
    PDEVICE_EXTENSION       data = Context;
    POWER_STATE             powerState;
    NTSTATUS                status;
    PKEYBOARD_WORK_ITEM_DATA    itemData;

    ASSERT (MinorFunction == IRP_MN_SET_POWER);

    if (data->WaitWakeEnabled) {
        //
        // We cannot call CreateWaitWake from this completion routine,
        // as it is a paged function.
        //
        itemData = (PKEYBOARD_WORK_ITEM_DATA)
                ExAllocatePool (NonPagedPool, sizeof (KEYBOARD_WORK_ITEM_DATA));

        if (NULL != itemData) {
            itemData->Item = IoAllocateWorkItem(data->Self);
            if (itemData->Item == NULL) {
                ExFreePool(itemData);
                goto CreateWaitWakeWorkerError;
            }

            itemData->Data = data;
            itemData->Irp = NULL;
            status = IoAcquireRemoveLock (&data->RemoveLock, itemData);
            if (NT_SUCCESS(status)) {
                IoQueueWorkItem (itemData->Item,
                                 KeyboardClassCreateWaitWakeIrpWorker,
                                 DelayedWorkQueue,
                                 itemData);
            }
            else {
                //
                // The device has been removed
                //
                IoFreeWorkItem (itemData->Item);
                ExFreePool (itemData);
            }
        } else {
CreateWaitWakeWorkerError:
            //
            // Well, we dropped the WaitWake.
            //
            // Print a warning to the debugger, and log an error.
            //
            DbgPrint ("KbdClass: WARNING: Failed alloc pool -> no WW Irp\n");

            KeyboardClassLogError (data->Self,
                                   KBDCLASS_NO_RESOURCES_FOR_WAITWAKE,
                                   2,
                                   STATUS_INSUFFICIENT_RESOURCES,
                                   0,
                                   NULL,
                                   0);
        }
    }
}

void
KeyboardClassCreateWaitWakeIrpWorker (
    IN PDEVICE_OBJECT DeviceObject,
    IN PKEYBOARD_WORK_ITEM_DATA  ItemData
    )
{
    PAGED_CODE ();

	//下面的函数在前面
    KeyboardClassCreateWaitWakeIrp (ItemData->Data);
    IoReleaseRemoveLock (&ItemData->Data->RemoveLock, ItemData);
    IoFreeWorkItem(ItemData->Item);
    ExFreePool (ItemData);
}

