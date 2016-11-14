NTSTATUS
KeyboardClassPower (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )
/*++

Routine Description:

    The power dispatch routine.

    In all cases it must call PoStartNextPowerIrp
    In all cases (except failure) it must pass on the IRP to the lower driver.

Arguments:

   DeviceObject - pointer to a device object.

   Irp - pointer to an I/O Request Packet.

Return Value:

      NT status code

--*/
{
    POWER_STATE_TYPE        powerType;
    PIO_STACK_LOCATION      stack;
    PDEVICE_EXTENSION       data;
    NTSTATUS        status;
    POWER_STATE     powerState;
    BOOLEAN         hookit = FALSE;
    BOOLEAN         pendit = FALSE;

    PAGED_CODE ();

    data = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;
    stack = IoGetCurrentIrpStackLocation (Irp);
    powerType = stack->Parameters.Power.Type;
    powerState = stack->Parameters.Power.State;

    if (data == Globals.GrandMaster) {
        //
        // We should never get a power irp to the grand master.
        //
        ASSERT (data != Globals.GrandMaster);
        PoStartNextPowerIrp (Irp);
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        IoCompleteRequest (Irp, IO_NO_INCREMENT);
        return STATUS_NOT_SUPPORTED;

    } else if (!data->PnP) {
        //
        // We should never get a power irp to a non PnP device object.
        //
        ASSERT (data->PnP);
        PoStartNextPowerIrp (Irp);
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        IoCompleteRequest (Irp, IO_NO_INCREMENT);
        return STATUS_NOT_SUPPORTED;
    }

    status = IoAcquireRemoveLock (&data->RemoveLock, Irp);

    if (!NT_SUCCESS (status)) {
        PoStartNextPowerIrp (Irp);
        Irp->IoStatus.Status = status;
        IoCompleteRequest (Irp, IO_NO_INCREMENT);
        return status;
    }

    switch (stack->MinorFunction) {
    case IRP_MN_SET_POWER:
        KbdPrint((2,"KBDCLASS-PnP Setting %s state to %d\n",
                  ((powerType == SystemPowerState) ?  "System" : "Device"),
                  powerState.SystemState));

        switch (powerType) {
        case DevicePowerState:
            status = Irp->IoStatus.Status = STATUS_SUCCESS;
            if (data->DeviceState < powerState.DeviceState) {
                //
                // Powering down
                // 设备power down还真没什么事，仅仅保存一下down到什么功率
                PoSetPowerState (data->Self, powerType, powerState);
                data->DeviceState = powerState.DeviceState;
            }
            else if (powerState.DeviceState < data->DeviceState) {
                //
                // Powering Up
                //
                hookit = TRUE;
            } // else { no change }.
            break;

        case SystemPowerState:

            if (data->SystemState < powerState.SystemState) {
                //
                // Powering down
                //
                status = IoAcquireRemoveLock (&data->RemoveLock, Irp);
                if (!NT_SUCCESS(status)) {
                    //
                    // This should never happen b/c we successfully acquired
                    // the lock already, but we must handle this case
                    //
                    // The S irp will completed with the value in status
                    //
                    break;
                }

				//计算powerState.DeviceState到底是什么
				//如果支持WW，并且数据合理的话，可以使用S[D]映射，否则统统D3
				if (WAITWAKE_ON (data) &&
                    powerState.SystemState < PowerSystemHibernate) {
                    ASSERT (powerState.SystemState >= PowerSystemWorking &&
                            powerState.SystemState < PowerSystemHibernate);

                    powerState.DeviceState =
                        data->SystemToDeviceState[powerState.SystemState];
                }
                else {
                    powerState.DeviceState = PowerDeviceD3;
                }

                IoMarkIrpPending(Irp);
                status  = PoRequestPowerIrp (data->Self,
                                             IRP_MN_SET_POWER,
                                             powerState,
                                             KeyboardClassPoRequestComplete,
                                             Irp,
                                             NULL);

                if (!NT_SUCCESS(status)) {
                    //
                    // Failure...release the inner reference we just took
                    //
                    IoReleaseRemoveLock (&data->RemoveLock, Irp);

                    //
                    // Propagate the failure back to the S irp
                    //
                    PoStartNextPowerIrp (Irp);
                    Irp->IoStatus.Status = status;
                    IoCompleteRequest(Irp, IO_NO_INCREMENT);

                    //
                    // Release the outer reference (top of the function)
                    //
                    IoReleaseRemoveLock (&data->RemoveLock, Irp);

                    //
                    // Must return status pending b/c we marked the irp pending
                    // so we special case the return here and avoid overly
                    // complex processing at the end of the function.
                    //
                    return STATUS_PENDING;
                }
                else {
                    pendit = TRUE; //S_IRP的power down需要pending，因为暂时终止了S_IRP的下传
                }
            }
            else if (powerState.SystemState < data->SystemState) {
                //
                // Powering Up
                //
                hookit = TRUE;
                status = Irp->IoStatus.Status = STATUS_SUCCESS;
            }
            else {
                //
                
                //
                if (powerState.SystemState == PowerSystemWorking &&
                    SHOULD_SEND_WAITWAKE (data)) {
                    KeyboardClassCreateWaitWakeIrp (data);
                }
                status = Irp->IoStatus.Status = STATUS_SUCCESS;
            }
            break;
        }

        break;

    case IRP_MN_QUERY_POWER:
        ASSERT (SystemPowerState == powerType);

        //
        // Fail the query if we can't wake the machine.  We do, however, want to
        // let hibernate succeed no matter what (besides, it is doubtful that a
        // keyboard can wait wake the machine out of S4).
        //
        if (powerState.SystemState < PowerSystemHibernate       &&
            powerState.SystemState > data->MinSystemWakeState   &&
            WAITWAKE_ON(data)) {
            status = STATUS_POWER_STATE_INVALID;
        }
        else {
            status = STATUS_SUCCESS;
        }

        Irp->IoStatus.Status = status;
        break;

    case IRP_MN_WAIT_WAKE:
        if (InterlockedCompareExchangePointer(&data->WaitWakeIrp,
                                              Irp,
                                              NULL) != NULL) {
            /*  When powering up with WW being completed at same time, there
                is a race condition between PoReq completion for S_Irp and
                completion of WW_irp. Steps to repro this:

                S_irp completes and does PoReq of D_irp with completion
                routine MouseClassPoRequestComplete
                WW Irp completion fires and the following happens:
                    1. set data->WaitWakeIrp to NULL
                    2. PoReq D_irp with completion routine MouseClassWWPowerUpComplete

                MouseClassPoRequestComplete fires first and sees no WW queued,
                so it queues one.
                MouseClassWWPowerUpComplete fires second and tries to queue
                WW. When the WW arrives in mouclass, it sees there's one
                queued already, so it fails it with invalid device state.
                The completion routine, MouseClassWaitWakeComplete, fires
                and it deletes the irp from the device extension.

                This results in the appearance of wake being disabled,
                even though the first irp is still queued.
            */

            InterlockedExchangePointer(&data->ExtraWaitWakeIrp, Irp);
            status = STATUS_INVALID_DEVICE_STATE;
        }
        else {
            status = STATUS_SUCCESS;
        }
        break;

    default:
        break;
    }

    if (!NT_SUCCESS (status)) {
        Irp->IoStatus.Status = status;
        PoStartNextPowerIrp (Irp);
        IoCompleteRequest (Irp, IO_NO_INCREMENT);

    } else if (hookit) {
		//无论S_IRP还是D_IRP的上电都到此
		//按要求就应该设置完成函数，在完成函数中做事
        status = IoAcquireRemoveLock (&data->RemoveLock, Irp);
        ASSERT (STATUS_SUCCESS == status);
        IoCopyCurrentIrpStackLocationToNext (Irp);

        IoSetCompletionRoutine (Irp,
                                KeyboardClassPowerComplete,//一函数两用途
                                NULL,
                                TRUE,
                                TRUE,
                                TRUE);
        IoMarkIrpPending(Irp);
        PoCallDriver (data->TopPort, Irp);//必须下行到Bus驱动，不理会PoCallDriver的返回

        //
        // We are returning pending instead of the result from PoCallDriver because:
        // 1  we are changing the status in the completion routine
        // 2  we will not be completing this irp in the completion routine
        //
        status = STATUS_PENDING;
    }
    else if (pendit) {
        status = STATUS_PENDING; //S_IRP的power down需要pending，因为暂时终止了S_IRP的下传
    } else {
        PoStartNextPowerIrp (Irp);
        IoSkipCurrentIrpStackLocation (Irp);
        status = PoCallDriver (data->TopPort, Irp);
    }

    IoReleaseRemoveLock (&data->RemoveLock, Irp);
    return status;
}
NTSTATUS
KeyboardClassPowerComplete (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context
    )
{
    NTSTATUS            status;
    POWER_STATE         powerState;
    POWER_STATE_TYPE    powerType;
    PIO_STACK_LOCATION  stack, next;
    PIRP                irpLeds;
    PDEVICE_EXTENSION   data;
    IO_STATUS_BLOCK     block;
    PFILE_OBJECT        file;
    PKEYBOARD_INDICATOR_PARAMETERS params;

    UNREFERENCED_PARAMETER (Context);

    data = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;
    stack = IoGetCurrentIrpStackLocation (Irp);
    next = IoGetNextIrpStackLocation (Irp);
    powerType = stack->Parameters.Power.Type;  //取回信息
    powerState = stack->Parameters.Power.State;//取回信息

    ASSERT (data != Globals.GrandMaster);
    ASSERT (data->PnP);

    switch (stack->MinorFunction) {
    case IRP_MN_SET_POWER:
        switch (powerType) {
        case DevicePowerState:
            ASSERT (powerState.DeviceState < data->DeviceState);
            //
            // Powering up
            //
            PoSetPowerState (data->Self, powerType, powerState);//此时告知合理
            data->DeviceState = powerState.DeviceState;//保存context，这里是新的DeviceState

            irpLeds = IoAllocateIrp(DeviceObject->StackSize, FALSE);
            if (irpLeds) {
                status = IoAcquireRemoveLock(&data->RemoveLock, irpLeds);

                if (NT_SUCCESS(status)) {
                    //
                    // Set the keyboard Indicators.
                    //
                    if (Globals.GrandMaster) {
                        params = &Globals.GrandMaster->IndicatorParameters;
                        file = Globals.AssocClassList[data->UnitId].File;
                    } else {
                        params = &data->IndicatorParameters;
                        file = stack->FileObject;
                    }

                    //
                    // This is a completion routine.  We could be at DISPATCH_LEVEL
                    // Therefore we must bounce the IRP
                    //
                    next = IoGetNextIrpStackLocation(irpLeds);

                    next->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
                    next->Parameters.DeviceIoControl.IoControlCode =
                        IOCTL_KEYBOARD_SET_INDICATORS;
                    next->Parameters.DeviceIoControl.InputBufferLength =
                        sizeof (KEYBOARD_INDICATOR_PARAMETERS);
                    next->Parameters.DeviceIoControl.OutputBufferLength = 0;
                    next->FileObject = file;

                    IoSetCompletionRoutine (irpLeds,
                                            KeyboardClassSetLedsComplete,
                                            data,
                                            TRUE,
                                            TRUE,
                                            TRUE);

                    irpLeds->AssociatedIrp.SystemBuffer = params;

                    IoCallDriver (data->TopPort, irpLeds);
                }
                else {
                    IoFreeIrp (irpLeds); //拿锁失败
                }
            }

            break;

        case SystemPowerState:
            ASSERT (powerState.SystemState < data->SystemState);
            //
            // Powering up
            //
            // Save the system state before we overwrite it
            //
            PoSetPowerState (data->Self, powerType, powerState);//此时告知合理
            data->SystemState = powerState.SystemState;//保存context，这里是新的SystemState
            powerState.DeviceState = PowerDeviceD0;

			//按照微软要求，在S_IRP的完成函数中应当开始SET设备功率的历程
            status = PoRequestPowerIrp (data->Self,
                                        IRP_MN_SET_POWER,
                                        powerState, //PowerDeviceD0
                                        KeyboardClassPoRequestComplete,//一函数两用途
                                        NULL, //以示区别
                                        NULL);

            //
            // Propagate the error if one occurred
            //
            if (!NT_SUCCESS(status)) {
                Irp->IoStatus.Status = status;
            }

            break;
        }
        break;

    default:
        ASSERT (0xBADBAD == stack->MinorFunction);
        break;
    }

    PoStartNextPowerIrp (Irp);
    IoReleaseRemoveLock (&data->RemoveLock, Irp);

    return STATUS_SUCCESS;
}

