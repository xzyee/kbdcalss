NTSTATUS
KeyboardClassFlush(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )

/*++

Routine Description:

    This routine is the dispatch routine for flush requests.  The class
    input data queue is reinitialized.

Arguments:

    DeviceObject - Pointer to class device object.

    Irp - Pointer to the request packet.

Return Value:

    Status is returned.

--*/

{
    PDEVICE_EXTENSION deviceExtension;
    NTSTATUS status = STATUS_SUCCESS;
    PIO_STACK_LOCATION irpSp;

    KbdPrint((2,"KBDCLASS-KeyboardClassFlush: enter\n"));

    deviceExtension = DeviceObject->DeviceExtension;
    irpSp = IoGetCurrentIrpStackLocation(Irp);

    if (deviceExtension->Self != deviceExtension->TrueClassDevice) {
        status = STATUS_NOT_SUPPORTED;

    } else if (!IS_TRUSTED_FILE_FOR_READ (irpSp->FileObject)) {
        status = STATUS_PRIVILEGE_NOT_HELD;
    }

    if (NT_SUCCESS (status)) {
        //
        // Initialize keyboard class input data queue.
        //
        KbdInitializeDataQueue((PVOID)deviceExtension);
    }

    //
    // Complete the request and return status.
    //
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    KbdPrint((2,"KBDCLASS-KeyboardClassFlush: exit\n"));

    return(status);

}