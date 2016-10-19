//
// WMI System Call back functions
//
NTSTATUS
KeyboardClassSystemControl (
    IN  PDEVICE_OBJECT  DeviceObject,
    IN  PIRP            Irp
    )
/*++
Routine Description

    We have just received a System Control IRP.

    Assume that this is a WMI IRP and
    call into the WMI system library and let it handle this IRP for us.

--*/
{
    PDEVICE_EXTENSION   deviceExtension;
    NTSTATUS            status;
    SYSCTL_IRP_DISPOSITION disposition;

    deviceExtension = (PDEVICE_EXTENSION) DeviceObject->DeviceExtension;

    status = IoAcquireRemoveLock (&deviceExtension->RemoveLock, Irp);
    if (!NT_SUCCESS (status)) {
        Irp->IoStatus.Status = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    status = WmiSystemControl(&deviceExtension->WmiLibInfo,
                                 DeviceObject,
                                 Irp,
                                 &disposition);
    switch(disposition) {
    case IrpProcessed:
        //
        // This irp has been processed and may be completed or pending.
        //
        break;

    case IrpNotCompleted:
        //
        // This irp has not been completed, but has been fully processed.
        // we will complete it now
        //
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        break;

    case IrpForward:
    case IrpNotWmi:
        //
        // This irp is either not a WMI irp or is a WMI irp targetted
        // at a device lower in the stack.
        //
        status = KeyboardClassPassThrough(DeviceObject, Irp);
        break;

    default:
        //
        // We really should never get here, but if we do just forward....
        //
        ASSERT(FALSE);
        status = KeyboardClassPassThrough(DeviceObject, Irp);
        break;
    }

    IoReleaseRemoveLock (&deviceExtension->RemoveLock, Irp);
    return status;
}

//KeyboardClassPassThrough在其他地方用到
//DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = 
//                                   KeyboardClassPassThrough;