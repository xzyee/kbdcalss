//The operating system sends an IRP_MJ_CREATE request to open a handle to a file object or device object.
//问题：这里的Irp是什么？
//本函数实质上是调用KbdEnableDisablePort函数，实质上向下面的PDO转发IRP并等待完成，转发的IRP可能经过改动
//If the routine succeeds, it must return STATUS_SUCCESS,所以下面的函数需要等待IRP完成
//一般处理原则：Most device and intermediate drivers set STATUS_SUCCESS in the I/O status block of the IRP
//              and complete the create request, but drivers can optionally use their DispatchCreate routine 
//              to reserve resources for any subsequent I/O requests for that handle.
//是否可以这样理解：这是给驱动一个机会，好干点什么

//问题：fileobject在驱动里起什么用？
//答案：用的时候，放在IO_STACK_LOCATION->FileObject中
//问题：fileobject如何获得的？
//答案：随着IRP传过来的，具体地在IO_STACK_LOCATION就有，即irpSp->FileObject
//问题：对fileobject做了那些改动？
//答案：当确信打开文件或者设备的子系统是可以信任的，在fileobject的一个字段设一个标记，以后方便用
//问题：1:1映射时，fileobject有什么用？
//答案：没用
//问题：1:m映射时，fileobject有什么用？
//答案：自行管理计数
//问题：为什么要找出fileobject？
//答案：首先说明，只有在1：m映射的情况下才会通过一些手段找出fileobject，这样可以管理引用计数

