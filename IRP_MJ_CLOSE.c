//派发之间干点事情，主要和设备扩展有关
NTSTATUS
KeyboardClassClose (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )

/*++

Routine Description:

    This routine is the dispatch routine for create/open and close requests.
    Open/close requests are completed here.

Arguments:

    DeviceObject - Pointer to class device object.

    Irp - Pointer to the request packet.

Return Value:

    Status is returned.

--*/

{
    PIO_STACK_LOCATION   irpSp;
    PDEVICE_EXTENSION    deviceExtension;
    PPORT        port;
    KIRQL        oldIrql;
    NTSTATUS     status = STATUS_SUCCESS;
    ULONG        i;
    LUID         priv;
    KEVENT       event;
    PFILE_OBJECT file;
    BOOLEAN      someEnableDisableSucceeded = FALSE;
    BOOLEAN      enabled;
    PVOID        notifyHandle;

    KbdPrint((2,"KBDCLASS-KeyboardClassClose: enter\n"));

    //
    // Get a pointer to the device extension.
    //

    deviceExtension = DeviceObject->DeviceExtension;

    //
    // Get a pointer to the current parameters for this request.  The
    // information is contained in the current stack location.
    //

    irpSp = IoGetCurrentIrpStackLocation(Irp);

    //
    // Let the close go through even if the device is removed
    // AKA do not call KbdIncIoCount
    //

    //
    // For the create/open operation, send a KEYBOARD_ENABLE internal
    // device control request to the port driver to enable interrupts.
    //

    ASSERT (IRP_MJ_CLOSE == irpSp->MajorFunction);
	//必须是真设备才行，如果是一个无名的设备就不行，有大师管着呢
    if (deviceExtension->Self == deviceExtension->TrueClassDevice) {

        KeAcquireSpinLock(&deviceExtension->SpinLock, &oldIrql);
        if (IS_TRUSTED_FILE_FOR_READ (irpSp->FileObject)) {
            ASSERT(0 < deviceExtension->TrustedSubsystemCount);
            deviceExtension->TrustedSubsystemCount--;
            CLEAR_TRUSTED_FILE_FOR_READ (irpSp->FileObject);
        }
        KeReleaseSpinLock(&deviceExtension->SpinLock, oldIrql);
    }

    //
    // Pass on enables for closes to the true class device
    //
    ExAcquireFastMutex (&Globals.Mutex);
    if ((Globals.GrandMaster == deviceExtension) && (0 == --Globals.Opens)) {
	//本层碰巧是大师，而且大师管理的设备的打开次数减到0了
	//没关闭的设备对象（在AssocClassList里面登记的）全部关了，结果是反注册notify，然后文件引用减去一个
	//已经关闭的设备对象无事可做
        for (i = 0; i < Globals.NumAssocClass; i++) {
            port = &Globals.AssocClassList[i];

            if (port->Free) {
                continue;
            }

            enabled = port->Enabled; //在保护区里面别磨蹭，暂存后面慢慢弄
            port->Enabled = FALSE; //标志设置成关闭
            ExReleaseFastMutex (&Globals.Mutex);

            if (enabled) {
				
				//把notify回调反注册
                notifyHandle = InterlockedExchangePointer (
                                    &port->Port->TargetNotifyHandle,
                                    NULL);

                if (NULL != notifyHandle) {
                    IoUnregisterPlugPlayNotification (notifyHandle);
                }
				
				//真正的关闭
                status = KbdEnableDisablePort(FALSE,
                                              Irp,
                                              port->Port,
                                              &port->File); //会减去一个引用
            } else {
                ASSERT (NULL == port->Port->TargetNotifyHandle);
            }

            if (!NT_SUCCESS(status)) {

                KbdPrint((0,
                          "KBDCLASS-KeyboardClassOpenClose: Could not enable/disable interrupts for port device object @ 0x%x\n",
                          port->Port->TopPort));

                //
                // Log an error.
                //
                KeyboardClassLogError (DeviceObject,
                                       KBDCLASS_PORT_INTERRUPTS_NOT_DISABLED,
                                       KEYBOARD_ERROR_VALUE_BASE + 120,
                                       status,
                                       0,
                                       NULL,
                                       0);
            }
            else {
                someEnableDisableSucceeded = TRUE;
            }
            ExAcquireFastMutex (&Globals.Mutex);
        }
        ExReleaseFastMutex (&Globals.Mutex);

    } else if (Globals.GrandMaster != deviceExtension) {
        ExReleaseFastMutex (&Globals.Mutex);

        if (deviceExtension->TrueClassDevice == DeviceObject) {
            //
            // A close to the true class Device => disable the one and only port
            //

            status = KbdEnableDisablePort (FALSE,
                                           Irp,
                                           deviceExtension,
                                           &irpSp->FileObject);//也会减去一个引用

        } else {
			//根本不关本设备层的事情
            IoSkipCurrentIrpStackLocation (Irp);
            status = IoCallDriver (deviceExtension->TopPort, Irp);
            return status;
        }

        if (!NT_SUCCESS(status)) {

            KbdPrint((0,
                      "KBDCLASS-KeyboardClassOpenClose: Could not enable/disable interrupts for port device object @ 0x%x\n",
                      deviceExtension->TopPort));

            //
            // Log an error.
            //
            KeyboardClassLogError (DeviceObject,
                                   KBDCLASS_PORT_INTERRUPTS_NOT_DISABLED,
                                   KEYBOARD_ERROR_VALUE_BASE + 120,
                                   status,
                                   0,
                                   NULL,
                                   irpSp->MajorFunction);
        }
        else {
            someEnableDisableSucceeded = TRUE;
        }
    } else {
        ExReleaseFastMutex (&Globals.Mutex);
    }

    //
    // Complete the request and return status.
    //
    // NOTE: We complete the request successfully if any one of the
    //       connected port devices successfully handled the request.
    //       The RIT only knows about one pointing device.
    //

    if (someEnableDisableSucceeded) {
        status = STATUS_SUCCESS;
    }

	//在本层派发对象中就完结了
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    KbdPrint((2,"KBDCLASS-KeyboardClassOpenClose: exit\n"));
    return(status);
}