//问题：想到了ioctl没有？
//问题：ioctl放在哪里了？IO_STACK_LOCATION有专门的参数块：stack->Parameters.DeviceIoControl.IoControlCode
//问题：unit放在哪儿了？在输入缓冲区内Irp->AssociatedIrp.SystemBuffer
//问题：Pass the device control request on to the port driver asynchronously，如何做到的？
//答案：关键是拷贝堆栈和设置下层栈的东西
//      IoCopyCurrentIrpStackLocationToNext (Irp);
//      (IoGetNextIrpStackLocation (Irp))->MajorFunction =
//            IRP_MJ_INTERNAL_DEVICE_CONTROL;
//问题：怎么转变为内部设备控制请求
//问题：哪几种控制需要转变为内部设备控制请求
//问题：什么情况下会循环发送(loopit=1)？
//答案：Globals.SendOutputToAllPorts==1的情况

//所有的设备控制子函数都是异步的？All device control subfunctions are passed, asynchronously
NTSTATUS
KeyboardClassDeviceControl(
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp
    )

/*++

Routine Description:

    This routine is the dispatch routine for device control requests.
    All device control subfunctions are passed, asynchronously, to the
    connected port driver for processing and completion.

Arguments:

    DeviceObject - Pointer to class device object.

    Irp - Pointer to the request packet.

Return Value:

    Status is returned.

--*/

