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
    BOOLEAN         pendit = FALSE; //仅仅为"系统关电"而设计

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
                //
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

		//分三种情况处理
		// （1）关电，调用PoRequestPowerIrp发包，弄个将返回STATUS_PENDING的标志
		//		在完成函数中：
        //		PoStartNextPowerIrp (S_Irp);
        //		IoSkipCurrentIrpStackLocation (S_Irp);//本层不再处理
        //		PoCallDriver (data->TopPort, S_Irp);//往下传递
		
		// （2）上电，暂时不做事，后面再添加完成函数，发到下层，最终会返回STATUS_PENDING
		//		在完成函数中又调用PoRequestPowerIrp发包要设备上电
		//      最终确认调用：
        //		PoStartNextPowerIrp (Irp);		
		// （3）没变化，发一个Wait Wake IRP，最后会：
		//		PoStartNextPowerIrp (Irp);
        //		IoSkipCurrentIrpStackLocation (Irp);
        //		status = PoCallDriver (data->TopPort, Irp);//真正代码在此
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

				//设置powerState.DeviceState 到底是多少
				
                if (WAITWAKE_ON (data) && // ((port)->WaitWakeIrp != 0)
                    powerState.SystemState < PowerSystemHibernate) {
                    ASSERT (powerState.SystemState >= PowerSystemWorking &&
                            powerState.SystemState < PowerSystemHibernate);

                    powerState.DeviceState =
                        data->SystemToDeviceState[powerState.SystemState];
                }
                else {
                    powerState.DeviceState = PowerDeviceD3;
                }

				//现在在派遣函数里面
				//即没有使用IoCompleteRequest 完成IRP，
				//也没有把IRP传递到下层，而是搞了个功率管理IRP把本IRP当参数发出去了，
				//要在那个完成函数中完成本IRP，在那个完成函数中也没有完成，而是转发了
				//所以就应该调用IoMarkIrpPending(Irp)
                IoMarkIrpPending(Irp);
				//allocates a power IRP and sends it to the top driver in the device stack for the specified device.
                status  = PoRequestPowerIrp (data->Self, //PDEVICE_OBJECT
                                             IRP_MN_SET_POWER, //MinorFunction
                                             powerState, //POWER_STATE             
                                             KeyboardClassPoRequestComplete,//PREQUEST_POWER_COMPLETE
                                             Irp,//Context
                                             NULL); //out，没有

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
                    pendit = TRUE; //仅仅为"系统关电"而设计
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
                // No change, but we want to make sure a wait wake irp is sent.
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
                is a race condition between PoReq completion for S Irp and
                completion of WW irp. Steps to repro this:

                S irp completes and does PoReq of D irp with completion
                routine MouseClassPoRequestComplete
                WW Irp completion fires and the following happens:
                    set data->WaitWakeIrp to NULL
                    PoReq D irp with completion routine MouseClassWWPowerUpComplete

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

	//怎么说好呢？因为什么就结束了？
	//你不处理，也不让别人处理？
	//这IRP包到底是发给谁的？没有谁主动承担？还是这么一咕噜往下溜，溜到哪算哪？
    if (!NT_SUCCESS (status)) {
        Irp->IoStatus.Status = status;
        PoStartNextPowerIrp (Irp);
        IoCompleteRequest (Irp, IO_NO_INCREMENT);

    } else if (hookit) { //只处理"Power up"的情况
        status = IoAcquireRemoveLock (&data->RemoveLock, Irp);
        ASSERT (STATUS_SUCCESS == status);
        IoCopyCurrentIrpStackLocationToNext (Irp);

        IoSetCompletionRoutine (Irp,
                                KeyboardClassPowerComplete,//通知Power+创建另一个IRP点亮键盘灯
                                NULL,
                                TRUE,
                                TRUE,
                                TRUE);
        IoMarkIrpPending(Irp); //IO Manager，我要返回STATUS_PENDING了
        PoCallDriver (data->TopPort, Irp);

        //
        // We are returning pending instead of the result from PoCallDriver because:
        // 1  we are changing the status in the completion routine
        // 2  we will not be completing this irp in the completion routine
        //
        status = STATUS_PENDING;
    }
    else if (pendit) {//仅仅为"系统关电"而设计,目的是暂时不往下传，IRP被当成参数传出去了
		
        status = STATUS_PENDING;//针对"系统关电"，因为已经通过PoRequestPowerIrp另起炉灶，把IRP通过参数传出去了
								//在PoRequestPowerIrp的自己的完成函数的最后会继续往下传，就像没有完成函数一样
    } else {
		
        PoStartNextPowerIrp (Irp);
        IoSkipCurrentIrpStackLocation (Irp);
        status = PoCallDriver (data->TopPort, Irp);

    IoReleaseRemoveLock (&data->RemoveLock, Irp);
    return status;
}

