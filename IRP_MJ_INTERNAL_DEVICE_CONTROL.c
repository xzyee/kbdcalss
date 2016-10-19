//KeyboardClassSystemControl也会用到
NTSTATUS
KeyboardClassPassThrough(
        IN PDEVICE_OBJECT DeviceObject,
        IN PIRP Irp
        )
/*++
Routine Description:

        Passes a request on to the lower driver.

--*/
{
        //
        // Pass the IRP to the target
        //
    IoSkipCurrentIrpStackLocation (Irp);
        return IoCallDriver (
        ((PDEVICE_EXTENSION) DeviceObject->DeviceExtension)->TopPort,
                Irp);
}