{
    PIO_STACK_LOCATION stack;
    PDEVICE_EXTENSION deviceExtension;
    PDEVICE_EXTENSION port;
    BOOLEAN  loopit = FALSE;
    NTSTATUS status = STATUS_SUCCESS;
    PKEYBOARD_INDICATOR_PARAMETERS param;
    ULONG    unitId;
    ULONG    ioctl;
    ULONG    i;
    PKBD_CALL_ALL_PORTS callAll;

    PAGED_CODE ();

    KbdPrint((2,"KBDCLASS-KeyboardClassDeviceControl: enter\n"));

    //
    // Get a pointer to the device extension.
    //

    deviceExtension = DeviceObject->DeviceExtension;

    //
    // Get a pointer to the current parameters for this request.  The
    // information is contained in the current stack location.
    //

    stack = IoGetCurrentIrpStackLocation(Irp);

	//别让本层设备跑了
    status = IoAcquireRemoveLock (&deviceExtension->RemoveLock, Irp);
    if (!NT_SUCCESS (status)) {
		//错误
        Irp->IoStatus.Status = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return status;
    }

    //
    // Check for adequate input buffer length.  The input buffer
    // should, at a minimum, contain the unit ID specifying one of
    // the connected port devices.  If there is no input buffer (i.e.,
    // the input buffer length is zero), then we assume the unit ID
    // is zero (for backwards compatibility).
    //

	//检查至少有一个unit ID的输入内存
    unitId = 0;//假定，向后兼容
    switch (ioctl = stack->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_KEYBOARD_SET_INDICATORS: //设置三个键盘指示灯，转变为内部设备控制请求
        if (stack->Parameters.DeviceIoControl.InputBufferLength <
            sizeof (KEYBOARD_INDICATOR_PARAMETERS)) {
			//错误
            status = STATUS_BUFFER_TOO_SMALL;
            Irp->IoStatus.Status = status;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            goto KeyboardClassDeviceControlReject;
        }

        deviceExtension->IndicatorParameters //把三个键盘指示灯保存起来
            = *(PKEYBOARD_INDICATOR_PARAMETERS)Irp->AssociatedIrp.SystemBuffer;
        // Fall through
    case IOCTL_KEYBOARD_SET_TYPEMATIC: //重复击键，转变为内部设备控制请求
        if (Globals.SendOutputToAllPorts) {
            loopit = TRUE;
        }
        // Fall through
    case IOCTL_KEYBOARD_QUERY_ATTRIBUTES: //转变为内部设备控制请求
    case IOCTL_KEYBOARD_QUERY_INDICATOR_TRANSLATION: //转变为内部设备控制请求
    case IOCTL_KEYBOARD_QUERY_INDICATORS: //转变为内部设备控制请求
    case IOCTL_KEYBOARD_QUERY_TYPEMATIC: //转变为内部设备控制请求
    case IOCTL_KEYBOARD_QUERY_IME_STATUS: //查不到
    case IOCTL_KEYBOARD_SET_IME_STATUS://查不到

		//unitId来自输入buffer，是的
        if (stack->Parameters.DeviceIoControl.InputBufferLength == 0) {
            unitId = 0;//向后兼容
        } else if (stack->Parameters.DeviceIoControl.InputBufferLength <
                      sizeof(KEYBOARD_UNIT_ID_PARAMETER)) {
			//错误
            status = STATUS_BUFFER_TOO_SMALL;
            Irp->IoStatus.Status = status;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            goto KeyboardClassDeviceControlReject;

        } else {
			/*
			typedef struct _KEYBOARD_UNIT_ID_PARAMETER {
			  USHORT UnitId; //L"\\Device\\KeyboardPortN"中的N
			} KEYBOARD_UNIT_ID_PARAMETER, *PKEYBOARD_UNIT_ID_PARAMETER;
			*/
            unitId = ((PKEYBOARD_UNIT_ID_PARAMETER) 
                         Irp->AssociatedIrp.SystemBuffer)->UnitId;
        }

		//分三种情况进行处理
		//(1)本设备对象不是真设备对象——这是个错误
		//(2)本设备是大师
		//(3)本设备是大师下的带unitID的设备，比如L"\\Device\\KeyboardPortN"
        if (deviceExtension->Self != deviceExtension->TrueClassDevice) {
			//错误
            status = STATUS_NOT_SUPPORTED;
            Irp->IoStatus.Status = status;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            goto KeyboardClassDeviceControlReject;

        } else if (deviceExtension == Globals.GrandMaster) {
            ExAcquireFastMutex (&Globals.Mutex);
            if (Globals.NumAssocClass <= unitId) {
				//错误：unitId太大
                ExReleaseFastMutex (&Globals.Mutex);
                status = STATUS_INVALID_PARAMETER;
                Irp->IoStatus.Status = status;
                Irp->IoStatus.Information = 0;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                goto KeyboardClassDeviceControlReject;
            }
            if (0 < Globals.NumAssocClass) {
                if (!PORT_WORKING (&Globals.AssocClassList [unitId])) {
                    unitId = 0; //unitID设备要是没有工作的话，unitID先回到0
                }
				//找哪个unitID在工作
                while (Globals.NumAssocClass > unitId &&
                       !PORT_WORKING (&Globals.AssocClassList [unitId])) {
                    unitId++;
                }
            }
            if (Globals.NumAssocClass <= unitId) {
				//错误：查找unitID没找到
                ExReleaseFastMutex (&Globals.Mutex);
                status = STATUS_INVALID_PARAMETER;
                Irp->IoStatus.Status = status;
                Irp->IoStatus.Information = 0;
                IoCompleteRequest(Irp, IO_NO_INCREMENT);
                goto KeyboardClassDeviceControlReject;
            }
			//真正符合要求的unitID找到了，就是真正那个设备找到了
			//上面是一些容错的措施，主要防止输入参数错误
			
			//把设备扩展和文件对象取出来，并且把文件对象放到stack中
            port = Globals.AssocClassList [unitId].Port;
            stack->FileObject = Globals.AssocClassList[unitId].File;

            ExReleaseFastMutex (&Globals.Mutex);
        } else {
            loopit = FALSE;
            port = deviceExtension;
        }

        //
        // Pass the device control request on to the port driver,
        // asynchronously.  Get the next IRP stack location and copy the
        // input parameters to the next stack location.  Change the major
        // function to internal device control.
        //

		//注意是如何把设备控制请求pass on to端口驱动程序的
        IoCopyCurrentIrpStackLocationToNext (Irp);
        (IoGetNextIrpStackLocation (Irp))->MajorFunction =
            IRP_MJ_INTERNAL_DEVICE_CONTROL; //转变为内部设备控制请求

        if (loopit) {
            //
            // Inc the lock one more time until this looping is done.
            // Since we are allready holding this semiphore, it should not
            // have triggered on us.
            //
            status = IoAcquireRemoveLock (&deviceExtension->RemoveLock, Irp);
            ASSERT (NT_SUCCESS (status));

            //
            // Prepare to call multiple ports
            // Make a copy of the port array.
            //
            // If someone yanks the keyboard, while the caps lock is
            // going we could be in trouble.
            //
            // We should therefore take out remove locks on each and every
            // port device object so that it won't.
            //

			//见过拿着快速锁保护着，干着分配内存还有发IRP包的愚蠢事吗
			//好处是这个包还会自动地一个接一个地发
			//不过不会等待所有的包完成才放弃锁
			//问题：下面的Globals.Mutex到底保护什么？
			//答案：Globals.GrandMaster和Globals.AssocClassList？
            ExAcquireFastMutex (&Globals.Mutex);
            callAll = ExAllocatePool (NonPagedPool,
                                      sizeof (KBD_CALL_ALL_PORTS) +
                                      (sizeof (PORT) * Globals.NumAssocClass));

			//完全填充数据结构，还不忘拿个锁
            if (callAll) {
                callAll->Len = Globals.NumAssocClass;
                callAll->Current = 0;
				
                for (i = 0; i < Globals.NumAssocClass; i++) {

                    callAll->Port[i] = Globals.AssocClassList[i];
					//如果设备正在工作，拿一个锁，免得设备跑了
                    if (PORT_WORKING (&callAll->Port[i])) {
                        status = IoAcquireRemoveLock (
                                     &(callAll->Port[i].Port)->RemoveLock,
                                    Irp);
                        ASSERT (NT_SUCCESS (status));
                    }
                }
				//执行这个特别的"弹"函数，这个函数会发出IRP包，而且个自动续杯
                status = KeyboardCallAllPorts (DeviceObject, Irp, callAll);
				//到这儿，只能保证发了一个IRP控制包

            } else {
				//错误：内存不足
                status = STATUS_INSUFFICIENT_RESOURCES;
                Irp->IoStatus.Status = status;
                Irp->IoStatus.Information = 0;
                IoCompleteRequest (Irp, IO_NO_INCREMENT);
            }
            ExReleaseFastMutex (&Globals.Mutex);


        } else {
			//不循环的情况，loopit=0
            status = IoCallDriver(port->TopPort, Irp);
        }
        break;

	//以下是不转变为内部设备控制请求的19个请求，这些请求不是kbdclass要处理的
    case IOCTL_GET_SYS_BUTTON_CAPS:
    case IOCTL_GET_SYS_BUTTON_EVENT:
    case IOCTL_HID_GET_DRIVER_CONFIG:
    case IOCTL_HID_SET_DRIVER_CONFIG:
    case IOCTL_HID_GET_POLL_FREQUENCY_MSEC:
    case IOCTL_HID_SET_POLL_FREQUENCY_MSEC:
    case IOCTL_GET_NUM_DEVICE_INPUT_BUFFERS:
    case IOCTL_SET_NUM_DEVICE_INPUT_BUFFERS:
    case IOCTL_HID_GET_COLLECTION_INFORMATION:
    case IOCTL_HID_GET_COLLECTION_DESCRIPTOR:
    case IOCTL_HID_FLUSH_QUEUE:
    case IOCTL_HID_SET_FEATURE:
    case IOCTL_HID_GET_FEATURE:
    case IOCTL_GET_PHYSICAL_DESCRIPTOR:
    case IOCTL_HID_GET_HARDWARE_ID:
    case IOCTL_HID_GET_MANUFACTURER_STRING:
    case IOCTL_HID_GET_PRODUCT_STRING:
    case IOCTL_HID_GET_SERIALNUMBER_STRING:
    case IOCTL_HID_GET_INDEXED_STRING:
		//如果本层设备是pnp的，并且不是大师，正确
		//1.不是pnp的，通过本层发控制不行，给大师发送不了，因为大师不好意思也不是pnp
		//2.只要本层是pnp的且不是大师，就能处理，这个处理就是转发irp，那么问题来了，谁不能转发？
        if (deviceExtension->PnP && (deviceExtension != Globals.GrandMaster)) {
            IoSkipCurrentIrpStackLocation (Irp);//本层不处理，交给其他层
            status = IoCallDriver (deviceExtension->TopPort, Irp);
            break;
        }
    default:
		//错误
        status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Status = status;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        break;
    }

KeyboardClassDeviceControlReject:

	//现在设备跑了也没事
    IoReleaseRemoveLock (&deviceExtension->RemoveLock, Irp);

    KbdPrint((2,"KBDCLASS-KeyboardClassDeviceControl: exit\n"));

    return(status);

}