NTSTATUS
KeyboardClassCreate (
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
    BOOLEAN      someEnableDisableSucceeded = FALSE;
    BOOLEAN      enabled;

    KbdPrint((2,"KBDCLASS-KeyboardClassCreate: enter\n"));

    //
    // Get a pointer to the device extension.
    //

    deviceExtension = DeviceObject->DeviceExtension;

    //
    // Get a pointer to the current parameters for this request.  The
    // information is contained in the current stack location.
    // 信息保存在当前栈位置（io_stack_location）中

    irpSp = IoGetCurrentIrpStackLocation(Irp);
    ASSERT (IRP_MJ_CREATE == irpSp->MajorFunction);

    //
    // We do not allow user mode opens for read.  This includes services (who
    // have the TCB privilege).
    // Irp->RequestorMode很重要，这是一个使用例子，学习一下
	// 下面是open权限检查，让不让打开，用户不一定打得开设备，不让打开的情况下直接就完成了
    if (Irp->RequestorMode == UserMode &&
        (irpSp->Parameters.Create.SecurityContext->DesiredAccess & FILE_READ_DATA)
        ) {
        status = STATUS_ACCESS_DENIED;
        goto KeyboardClassCreateEnd;
    }

	//请求暂时不要删除设备对象
    status = IoAcquireRemoveLock (&deviceExtension->RemoveLock, Irp);

    if (!NT_SUCCESS (status)) {
        goto KeyboardClassCreateEnd;
    }

	//问题：deviceExtension->PnP在哪里被设置？在AddDevice子程序中
	//问题：deviceExtension->Started在哪里被设置？在PNP处理IRP_MN_START_DEVICE
	//下面可以理解为设备未添加FDO并且FDO未启动？为什么是相与的关系？
    if ((deviceExtension->PnP) && (!deviceExtension->Started)) {
        KbdPrint((
            1,
            "KBDCLASS-Create: failed create because PnP and Not started\n"
             ));

        status = STATUS_UNSUCCESSFUL;
        IoReleaseRemoveLock (&deviceExtension->RemoveLock, Irp);
        goto KeyboardClassCreateEnd;
    }


    //
    // For the create/open operation, send a KEYBOARD_ENABLE internal
    // device control request to the port driver to enable interrupts.
    //
	//真正的keyboead正在被打开...
	//注意：大师或者1:1都能使下面等式成立
	//下面是read权限检查：虽然打得开设备对象，当时将来不一定能读出来，还是权限问题
    if (deviceExtension->Self == deviceExtension->TrueClassDevice) {
        //
        // The real keyboard is being opened.  This either represents the
        // Grand Master, if one exists, or the individual keyboards objects,
        // if all for one is not set.  (IE "KeyboardClassX")
        //
        // First, if the requestor is the trusted subsystem (the single
        // reader), reset the the cleanup indicator and place a pointer
        // to the file object which this class driver uses
        // to determine if the requestor has sufficient
        // privilege to perform the read operation).
        //
		//为什么要fileobject？类驱动程序用来确定请求者是否有足够的权利来执行读取操作
		//什么是可信任的子系统？与单个读者什么关系？
		//另外两个参数没有检查：(1)Parameters.Create.Options;(2)Parameters.Create.ShareAccess
		
        priv = RtlConvertLongToLuid(SE_TCB_PRIVILEGE);//要求可信任子系统才能Create

        if (SeSinglePrivilegeCheck(priv, Irp->RequestorMode)) { //检查IRP权限的说法不准确，但就是那个意思吧

            KeAcquireSpinLock(&deviceExtension->SpinLock, &oldIrql);

            ASSERT (!IS_TRUSTED_FILE_FOR_READ (irpSp->FileObject));//当前fileobject的某个字段为0，因为还没设置
            SET_TRUSTED_FILE_FOR_READ (irpSp->FileObject); //做个记号
            deviceExtension->TrustedSubsystemCount++; //某个操作系统的子系统要Create，是可信的

            KeReleaseSpinLock(&deviceExtension->SpinLock, oldIrql);
        }
    }

    //
    // Pass on enables for opens to the true class device
    //
    ExAcquireFastMutex (&Globals.Mutex);
	
	//分几种情况：(1)我是1:m的大师FDO，那么为大师映射的下面所有的设备端口（PDO）分别执行使能任务
	//            (2)我是1:1的FDO，对唯一的设备端口（PDO）发个控制
	//            (3)open从属fdo，不是open我，和我没关系往下传
	
	//大师只会执行一次：如果Globals.Opens==0才会执行，如果不等于0说明打开过了，首次创建才会执行下面这一段
	//原因很简单：会对所有映射的设备都执行一下某功能
    if ((Globals.GrandMaster == deviceExtension) && (1 == ++Globals.Opens)) {

        for (i = 0; i < Globals.NumAssocClass; i++) {
            port = &Globals.AssocClassList[i];

			//port要素：File，真正的Port，Enabled标志，Free标志
            if (port->Free) {
                continue;
            }

            enabled = port->Enabled; //保存起来，以便快速退出fastmutex的保护区
            port->Enabled = TRUE; //设置为真，允许端口使能
            ExReleaseFastMutex (&Globals.Mutex);

			//现在可以慢慢处理port->Enabled
			//如果还没有使能的话，使能之，因为我们在Create中
            if (!enabled) {
                status = KbdEnableDisablePort(TRUE,//使能
                                              Irp,
                                              port->Port,//真正的Port
                                              &port->File); //输出，fileobject保存在这里
            }

            if (!NT_SUCCESS(status)) {

                KbdPrint((0,
                          "KBDCLASS-KeyboardClassOpenClose: Could not enable/disable interrupts for port device object @ 0x%x\n",
                          port->Port->TopPort));

                KeyboardClassLogError (DeviceObject,
                                       KBDCLASS_PORT_INTERRUPTS_NOT_ENABLED,
                                       KEYBOARD_ERROR_VALUE_BASE + 120,
                                       status,
                                       0,
                                       NULL,
                                       irpSp->MajorFunction);

                port->Enabled = FALSE; //回到未使能的状态，为什么这个时候不用fastmutex？
            }
            else {
                someEnableDisableSucceeded = TRUE;
            }
			//为下一个循环获取fastmutex
            ExAcquireFastMutex (&Globals.Mutex);
        }
        ExReleaseFastMutex (&Globals.Mutex);

    } else if (Globals.GrandMaster != deviceExtension) {
        ExReleaseFastMutex (&Globals.Mutex);

        if (deviceExtension->TrueClassDevice == DeviceObject) { //如果是孪生关系，如果是FDO
            //
            // An open to the true class Device => enable the one and only port
            //

            status = KbdEnableDisablePort (TRUE,//使能
                                           Irp,
                                           deviceExtension,
                                           &irpSp->FileObject);//虽然是输出，但确未改变
										                       //这个参数在这种情况下压根没用过

        } else {
            //
            // A subordinant下级的 FDO.  They are not their own TrueClassDeviceObject.
            // Therefore pass the create straight on through.
            //
            IoSkipCurrentIrpStackLocation (Irp);
            status = IoCallDriver (deviceExtension->TopPort, Irp);
            IoReleaseRemoveLock (&deviceExtension->RemoveLock, Irp);
            return status;
        }

        if (!NT_SUCCESS(status)) {

            KbdPrint((0,
                      "KBDCLASS-KeyboardClassOpenClose: Create failed (0x%x) port device object @ 0x%x\n",
                      status, deviceExtension->TopPort));
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

    IoReleaseRemoveLock (&deviceExtension->RemoveLock, Irp);

KeyboardClassCreateEnd:
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    KbdPrint((2,"KBDCLASS-KeyboardClassOpenClose: exit\n"));
    return(status);
}


//在很多地方都会调用这个函数：IRP_MJ_CREATE、IRP_MJ_PNP、KeyboardClassPlugPlayNotification
//第四个参数的IN用错了，改为INOUT

//本函数在1:1映射的情况下，原封不动直接往下传
//本函数在传统设备的情况下，发送一个IOCTL_INTERNAL_KEYBOARD_ENABLE或者IOCTL_INTERNAL_KEYBOARD_DIUSABLE的控制包
//本函数在1:m映射的情况下，发送一个向PDO发IOCTL_KEYBOARD_SET_INDICATORS的IRP的控制包，找出fileobject好进行引用计数，注册一个回调

//注意本函数既服务于创建(open)，也服务于关闭(close)
NTSTATUS
KbdEnableDisablePort(
    IN BOOLEAN EnableFlag,//使能或不使能
    IN PIRP    Irp,
    IN PDEVICE_EXTENSION Port,
    INOUT PFILE_OBJECT * File //Pointer to the file object that represents the corresponding device object to user-mode code
    )

/*++

Routine Description:

    This routine sends an enable or a disable request to the port driver.
    The legacy port drivers require an enable or disable ioctl, while the
    plug and play drivers require merely a create.

Arguments:

    DeviceObject - Pointer to class device object.

    EnableFlag - If TRUE, send an ENABLE request; otherwise, send DISABLE.

    PortIndex - Index into the PortDeviceObjectList[] for the current
        enable/disable request.

Return Value:

    Status is returned.

--*/

{
    IO_STATUS_BLOCK ioStatus;
    UNICODE_STRING  name = {0,0,0};
    PDEVICE_OBJECT  device = NULL;
    NTSTATUS    status = STATUS_SUCCESS;
    PWCHAR      buffer = NULL;
    ULONG       bufferLength = 0;
    PIO_STACK_LOCATION stack;

    PAGED_CODE ();

    KbdPrint((2,"KBDCLASS-KbdEnableDisablePort: enter\n"));

    //
    // Create notification event object to be used to signal the
    // request completion.
    //

	//下面的port实际是PDEVICE_EXTENSION
	//分三种情况处理：
	//(1) pnp的，1:1映射情况下
	//(2) 非pnp的，向PDO发IOCTL_INTERNAL_KEYBOARD_ENABLE或者IOCTL_INTERNAL_KEYBOARD_DISABLE的IRP并等待完成
	//(3) pnp的，1:m映射情况下，向PDO发IOCTL_KEYBOARD_SET_INDICATORS的IRP并等待完成，还要找出fileobject存起来
    if ((Port->TrueClassDevice == Port->Self) && (Port->PnP)) {

        IoCopyCurrentIrpStackLocationToNext (Irp);
        stack = IoGetNextIrpStackLocation (Irp);

		//下面搞对了stack->MajorFunction，不是IRP_MJ_CREATE就是IRP_MJ_CLOSE
        if (EnableFlag) { //想使能？
            //
            // Since there is no grand master there could not have been a
            // create file against the FDO before it was started.  Therefore
            // the only time we would enable is during a create and not a
            // start as we might with another FDO attached to an already open
            // grand master.
            //
            ASSERT (IRP_MJ_CREATE == stack->MajorFunction);

        } else {
			//改stack->MajorFunction
            if (IRP_MJ_CLOSE != stack->MajorFunction) {
                //
                // We are disabling.  This could be because the device was
                // closed, or because the device was removed out from
                // underneath us.
                //
                ASSERT (IRP_MJ_PNP == stack->MajorFunction);
                ASSERT ((IRP_MN_REMOVE_DEVICE == stack->MinorFunction) ||
                        (IRP_MN_STOP_DEVICE == stack->MinorFunction));
                stack->MajorFunction = IRP_MJ_CLOSE; //这种篡改合法吗？
            }
        }

        //
        // Either way we need only pass the Irp down without mucking施粪肥于 with the
        // file object.
        //
		//这种情况最省事，把IRP往下传，不过我们要等包被处理后才继续下去
		//问题是怎么做到的？通过下面的函数
        status = KeyboardSendIrpSynchronously (Port->TopPort, Irp, FALSE);

    } else if (!Port->PnP) {
		
		//传统那种，向PDO发送为IRP_MJ_INTERNAL_DEVICE_CONTROL同步包完事
		
        Port->Enabled = EnableFlag;

        //
        // We have here an old style Port Object.  Therefore we send it the
        // old style internal IOCTLs of ENABLE and DISABLE, and not the new
        // style of passing on a create and close.
        //
        IoCopyCurrentIrpStackLocationToNext (Irp);
        stack = IoGetNextIrpStackLocation (Irp);

        stack->Parameters.DeviceIoControl.OutputBufferLength = 0;
        stack->Parameters.DeviceIoControl.InputBufferLength = 0;
        stack->Parameters.DeviceIoControl.IoControlCode
            = EnableFlag ? IOCTL_INTERNAL_KEYBOARD_ENABLE
                         : IOCTL_INTERNAL_KEYBOARD_DISABLE;
        stack->Parameters.DeviceIoControl.Type3InputBuffer = NULL;
        stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;

        status = KeyboardSendIrpSynchronously (Port->TopPort, Irp, FALSE);

    } else {
		
		//大师映射的无名设备
        //
        // We are dealing with a plug and play port and we have a Grand
        // Master.
        //
        ASSERT (Port->TrueClassDevice == Globals.GrandMaster->Self);

        //
        // Therefore we need to substitute the given file object for a new
        // one for use with each individual ports.
        // For enable, we need to create this file object against the given
        // port and then hand it back in the File parameter, or for disable,
        // deref the File parameter and free that file object.
        //
        // Of course, there must be storage for a file pointer pointed to by
        // the File parameter.
        //
        ASSERT (NULL != File);

        if (EnableFlag) {

            ASSERT (NULL == *File); //File等待输出呢

            //
            // The following long list of rigamaroll translates into
            // sending the lower driver a create file IRP and creating a
            // NEW file object disjoint from the one given us in our create
            // file routine.
            //
            // Normally we would just pass down the Create IRP we were
            // given, but since we do not have a one to one correspondance of
            // top device objects and port device objects.
            // This means we need more file objects: one for each of the
            // miriad of lower DOs.
            //

            bufferLength = 0;
			//第一次调用，找出bufferLength
            status = IoGetDeviceProperty (
                             Port->PDO,
                             DevicePropertyPhysicalDeviceObjectName,
                             bufferLength,
                             buffer, // _Out_opt_ PVOID
                             &bufferLength); //_Out_     PULONG 
            ASSERT (STATUS_BUFFER_TOO_SMALL == status);

            buffer = ExAllocatePool (PagedPool, bufferLength);

            if (NULL == buffer) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }

			//第二次调用，找出information about a device，ObjectName
            status =  IoGetDeviceProperty (
                          Port->PDO,
                          DevicePropertyPhysicalDeviceObjectName,
                          bufferLength,
                          buffer, //ObjectName
                          &bufferLength);

            name.MaximumLength = (USHORT) bufferLength;
            name.Length = (USHORT) bufferLength - sizeof (UNICODE_NULL);
            name.Buffer = buffer;

			// 目前，*File = NULL
            status = IoGetDeviceObjectPointer (&name, //ObjectName,输入
                                               FILE_ALL_ACCESS,//希望得到的权限
                                               File, //输出，FileObject
                                               &device); //输出，DeviceObject，除了对比外没什么用
            ExFreePool (buffer);
            //
            // Note, that this create will first go to ourselves since we
            // are attached to this PDO stack.  Therefore two things are
            // noteworthy.  This driver will receive another Create IRP
            // (with a different file object) (not to the grand master but
            // to one of the subordenant下级的 FDO's).  The device object returned
            // will be the subordenant FDO, which in this case is the "self"
            // device object of this Port.
            //
            if (NT_SUCCESS (status)) {
                PVOID   tmpBuffer;

                ASSERT (device == Port->Self); //必然

                if (NULL != Irp) { //这种情况可以借用IRP
                    //
                    // Set the indicators for this port device object.
                    // NB: The Grandmaster's device extension is initialized to
                    // zero, and the flags for indicator lights are flags, so
                    // this means that unless the RIUT has set the flags that
                    // IndicatorParameters will have no lights set.
                    //

					//向PDO发一个IRP_MJ_INTERNAL_DEVICE_CONTROL同步包，设置键盘灯
                    IoCopyCurrentIrpStackLocationToNext (Irp);
                    stack = IoGetNextIrpStackLocation (Irp);

                    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
					
					//下面设置DeviceIoControl结构，都设全了
                    stack->Parameters.DeviceIoControl.OutputBufferLength = 0;
                    stack->Parameters.DeviceIoControl.InputBufferLength =
                        sizeof (KEYBOARD_INDICATOR_PARAMETERS);
                    stack->Parameters.DeviceIoControl.IoControlCode =
                        IOCTL_KEYBOARD_SET_INDICATORS;
                    stack->FileObject = *File; //用上了

                    tmpBuffer = Irp->AssociatedIrp.SystemBuffer;//腾地方

                    Irp->AssociatedIrp.SystemBuffer = //输入输出地址
                        & Globals.GrandMaster->IndicatorParameters; 

                    status = KeyboardSendIrpSynchronously (Port->TopPort, Irp, FALSE);

                    Irp->AssociatedIrp.SystemBuffer = tmpBuffer;//放回去
                }

                //
                // Register for Target device removal events
                // 使用IoRegisterPlugPlayNotification注册回调，只有1：m映射的情况才会注册
                ASSERT (NULL == Port->TargetNotifyHandle);
                status = IoRegisterPlugPlayNotification (
                             EventCategoryTargetDeviceChange, //枚举:PnP events=当设备改变时
                             0, // No flags
                             *File, //输入的额外信息
                             Port->Self->DriverObject,
                             KeyboardClassPlugPlayNotification, //CallbackRoutine
                             Port,                              //Context
                             &Port->TargetNotifyHandle); //保存起来，用于将来反注册
            }

        } else {
            //
            // Getting rid of the handle is easy.  Just deref the file.
            //
            ObDereferenceObject (*File);
            *File = NULL;
        }

    }
    KbdPrint((2,"KBDCLASS-KbdEnableDisablePort: exit\n"));

    return (status);
}
