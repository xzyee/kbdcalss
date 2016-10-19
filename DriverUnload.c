VOID
KeyboardClassUnload(
    IN PDRIVER_OBJECT DriverObject
    )

/*++

Routine Description:

    This routine is the class driver unload routine.

    NOTE:  Not currently implemented.

Arguments:

    DeviceObject - Pointer to class device object.

Return Value:

    None.

--*/

{
    PLIST_ENTRY entry;
    PDEVICE_EXTENSION data;
    PPORT port;
    PIRP irp;

    UNREFERENCED_PARAMETER(DriverObject);

    PAGED_CODE ();

    KbdPrint((2,"KBDCLASS-KeyboardClassUnload: enter\n"));

    //
    // Delete all of our legacy devices
    //
    for (entry = Globals.LegacyDeviceList.Flink;
         entry != &Globals.LegacyDeviceList;
         /* advance to next before deleting the devobj */) {

        BOOLEAN enabled = FALSE;
        PFILE_OBJECT file = NULL;
		
		//通过腰带Link的地址entry找大数据结构
        data = CONTAINING_RECORD (entry, DEVICE_EXTENSION, Link);
        ASSERT (data->PnP == FALSE); //必须不是pnp的，就是哪种非pnp的传统的，比如ps2鼠标

        if (Globals.GrandMaster) {
            port = &Globals.AssocClassList[data->UnitId];
            ASSERT (port->Port == data);

            enabled = port->Enabled;
            file = port->File;

            port->Enabled = FALSE;
            port->File = NULL;
            port->Free = TRUE;
        }
        else {
            enabled = data->Enabled;
            file = data->File;
            ASSERT (data->File);
            data->Enabled = FALSE;
        }

        if (enabled) { //如果还是打开的状态，那么就要关闭
            irp = IoAllocateIrp(data->TopPort->StackSize+1, FALSE); //搞出一个IRP来关闭端口
            if (irp) {
                KbdEnableDisablePort (FALSE, irp, data, &file);//关闭端口设备
                IoFreeIrp (irp);
            }
        }

        //
        // This file object represents the open we performed on the legacy
        // port device object.  It does NOT represent the open that the RIT
        // performed on our DO.
        //
        if (file) {
            ObDereferenceObject(file);
        }


        //
        // Clean out the queue only if there is no GM
        //
        if (Globals.GrandMaster == NULL) {
            KeyboardClassCleanupQueue (data->Self, data, NULL);
        }

        RemoveEntryList (&data->Link);
        entry = entry->Flink;

        KeyboardClassDeleteLegacyDevice (data);
    }

    //
    // Delete the grandmaster if it exists
    //
    if (Globals.GrandMaster) {
        data = Globals.GrandMaster;
        Globals.GrandMaster = NULL;

        KeyboardClassCleanupQueue (data->Self, data, NULL);
        KeyboardClassDeleteLegacyDevice (data);
    }

    ExFreePool(Globals.RegistryPath.Buffer);
    if (Globals.AssocClassList) {
#if DBG
        ULONG i;

        for (i = 0; i < Globals.NumAssocClass; i++) {
            ASSERT (Globals.AssocClassList[i].Free == TRUE);
            ASSERT (Globals.AssocClassList[i].Enabled == FALSE);
            ASSERT (Globals.AssocClassList[i].File == NULL);
        }
#endif

        ExFreePool(Globals.AssocClassList);
    }

    KbdPrint((2,"KBDCLASS-KeyboardClassUnload: exit\n"));
}

VOID
KeyboardClassCleanupQueue (
    IN PDEVICE_OBJECT       DeviceObject,
    IN PDEVICE_EXTENSION    DeviceExtension,
    IN PFILE_OBJECT         FileObject
    )
/*++
Routine Description:

    This does the work of MouseClassCleanup so that we can also do that work
    during remove device for when the grand master isn't enabled.


--*/
{
    PIRP irp;
    LIST_ENTRY listHead, *entry;
    KIRQL irql;

    InitializeListHead(&listHead);

    KeAcquireSpinLock(&DeviceExtension->SpinLock, &irql);

    do {
        irp = KeyboardClassDequeueReadByFileObject(DeviceExtension, FileObject);
        if (irp) {
            irp->IoStatus.Status = STATUS_CANCELLED;
            irp->IoStatus.Information = 0;

            InsertTailList (&listHead, &irp->Tail.Overlay.ListEntry);
        }
    } while (irp != NULL);

    KeReleaseSpinLock(&DeviceExtension->SpinLock, irql);

    //
    // Complete these irps outside of the spin lock
    //
    while (! IsListEmpty (&listHead)) {
        entry = RemoveHeadList (&listHead);
        irp = CONTAINING_RECORD (entry, IRP, Tail.Overlay.ListEntry);

        IoCompleteRequest (irp, IO_NO_INCREMENT);
        IoReleaseRemoveLock (&DeviceExtension->RemoveLock, irp);
    }
}