//"系统"功率的完成函数，在某个底层bus驱动完成后执行
//"上电"要整一个wait wake irp，我胡汉三回来了，都给我醒醒
//"关电"要保存关到什么程度，关电关呗，记录下
//注意该完成函数没有返回值，这个与其他完成函数不一样，就是干点事呗
VOID
KeyboardClassPoRequestComplete (
    IN PDEVICE_OBJECT DeviceObject,
    IN UCHAR MinorFunction, //未用
    IN POWER_STATE D_PowerState,//未用
    IN PIRP S_Irp, // The S irp that caused us to request the power.
    IN PIO_STATUS_BLOCK IoStatus //未用
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
        //
		//关电到什么程度？
        powerState = IoGetCurrentIrpStackLocation(S_Irp)->Parameters.Power.State;
		//A driver calls this routine after receiving a device set-power request and before 
		//calling PoStartNextPowerIrp.
		//If the device is powering down, the driver must call PoSetPowerState before leaving the D0 state
		//给Power Manager发个通知说"系统"已经处于一个新功率状态了		
        PoSetPowerState (data->Self, SystemPowerState, powerState);
        data->SystemState = powerState.SystemState;//保存起来

		//S_Irp就是原来请求电源管理的IRP，其传递的路径很有研究的必要：
		//在派遣函数中，没有继续往下传，派遣函数返回STATUS_PENDING
		//没往下传不对啊？原来他调用了另一个功率管理函数PoRequestPowerIrp另起炉灶，把IRP传递到此
		//本函数是PoRequestPowerIrp的完成函数，这里IRP按下面三行继续传下去
		
        PoStartNextPowerIrp (S_Irp);//处理其他功率管理IRP
        IoSkipCurrentIrpStackLocation (S_Irp);//本层不再处理
        PoCallDriver (data->TopPort, S_Irp);//往下传递

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

		//发送一个wait wake irp包
        if (SHOULD_SEND_WAITWAKE (data)) {
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
                status = IoAcquireRemoveLock (&data->RemoveLock, itemData);//马上要拿锁

                if (NT_SUCCESS(status)) {
					//搞个工作线程，工作任务是创建一个wait wake irp
                    IoQueueWorkItem (itemData->Item, //IoAllocateWorkItem的返回
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
/*
问题：注意到POWER_STATE结构是个union了吗？
答案：
typedef union _POWER_STATE {
  SYSTEM_POWER_STATE SystemState;
  DEVICE_POWER_STATE DeviceState;
} POWER_STATE, *PPOWER_STATE;
*/

//这个函数总返回STATUS_SUCCESS
//这个函数的目的是当"设备恢复上电"后，恢复性点亮指示灯
//都要调用PoSetPowerState给Power Manager发个通知说"系统"已经处于一个新功率状态了		

NTSTATUS
KeyboardClassPowerComplete ( //没有处理Power Down的内容
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context //未用，传来的参数为NULL
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
	
	//学习如何从IO_STACK_LOCATION取出数据
    powerType = stack->Parameters.Power.Type;
    powerState = stack->Parameters.Power.State;

	//永远不会给大师发送功率管理IRP
	//永远不会给非PNP设备发送功率管理IRP
	//所以这里验证一下
    ASSERT (data != Globals.GrandMaster);
    ASSERT (data->PnP);

    switch (stack->MinorFunction) {
    case IRP_MN_SET_POWER:
        switch (powerType) {
        case DevicePowerState:
			//保证这是增大功率（数字越小，功率越大）
            ASSERT (powerState.DeviceState < data->DeviceState);
            //
            // Powering up
            //
			//给Power Manager发个通知说"设备"已经处于一个新功率状态了
            PoSetPowerState (data->Self, DevicePowerState, powerState);
			//保存起来
            data->DeviceState = powerState.DeviceState;
			
			//下面恢复键盘灯，但是因为这是完成函数，运行的级别可能有点高。
			//这里弹出一个控制IRP来解决这个问题，不过非要在这里解决问题的话
			//应当怎么做？
			
			//驱动程序自己创建IRP的例子，不过应当自己释放内存，并且在完成函数中返回
			//STATUS_MORE_PROCESSING_REQUIRED，告诉IO Manager不要释放内存
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
                        file = stack->FileObject; //本层fileobject取出来何用？放下一层去
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

					//完成函数中又设了一个完成函数！
					//不过不是针对进来的IRP，而是在本函数中创建的IRP
					//所以也没有什么了不起
                    IoSetCompletionRoutine (irpLeds,
                                            KeyboardClassSetLedsComplete,//释放内存而已
                                            data,
                                            TRUE,
                                            TRUE,
                                            TRUE);
					//要养成先设完成函数，再设输入地址buffer的习惯
					//这其实也不一定要怎么做，非要这么做只是一种习惯
                    irpLeds->AssociatedIrp.SystemBuffer = params;

					//把新创建的点指示灯IRP控制包发出去
                    IoCallDriver (data->TopPort, irpLeds);
                }
                else {
                    IoFreeIrp (irpLeds); //正常释放
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
			
			//给Power Manager发个通知说"系统"已经处于一个新功率状态了		
            PoSetPowerState (data->Self, SystemPowerState, powerState);
			
			//保存起来
            data->SystemState = powerState.SystemState;
			
			//系统功率上来了，有必要改一下设备功率状态为D0级
            powerState.DeviceState = PowerDeviceD0;

			
			//与设备上电后自己分配IRP包不同，这里采用了一个更加方面的函数创建并发送包
			//因为功率请求IRP包的创建，设定完成函数，发送都写在一个函数中啦
			//这个函数还有一个好处，在IRP_MN_SET_POWER情况下，连IRP的生死都不用管，就当没发生过
            status = PoRequestPowerIrp (data->Self,
                                        IRP_MN_SET_POWER,
                                        powerState, //要设的功率
                                        KeyboardClassPoRequestComplete,
                                        NULL, //完成函数的参数，这次没参数
                                        NULL); //创建的IRP地址，IRP_MN_SET_POWER时必须为0
										       //就是函数返回前可能就被Free了，
											   //不需要用户再次释放了

            //
            // Propagate the error if one occurred
            //
            if (!NT_SUCCESS(status)) { //这里可能不对，上面函数会返回STATUS_PENDING代替STATUS_SUCCESS
                Irp->IoStatus.Status = status;
            }

            break;
        }
        break;

    default:
        ASSERT (0xBADBAD == stack->MinorFunction);
        break;
    }

	//每个IRP_MN_SET_POWER处理完后，必须来这么一句
	//新windows已经不用了
	//通知Power Manager继续处理下一个功率管理IRP
	//Msdn建议：As a general rule, a driver should call PoStartNextPowerIrp from its IoCompletion routine
	//现在Irp包的生命还未结束
    PoStartNextPowerIrp (Irp);
	
    IoReleaseRemoveLock (&data->RemoveLock, Irp);

    return STATUS_SUCCESS;//这个完成函数无论如何都返回成功，IRP将被IO Manager释放
}

NTSTATUS
KeyboardClassSetLedsComplete (
    IN PDEVICE_OBJECT DeviceObject,
    IN PIRP Irp,
    IN PVOID Context
    )
{
    PDEVICE_EXTENSION   data;

    UNREFERENCED_PARAMETER (DeviceObject);

    data = (PDEVICE_EXTENSION) Context;

    IoReleaseRemoveLock (&data->RemoveLock, Irp);
    IoFreeIrp (Irp);

    return STATUS_MORE_PROCESSING_REQUIRED;
}

//SHOULD_SEND_WAITWAKE宏
BOOLEAN
KeyboardClassCheckWaitWakeEnabled(
    IN PDEVICE_EXTENSION Data
    )
{
    KIRQL irql;
    BOOLEAN enabled;

    KeAcquireSpinLock (&Data->WaitWakeSpinLock, &irql);
    enabled = Data->WaitWakeEnabled;
    KeReleaseSpinLock (&Data->WaitWakeSpinLock, irql);

    return enabled;
}