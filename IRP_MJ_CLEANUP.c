NTSTATUS
KeyboardClassCleanup(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )

/*++

Routine Description:

    This routine is the dispatch routine for cleanup requests.
    All requests queued to the keyboard class device (on behalf of
    the thread for whom the cleanup request was generated) are
    completed with STATUS_CANCELLED.

Arguments:

    DeviceObject - Pointer to class device object.

    Irp - Pointer to the request packet.

Return Value:

    Status is returned.

--*/

{
    PDEVICE_EXTENSION deviceExtension;
    PIO_STACK_LOCATION irpSp;

    KbdPrint((2,"KBDCLASS-KeyboardClassCleanup: enter\n"));

    deviceExtension = DeviceObject->DeviceExtension;

    //
    // Get a pointer to the current stack location for this request.
    //

    irpSp = IoGetCurrentIrpStackLocation(Irp);

    //
    // If the file object is the FileTrustedForRead, then the cleanup
    // request is being executed by the trusted subsystem.  Since the
    // trusted subsystem is the only one with sufficient privilege to make
    // Read requests to the driver, and since only Read requests get queued
    // to the device queue, a cleanup request from the trusted subsystem is
    // handled by cancelling all queued requests.
    //
    // If not, there is no cleanup work to perform
    // (only read requests can be cancelled).
    //

    if (IS_TRUSTED_FILE_FOR_READ (irpSp->FileObject)) {
        KeyboardClassCleanupQueue (DeviceObject, deviceExtension, irpSp->FileObject);
    }

    //
    // Complete the cleanup request with STATUS_SUCCESS.
    //

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest (Irp, IO_NO_INCREMENT);

    KbdPrint((2,"KBDCLASS-KeyboardClassCleanup: exit\n"));

    return(STATUS_SUCCESS);

}