PIRP
KeyboardClassDequeueReadByFileObject(
    IN PDEVICE_EXTENSION DeviceExtension,
    IN PFILE_OBJECT FileObject
    )
/*++

Routine Description:
    Dequeues the next available read with a matching FileObject

Assumptions:
    DeviceExtension->SpinLock is already held (so no further sync is required).

  --*/
{
    PIRP                irp = NULL;
    PLIST_ENTRY         entry;
    PIO_STACK_LOCATION  stack;
    PDRIVER_CANCEL      oldCancelRoutine;
    KIRQL oldIrql;

    if (FileObject == NULL) {
        return KeyboardClassDequeueRead (DeviceExtension);
    }

    for (entry = DeviceExtension->ReadQueue.Flink;
         entry != &DeviceExtension->ReadQueue;
         entry = entry->Flink) {

        irp = CONTAINING_RECORD (entry, IRP, Tail.Overlay.ListEntry);
        stack = IoGetCurrentIrpStackLocation (irp);
        if (stack->FileObject == FileObject) {
            RemoveEntryList (entry);

            oldCancelRoutine = IoSetCancelRoutine (irp, NULL);

            //
            // IoCancelIrp() could have just been called on this IRP.
            // What we're interested in is not whether IoCancelIrp() was called
            // (ie, nextIrp->Cancel is set), but whether IoCancelIrp() called (or
            // is about to call) our cancel routine. To check that, check the result
            // of the test-and-set macro IoSetCancelRoutine.
            //
            if (oldCancelRoutine) {
                //
                //  Cancel routine not called for this IRP.  Return this IRP.
                //
                return irp;
            }
            else {
                //
                // This IRP was just cancelled and the cancel routine was (or will
                // be) called. The cancel routine will complete this IRP as soon as
                // we drop the spinlock. So don't do anything with the IRP.
                //
                // Also, the cancel routine will try to dequeue the IRP, so make the
                // IRP's listEntry point to itself.
                //
                ASSERT (irp->Cancel);
                InitializeListHead (&irp->Tail.Overlay.ListEntry);
            }
        }
    }

    return NULL;
}

PIRP
KeyboardClassDequeueRead(
    IN PDEVICE_EXTENSION DeviceExtension
    )
/*++

Routine Description:
    Dequeues the next available read irp regardless of FileObject

Assumptions:
    DeviceExtension->SpinLock is already held (so no further sync is required).

  --*/
{
    PIRP nextIrp = NULL;
    KIRQL oldIrql;

    while (!nextIrp && !IsListEmpty (&DeviceExtension->ReadQueue)){
        PDRIVER_CANCEL oldCancelRoutine;
        PLIST_ENTRY listEntry = RemoveHeadList (&DeviceExtension->ReadQueue);

        //
        // Get the next IRP off the queue and clear the cancel routine
        //
        nextIrp = CONTAINING_RECORD (listEntry, IRP, Tail.Overlay.ListEntry);
        oldCancelRoutine = IoSetCancelRoutine (nextIrp, NULL);

        //
        // IoCancelIrp() could have just been called on this IRP.
        // What we're interested in is not whether IoCancelIrp() was called
        // (ie, nextIrp->Cancel is set), but whether IoCancelIrp() called (or
        // is about to call) our cancel routine. To check that, check the result
        // of the test-and-set macro IoSetCancelRoutine.
        //
        if (oldCancelRoutine) {
            //
                //  Cancel routine not called for this IRP.  Return this IRP.
            //
                ASSERT (oldCancelRoutine == KeyboardClassCancel);
        }
        else {
            //
                // This IRP was just cancelled and the cancel routine was (or will
            // be) called. The cancel routine will complete this IRP as soon as
            // we drop the spinlock. So don't do anything with the IRP.
            //
                // Also, the cancel routine will try to dequeue the IRP, so make the
            // IRP's listEntry point to itself.
            //
            ASSERT (nextIrp->Cancel);
            InitializeListHead (&nextIrp->Tail.Overlay.ListEntry);
                nextIrp = NULL;
        }
    }

        return nextIrp;
}

