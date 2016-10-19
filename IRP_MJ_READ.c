NTSTATUS
KeyboardClassRead(
    IN PDEVICE_OBJECT Device,
    IN PIRP Irp
    )

/*++

Routine Description:

    This routine is the dispatch routine for read requests.  Valid read
    requests are either marked pending if no data has been queued or completed
    immediatedly with available data.

Arguments:

    DeviceObject - Pointer to class device object.

    Irp - Pointer to the request packet.

Return Value:

    Status is returned.

--*/

{
    NTSTATUS status;
    PIO_STACK_LOCATION irpSp; //要用到的重要东西
    PDEVICE_EXTENSION  deviceExtension;//要用到的重要东西

    KbdPrint((2,"KBDCLASS-KeyboardClassRead: enter\n"));

    irpSp = IoGetCurrentIrpStackLocation(Irp);

    //
    // Validate the read request parameters.  The read length should be an
    // integral number of KEYBOARD_INPUT_DATA structures.
    //

    deviceExtension = (PDEVICE_EXTENSION) Device->DeviceExtension;
    if (irpSp->Parameters.Read.Length == 0) {
        status = STATUS_SUCCESS;
    } else if (irpSp->Parameters.Read.Length % sizeof(KEYBOARD_INPUT_DATA)) {
        status = STATUS_BUFFER_TOO_SMALL;
    } else if (deviceExtension->SurpriseRemoved) {
        status = STATUS_DEVICE_NOT_CONNECTED;
    } else if (IS_TRUSTED_FILE_FOR_READ (irpSp->FileObject)) {
        //
        // If the file object's FsContext is non-null, then we've already
        // done the Read privilege check once before for this thread.  Skip
        // the privilege check.
        //
		//增加一个计数
        status = IoAcquireRemoveLock (&deviceExtension->RemoveLock, Irp); //irp:identifies this instance of acquiring the remove lock

        if (NT_SUCCESS (status)) {
            status = STATUS_PENDING; //从下面看要调用KeyboardClassHandleRead，因此是马上处理的意思
        }
    } else {
        //
        // We only allow a trusted subsystem with the appropriate privilege
        // level to execute a Read call.
        //

        status = STATUS_PRIVILEGE_NOT_HELD;
    }

    //
    // If status is pending, mark the packet pending and start the packet
    // in a cancellable state.  Otherwise, complete the request.
    //

	//什么意思？和返回值不一样？不要紧
	//有可能在后面读取函数中重新设置：
	//status = Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Status = status; 
    Irp->IoStatus.Information = 0;
	
	//这里STATUS_PENDING的意思是马上做，稍等的意思
	//不是本函数的返回值
    if (status == STATUS_PENDING) {
		
		//有可能返回STATUS_CANCELLED
        return KeyboardClassHandleRead(deviceExtension, Irp);
    }
    else {
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    KbdPrint((2,"KBDCLASS-KeyboardClassRead: exit\n"));

    return status;
}

//问题：ReadQueue在哪里？
//答案：在设备扩展里
NTSTATUS
KeyboardClassHandleRead(
    PDEVICE_EXTENSION DeviceExtension,
    PIRP Irp
    )
/*++

Routine Description:

    If there is queued data, the Irp will be completed immediately.  If there is
    no data to report, queue the irp.

  --*/
{
    PDRIVER_CANCEL oldCancelRoutine;
    NTSTATUS status = STATUS_PENDING;
    KIRQL irql;
    BOOLEAN completeIrp = FALSE;

	//spin锁开始=====================================================
    KeAcquireSpinLock(&DeviceExtension->SpinLock, &irql);
	//spin锁开始=====================================================

	//怎么判断是否有输入字符？原来在这里！DeviceExtension还真的不是吃干饭的
    if (DeviceExtension->InputCount == 0) {
        //
        // Easy case to handle, just enqueue the irp
        //
        InsertTailList (&DeviceExtension->ReadQueue, &Irp->Tail.Overlay.ListEntry);
        IoMarkIrpPending (Irp);

        //
        //  Must set a cancel routine before checking the Cancel flag.
        //
        oldCancelRoutine = IoSetCancelRoutine (Irp, KeyboardClassCancel);
        ASSERT (!oldCancelRoutine);

        if (Irp->Cancel) {
            //
            // The IRP was cancelled.  Check whether or not the cancel
            // routine was called.
            //
            oldCancelRoutine = IoSetCancelRoutine (Irp, NULL);
            if (oldCancelRoutine) {
                //
                // The cancel routine was NOT called so dequeue the IRP now and
                // complete it after releasing the spinlock.
                //
                RemoveEntryList (&Irp->Tail.Overlay.ListEntry);
				//重新设置本irp的状态
                status = Irp->IoStatus.Status = STATUS_CANCELLED;
            }
            else {
                //
                    //  The cancel routine WAS called.
                //
                //  As soon as we drop the spinlock it will dequeue and complete
                //  the IRP. So leave the IRP in the queue and otherwise don't
                //  touch it. Return pending since we're not completing the IRP
                //  here.
                //
                ;
            }
        }

        if (status != STATUS_PENDING){
            completeIrp = TRUE;
        }
    }
    else {
        //
        // If we have outstanding input to report, our queue better be empty!
        //
        ASSERT (IsListEmpty (&DeviceExtension->ReadQueue));

		//下面的函数总是返回STATUS_SUCCESS
        status = KeyboardClassReadCopyData (DeviceExtension, Irp);
        Irp->IoStatus.Status = status; //重新设置本irp的状态
        completeIrp = TRUE;
    }

	//spin锁结束=====================================================
    KeReleaseSpinLock (&DeviceExtension->SpinLock, irql);
	//spin锁结束=====================================================

    if (completeIrp) {
        IoReleaseRemoveLock (&DeviceExtension->RemoveLock, Irp);
        IoCompleteRequest (Irp, IO_NO_INCREMENT); //完成IRP
    }

	
	//取消了的化，返回STATUS_CANCELLED
	//当前没有字符可读，把IRP挂在ReadQueue，返回STATUS_PENDING
	//读到的化，返回STATUS_SUCCESS
    return status;
}

//总是返回STATUS_SUCCESS
//从设备扩展的ringbuffer里面往buffer里搬字符
//驱动程序的输入队列和ringbuffer是一个意思
NTSTATUS
KeyboardClassReadCopyData(
    IN PDEVICE_EXTENSION DeviceExtension,
    IN PIRP Irp
    )
/*++

Routine Description:
    Copies data as much from the internal queue to the irp as possible.

Assumptions:
    DeviceExtension->SpinLock is already held (so no further synch
    is required).

  --*/
{
    PIO_STACK_LOCATION irpSp;
    PCHAR destination;
    ULONG bytesInQueue;
    ULONG bytesToMove;
    ULONG moveSize;

    //
    // Bump the error log sequence number.
    //
    DeviceExtension->SequenceNumber += 1;

    ASSERT (DeviceExtension->InputCount != 0);

    //
    // Copy as much of the input data as possible from the class input
    // data queue to the SystemBuffer to satisfy the read.  It may be
    // necessary to copy the data in two chunks (i.e., if the circular
    // queue wraps).
    //
    irpSp = IoGetCurrentIrpStackLocation(Irp);

    //
    // BytesToMove <- MIN(Number of filled bytes in class input data queue,
    //                    Requested read length).
    //
    bytesInQueue = DeviceExtension->InputCount *
                       sizeof(KEYBOARD_INPUT_DATA);
    bytesToMove = irpSp->Parameters.Read.Length;

    KbdPrint((
        3,
        "KBDCLASS-KeyboardClassReadCopyData: queue size 0x%lx, read length 0x%lx\n",
        bytesInQueue,
        bytesToMove
        ));

    bytesToMove = (bytesInQueue < bytesToMove) ?
                                  bytesInQueue:bytesToMove;

    //
    // MoveSize <- MIN(Number of bytes to be moved from the class queue,
    //                 Number of bytes to end of class input data queue).
    //
    bytesInQueue = (ULONG)(((PCHAR) DeviceExtension->InputData +
                DeviceExtension->KeyboardAttributes.InputDataQueueLength) -
                (PCHAR) DeviceExtension->DataOut);
    moveSize = (bytesToMove < bytesInQueue) ?
                              bytesToMove:bytesInQueue;

    KbdPrint((
        3,
        "KBDCLASS-KeyboardClassReadCopyData: bytes to end of queue 0x%lx\n",
        bytesInQueue
        ));

    //
    // Move bytes from the class input data queue to SystemBuffer, until
    // the request is satisfied or we wrap the class input data buffer.
    //
    destination = Irp->AssociatedIrp.SystemBuffer;

    KbdPrint((
        3,
        "KBDCLASS-KeyboardClassReadCopyData: number of bytes in first move 0x%lx\n",
        moveSize
        ));
    KbdPrint((
        3,
        "KBDCLASS-KeyboardClassReadCopyData: move bytes from 0x%lx to 0x%lx\n",
        (PCHAR) DeviceExtension->DataOut,
        destination
        ));

    RtlMoveMemory(
        destination,
        (PCHAR) DeviceExtension->DataOut,
        moveSize
        );
    destination += moveSize;

    //
    // If the data wraps in the class input data buffer, copy the rest
    // of the data from the start of the input data queue
    // buffer through the end of the queued data.
    //读取考虑了折返的情况
    if ((bytesToMove - moveSize) > 0) {
        //
        // MoveSize <- Remaining number bytes to move.
        //
        moveSize = bytesToMove - moveSize;

        //
        // Move the bytes from the class input data queue to SystemBuffer.
        //
        KbdPrint((
            3,
            "KBDCLASS-KeyboardClassReadCopyData: number of bytes in second move 0x%lx\n",
            moveSize
            ));
        KbdPrint((
            3,
            "KBDCLASS-KeyboardClassReadCopyData: move bytes from 0x%lx to 0x%lx\n",
            (PCHAR) DeviceExtension->InputData,
            destination
            ));

		//移动字符串
        RtlMoveMemory(
            destination,//目的地，irq携带的缓冲区
            (PCHAR) DeviceExtension->InputData, //源头：输入队列
            moveSize
            );

        //
        // Update the class input data queue removal pointer.
        //
		//更新ringbuffer的移出指针
        DeviceExtension->DataOut = (PKEYBOARD_INPUT_DATA)
                         (((PCHAR) DeviceExtension->InputData) + moveSize);
    }
    else {
        //
        // Update the input data queue removal pointer.
        //
        DeviceExtension->DataOut = (PKEYBOARD_INPUT_DATA)
                         (((PCHAR) DeviceExtension->DataOut) + moveSize);
    }

    //
    // Update the class input data queue InputCount.
    //
	
	//输入ringbuffer现有长度要减掉一部分，因为读走了一部分
    DeviceExtension->InputCount -=
        (bytesToMove / sizeof(KEYBOARD_INPUT_DATA));

    if (DeviceExtension->InputCount == 0) {
        //
        // Reset the flag that determines whether it is time to log
        // queue overflow errors.  We don't want to log errors too often.
        // Instead, log an error on the first overflow that occurs after
        // the ring buffer has been emptied, and then stop logging errors
        // until it gets cleared out and overflows again.
        //
        KbdPrint((
            1,
            "KBDCLASS-KeyboardClassCopyReadData: Okay to log overflow\n"
            ));

        DeviceExtension->OkayToLogOverflow = TRUE;
    }

    KbdPrint((
        3,
        "KBDCLASS-KeyboardClassCopyReadData: new DataIn 0x%lx, DataOut 0x%lx\n",
        DeviceExtension->DataIn,
        DeviceExtension->DataOut
        ));
    KbdPrint((
        3,
        "KBDCLASS-KeyboardClassCopyReadData: new InputCount %ld\n",
        DeviceExtension->InputCount
        ));

    //
    // Record how many bytes we have satisfied
    //
	//操作结果填到IO_STACK_LOCATION，这也是一种通讯
    Irp->IoStatus.Information = bytesToMove;
    irpSp->Parameters.Read.Length = bytesToMove;

    return STATUS_SUCCESS;//总是返回STATUS_SUCCESS，合理
}

VOID
KeyboardClassCancel(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )

/*++

Routine Description:

    This routine is the class cancellation routine.  It is
    called from the I/O system when a request is cancelled.  Read requests
    are currently the only cancellable requests.

    N.B.  The cancel spinlock is already held upon entry to this routine.
          Also, there is no ISR to synchronize with.

Arguments:

    DeviceObject - Pointer to class device object.

    Irp - Pointer to the request packet to be cancelled.

Return Value:

    None.

--*/

{
    PDEVICE_EXTENSION deviceExtension;
    KIRQL irql;

    deviceExtension = DeviceObject->DeviceExtension;

    //
    //  Release the global cancel spinlock.
    //  Do this while not holding any other spinlocks so that we exit at the
    //  right IRQL.
    //
	
	//madn:The IoReleaseCancelSpinLock routine releases the cancel spin lock 
	//     after the driver has changed the cancelable state of an IRP.
	//找不到IoAcquireCancelSpinLock在哪里？难道是IO Manager？
    IoReleaseCancelSpinLock (Irp->CancelIrql); //要恢复的IRQL

    //
    // Dequeue and complete the IRP.  The enqueue and dequeue functions
    // synchronize properly so that if this cancel routine is called,
    // the dequeue is safe and only the cancel routine will complete the IRP.
    //
    KeAcquireSpinLock(&deviceExtension->SpinLock, &irql);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&deviceExtension->SpinLock, irql);

    //
    // Complete the IRP.  This is a call outside the driver, so all spinlocks
    // must be released by this point.
    //
    Irp->IoStatus.Status = STATUS_CANCELLED;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    //
    // Remove the lock we took in the read handler
    //
    IoReleaseRemoveLock(&deviceExtension->RemoveLock, Irp);
}

