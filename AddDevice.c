NTSTATUS
KeyboardAddDevice(
    IN PDRIVER_OBJECT DriverObject,
    IN PDEVICE_OBJECT PhysicalDeviceObject //PDO
    )
/*++
Description:
    The plug play entry point "AddDevice"

--*/
{
    NTSTATUS            status;
    PDEVICE_OBJECT      fdo;
    PDEVICE_EXTENSION   port;
    PWCHAR              fullClassName = NULL;
    POWER_STATE         state;
    HANDLE              hService, hParameters;
    ULONG               tmp;
    OBJECT_ATTRIBUTES   oa;

    PAGED_CODE ();

	//现在只有驱动对象和PDO对象，要创建FDO对象
    status = KbdCreateClassObject (DriverObject,//本函数的输入参数
                                   &Globals.InitExtension,//输入
                                   &fdo, //输出，PDEVICE_OBJECT
                                   &fullClassName, //输出，FullDeviceName
                                   FALSE);

    if (!NT_SUCCESS (status)) {
        return status;
    }

	//创建的FDO不完整，修改之
	//其实不是FDO对象不完整，而是驱动自定义结构DeviceExtension不完整，没这个FDO也不能工作
	//这里的port都是FDO的设备扩展，代表FDO不？完全代表，他们是孪生的
	//其实port在上面那个函数中已经设置的差不多了，有些关于设备堆栈的信息那里设置不了，这里继续设置
	
	
    port = (PDEVICE_EXTENSION) fdo->DeviceExtension;
	
	//把设备对象贴到设备堆栈的最高层
	//port->TopPort实际指向本FDO的下层设备！
    port->TopPort = IoAttachDeviceToDeviceStack (fdo, PhysicalDeviceObject);//应当是fdo盖在PhysicalDeviceObject上面

	//不可能发生的事情，可以不看下面这一段
    if (port->TopPort == NULL) {
        PIO_ERROR_LOG_PACKET errorLogEntry;

        //
        // Not good; in only extreme cases will this fail
        //
        errorLogEntry = (PIO_ERROR_LOG_PACKET)
            IoAllocateErrorLogEntry (DriverObject,
                                     (UCHAR) sizeof(IO_ERROR_LOG_PACKET));

        if (errorLogEntry) {
            errorLogEntry->ErrorCode = KBDCLASS_ATTACH_DEVICE_FAILED;
            errorLogEntry->DumpDataSize = 0;
            errorLogEntry->SequenceNumber = 0;
            errorLogEntry->MajorFunctionCode = 0;
            errorLogEntry->IoControlCode = 0;
            errorLogEntry->RetryCount = 0;
            errorLogEntry->UniqueErrorValue = 0;
            errorLogEntry->FinalStatus =  STATUS_DEVICE_NOT_CONNECTED;

            IoWriteErrorLogEntry (errorLogEntry);
        }

        IoDeleteDevice (fdo);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

	//继续保存DeviceExtension
	//DeviceExtension所指的PDO是下层的pdo
    port->PDO = PhysicalDeviceObject;
	//DeviceExtension的pnp类型
    port->PnP = TRUE; //从AddDevice进来的一定是PNP，这个不要有疑问
    port->Started = FALSE;
    port->DeviceState = PowerDeviceD0;
    port->SystemState = PowerSystemWorking;

	
	//设置功率状态？
	//state目前完全是杂乱东西
    state.DeviceState = PowerDeviceD0;
	//居然现在就可以把设备启动到全功率状态了
    PoSetPowerState (fdo, DevicePowerState/*power type*/, state);

	//继续保存DeviceExtension
    port->MinDeviceWakeState = PowerDeviceUnspecified;
    port->MinSystemWakeState = PowerSystemUnspecified;
    port->WaitWakeEnabled = FALSE;
    port->AllowDisable  = FALSE; //是否允许不使能要看注册表的值

    //下面设置port->AllowDisable
	//这个来自于注册表，如果注册表关了也没办法
	//第1次初始化oa
	InitializeObjectAttributes (&oa, //输出，OBJECT_ATTRIBUTES
                                &Globals.RegistryPath,
                                OBJ_CASE_INSENSITIVE,
                                NULL,
                                (PSECURITY_DESCRIPTOR) NULL);

    status = ZwOpenKey (&hService/*输出，服务句柄*/, KEY_ALL_ACCESS, &oa);

    if (NT_SUCCESS (status)) {
        UNICODE_STRING parameters;

        RtlInitUnicodeString(&parameters, L"Parameters");
		//第2次初始化oa
        InitializeObjectAttributes (&oa, //现在有数据了，是输入
                                    &parameters, //要取服务的参数（键为Parameters）
                                    OBJ_CASE_INSENSITIVE,
                                    hService, //读注册表需要句柄啊
                                    (PSECURITY_DESCRIPTOR) NULL);

        status = ZwOpenKey (&hParameters/*输出，参数句柄*/, KEY_ALL_ACCESS, &oa);
        if (NT_SUCCESS (status)) {
			//查询键值KEYBOARD_ALLOW_DISABLE
            status = KeyboardQueryDeviceKey (hParameters,
                                             KEYBOARD_ALLOW_DISABLE,
                                             &tmp, //从注册表读取的value放这里
                                             sizeof (tmp));

            if (NT_SUCCESS (status)) {
                port->AllowDisable = (tmp ? TRUE : FALSE);
            }

            ZwClose (hParameters); //要关闭读注册表的句柄
        }

        ZwClose (hService);//要关闭读注册表的句柄
    }

    fdo->Flags |= DO_POWER_PAGABLE; //什么意思？
    fdo->Flags &= ~DO_DEVICE_INITIALIZING; //快好了

	//光有FDO现在没接口，不过用
	//注册键盘类接口（有专用GUID：GUID_CLASS_KEYBOARD），并且创建一个设备接口的实例
    status = IoRegisterDeviceInterface (PhysicalDeviceObject, //不能使用FDO创建接口，而必须使用PDO创建接口
                                        (LPGUID)&GUID_CLASS_KEYBOARD,
                                        NULL,
                                        &port->SymbolicLinkName );//输出，接口符号名，放在设备扩展内

    if (!NT_SUCCESS (status)) {
        IoDetachDevice (port->TopPort); //搬下来
        port->TopPort = NULL;
        IoDeleteDevice (fdo);//删除本函数里面刚刚创建的
    } else {
        status = KeyboardAddDeviceEx (port, fullClassName/*创建FDO一并的得到的*/, NULL);
    }

    if (fullClassName) {
        ExFreePool(fullClassName);
    }

    return status;
}


NTSTATUS
KeyboardQueryDeviceKey (
    IN  HANDLE  Handle,
    IN  PWCHAR  ValueNameString,
    OUT PVOID   Data,
    IN  ULONG   DataLength
    )
{
    NTSTATUS        status;
    UNICODE_STRING  valueName;
    ULONG           length;
    PKEY_VALUE_FULL_INFORMATION fullInfo;

    PAGED_CODE();

    RtlInitUnicodeString (&valueName, ValueNameString);

    length = sizeof (KEY_VALUE_FULL_INFORMATION)
           + valueName.MaximumLength
           + DataLength;

    fullInfo = ExAllocatePool (PagedPool, length);

    if (fullInfo) {
        status = ZwQueryValueKey (Handle,
                                  &valueName,
                                  KeyValueFullInformation,
                                  fullInfo,
                                  length,
                                  &length);

        if (NT_SUCCESS (status)) {
            ASSERT (DataLength == fullInfo->DataLength);
            RtlCopyMemory (Data,
                           ((PUCHAR) fullInfo) + fullInfo->DataOffset,
                           fullInfo->DataLength);
        }

        ExFreePool (fullInfo);
    } else {
        status = STATUS_NO_MEMORY;
    }

    return status;
}


    PIRP irp;
    IO_STATUS_BLOCK ioStatus;
    NTSTATUS status;
    KEVENT event;
    CONNECT_DATA connectData;

    PAGED_CODE ();

    KbdPrint((2,"KBDCLASS-KbdSendConnectRequest: enter\n"));

    //
    // Create notification event object to be used to signal the
    // request completion.
    //

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    //
    // Build the synchronous request to be sent to the port driver
    // to perform the request.  Allocate an IRP to issue the port internal
    // device control connect call.  The connect parameters are passed in
    // the input buffer, and the keyboard attributes are copied back
    // from the port driver directly into the class device extension.
    //

    connectData.ClassDeviceObject = ClassData->TrueClassDevice;
    connectData.ClassService = ServiceCallback;

    irp = IoBuildDeviceIoControlRequest(
            IOCTL_INTERNAL_KEYBOARD_CONNECT,
            ClassData->TopPort,
            &connectData,
            sizeof(CONNECT_DATA),
            NULL,
            0,
            TRUE,
            &event,
            &ioStatus
            );

    if (irp) {
                 
        //
        // Call the port driver to perform the operation.  If the returned status
        // is PENDING, wait for the request to complete.
        //

        status = IoCallDriver(ClassData->TopPort, irp);

        if (status == STATUS_PENDING) {
            (VOID) KeWaitForSingleObject(
                &event,
                Executive,
                KernelMode,
                FALSE,
                NULL
                );
            
            status = irp->IoStatus.Status;
            
        } else {

            //
            // Ensure that the proper status value gets picked up.
            //

            ioStatus.Status = status;
            
        }

    } else {
        
        ioStatus.Status = STATUS_INSUFFICIENT_RESOURCES;

    }



    KbdPrint((2,"KBDCLASS-KbdSendConnectRequest: exit\n"));

    return(ioStatus.Status);

} // end KbdSendConnectRequest()