//这个函数有三个出口，第一个为普通函数准备，第二个和第三个为完成函数准备，完成函数的
//两个可能的返回值都有了，就是说完成函数所有可能的出口都具备了
//这个函数能当普通函数使用，也能当完成函数使用
//这个函数自己调用自己，严格的说不是调用，而是预设，通过完成函数的方式
//这个函数能死而复生，按照STATUS_MORE_PROCESSING_REQUIRED的方式生，最终会以完成函数STATUS_SUCCESS的方式死掉
//这个函数生死的判断种子在参数CallALL中，每次都要把他当参数传递进来，不传进来的话使用全局变量也不知道行不行
//这个函数用来研究完成函数很有意义
//这个函数作为完成函数，在退出时不发送一个EVENT，所以会检查IRP的 PendingReturned标志，如果为真则调用IoMarkIrpPending标记IRP为pending状态，否则IRP...

NTSTATUS
KeyboardCallAllPorts (
   PDEVICE_OBJECT       Device,
   PIRP                 Irp,
   PKBD_CALL_ALL_PORTS  CallAll
   )
/*++
Routine Description:
    Bounce this Irp to all the ports associated with the given device extension.

--*/
{
    PIO_STACK_LOCATION  nextSp;
    NTSTATUS            status;
    PDEVICE_EXTENSION   port;
    BOOLEAN             firstTime;

    //firstTime关系到从哪儿退出的问题
	//(1)从KeyboardClassDeviceControl退出
	//(2)从完成函数中退出，完成函数还有两个退出，一个是最后正常退出，一个是中间还要处理
	firstTime = CallAll->Current == 0; 
	
	//只有大师才会把IRP弹到所有的端口，验证一下
    ASSERT (Globals.GrandMaster->Self == Device);

	//给下层准备堆栈，并修改其功能码（MajorFunction）
    nextSp = IoGetNextIrpStackLocation (Irp);
    IoCopyCurrentIrpStackLocationToNext (Irp);
    nextSp->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;

	//跳过没有工作的port设备，一点都没有什么不方便的
    while ((CallAll->Current < CallAll->Len) &&
           (!PORT_WORKING (&CallAll->Port[CallAll->Current]))) {
        CallAll->Current++;
    }
	
	//当前要发送的端口设备找到了，但还是要准备一个退出条件
    if (CallAll->Current < CallAll->Len) {

		//取出端口设备
        port = CallAll->Port [CallAll->Current].Port;
		
		//设置IO_STACK_LOCATION的FileObject，文件是关键
        nextSp->FileObject = CallAll->Port [CallAll->Current].File;

        CallAll->Current++;

		//把IRP发出去前还有一步：把完成函数设置成本函数，作用是让本函数死而复生，又获得执行任务的能力
        IoSetCompletionRoutine (Irp,
                                &KeyboardCallAllPorts,//CompletionRoutine
                                CallAll, //Context，每次都在变，特别是current
                                TRUE,  //InvokeOnSuccess，观察下面的设置，任何情况都要执行完成函数
                                TRUE,  //InvokeOnError
                                TRUE); //InvokeOnCancel
		//把IRP发出去~~~
        status = IoCallDriver (port->TopPort, Irp);
        IoReleaseRemoveLock (&port->RemoveLock, Irp);//那个锁可以释放了，在另一个函数中把所有设备的锁都拿了

    } else {
        //
        // We are done so let this Irp complete normally
        //
		//大师本身没有fileobject，大师下面的小弟有fileobject
		//Device每次都传回来，大师每次都回来
        ASSERT (Globals.GrandMaster == Device->DeviceExtension);

		//别忘记我们在完成函数中，我们不会发出一个event了，按照微软要求，必须检查
		//Checking the PendingReturned Flag
        if (Irp->PendingReturned) {
            IoMarkIrpPending (Irp);
        }

		//在大师身上也拿了个锁，现在可以放了
        IoReleaseRemoveLock (&Globals.GrandMaster->RemoveLock, Irp);
        ExFreePool (CallAll);//没有用了
        return STATUS_SUCCESS; //这个出口是为完成函数的目的而准备
    }

    if (firstTime) {
        //
        // Here we are not completing an IRP but sending it down for the first
        // time.
        //
        return status; //这个出口不是为完成函数的目的而准备的
    }

    //
    // Since we bounced the Irp another time we must stop the completion on
    // this particular trip.
    //
	//这个出口是为完成函数的目的而准备
	//完成函数中中间退出，还要继续使用此IRP发包不得完成
    return STATUS_MORE_PROCESSING_REQUIRED; //STATUS_MORE_PROCESSING_REQUIRED halts the I/O Manager's completion processing on the IRP.
}