VOID
KeyboardClassPoRequestComplete (
    IN PDEVICE_OBJECT DeviceObject,
    IN UCHAR MinorFunction,
    IN POWER_STATE D_PowerState,
    IN PIRP S_Irp, // The S irp that caused us to request the power.
    IN PIO_STATUS_BLOCK IoStatus
    )
{
    PDEVICE_EXTENSION   data;
    PKEYBOARD_WORK_ITEM_DATA    itemData;

    data = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

    //
    // If the S_Irp is present, we are powering down.  We do not pass the S_Irp
    // as a parameter to PoRequestPowerIrp when we are powering up
    //
    if (ARGUMENT_PRESENT(S_Irp)) {
        POWER_STATE powerState;

        //
        // Powering Down
        // 到这里，设备SET POWER的完成函数都执行完毕了，设备马上要进入低功耗状态
        powerState = IoGetCurrentIrpStackLocation(S_Irp)->Parameters.Power.State;
        PoSetPowerState (data->Self, SystemPowerState, powerState);//这时告知很合理
        data->SystemState = powerState.SystemState;//保存context，这里是新的SystemState

		//往下传
        PoStartNextPowerIrp (S_Irp);
        IoSkipCurrentIrpStackLocation (S_Irp);//本层未来没有要处理的
        PoCallDriver (data->TopPort, S_Irp);

        //
        // Finally, release the lock we acquired based on this irp
        //
        IoReleaseRemoveLock (&data->RemoveLock, S_Irp);
    }
    else {
        //
        // Powering Up
        //

        //
        // We have come back to the PowerSystemWorking state and the device is
        // fully powered.  If we can (and should), send a wait wake irp down
        // the stack.  This is necessary because we might have gone into a power
        // state where the wait wake irp was invalid.
        //
        ASSERT(data->SystemState == PowerSystemWorking);

        if (SHOULD_SEND_WAITWAKE (data)) { //如果不发WW的话，就什么事都不需要做
            //
            // We cannot call CreateWaitWake from this completion routine,
            // as it is a paged function.
            //
            itemData = (PKEYBOARD_WORK_ITEM_DATA)
                    ExAllocatePool (NonPagedPool, sizeof (KEYBOARD_WORK_ITEM_DATA));

            if (NULL != itemData) {
                NTSTATUS  status;

                itemData->Item = IoAllocateWorkItem (data->Self);
                if (itemData->Item == NULL) {
                    ExFreePool (itemData);
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
                    IoFreeWorkItem (itemData->Item);
                    ExFreePool (itemData);
                    goto CreateWaitWakeWorkerError;
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
                                       1,
                                       STATUS_INSUFFICIENT_RESOURCES,
                                       0,
                                       NULL,
                                       0);
            }
        }
    }
}

//
// On some busses, we can power down the bus, but not the system, in this case
// we still need to allow the device to wake said bus, therefore
// waitwake-supported should not rely on systemstate.
//
// #define WAITWAKE_SUPPORTED(port) ((port)->MinDeviceWakeState > PowerDeviceUnspecified) && \
//                                  (port)->MinSystemWakeState > PowerSystemWorking)
#define WAITWAKE_SUPPORTED(port) ((port)->MinDeviceWakeState > PowerDeviceD0 && \
                                  (port)->MinSystemWakeState > PowerSystemWorking)

// #define WAITWAKE_ON(port)        ((port)->WaitWakeIrp != 0)
#define WAITWAKE_ON(port) \
       (InterlockedCompareExchangePointer(&(port)->WaitWakeIrp, NULL, NULL) != NULL)

#define SHOULD_SEND_WAITWAKE(port) (WAITWAKE_SUPPORTED(port) && \
                                    !WAITWAKE_ON(port)       && \
                                    KeyboardClassCheckWaitWakeEnabled(port))