VOID
KeyboardClassLogError(
    PVOID Object,
    ULONG ErrorCode,
    ULONG UniqueErrorValue,
    NTSTATUS FinalStatus,
    ULONG DumpCount,
    ULONG *DumpData,
    UCHAR MajorFunction
    )
{
    PIO_ERROR_LOG_PACKET errorLogEntry;
    ULONG i;

    errorLogEntry = (PIO_ERROR_LOG_PACKET)
        IoAllocateErrorLogEntry(
            Object,
            (UCHAR) (sizeof(IO_ERROR_LOG_PACKET) + (DumpCount * sizeof(ULONG)))
            );

    if (errorLogEntry != NULL) {

        errorLogEntry->ErrorCode = ErrorCode;
        errorLogEntry->DumpDataSize = (USHORT) (DumpCount * sizeof(ULONG));
        errorLogEntry->SequenceNumber = 0;
        errorLogEntry->MajorFunctionCode = MajorFunction;
        errorLogEntry->IoControlCode = 0;
        errorLogEntry->RetryCount = 0;
        errorLogEntry->UniqueErrorValue = UniqueErrorValue;
        errorLogEntry->FinalStatus = FinalStatus;
        for (i = 0; i < DumpCount; i++)
            errorLogEntry->DumpData[i] = DumpData[i];

        IoWriteErrorLogEntry(errorLogEntry);
    }
}