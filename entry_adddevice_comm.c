//DriverEntry和AddDevice共用的函数！
//创建FDO，正规名字是keyboard class device object
//创建FDO的同时一并得到了设备扩展，两个是共生的关系
//创建的FDO只设置了一下Flags，为缓冲型IO，而设备扩展的初始化要做很多,毕竟驱动程序就是该干这些活

//问题：就是调用了一个IoCreateDevice函数，为什么还单独写了这个函数？
//答案：(1)分两种情况：带设备名的创建和不带设备名的创建
//      (2)考虑到全局变量Globals的设置，虽然很少
//      (3)对设备扩展的初始化比较多，特别是Ringbuffer的初始化

NTSTATUS
KbdCreateClassObject(
    IN  PDRIVER_OBJECT      DriverObject,
    IN  PDEVICE_EXTENSION   TmpDeviceExtension,
    OUT PDEVICE_OBJECT    * ClassDeviceObject, //输出，指针的指针
    OUT PWCHAR            * FullDeviceName,    //输出，指针的指针
	                                           //输出：有名字时，"\Device\KeyboardClass0"或"\Device\KeyboardClass1"...
											   //输出：无名字时，为NULL
    IN  BOOLEAN             Legacy
    )

/*++

Routine Description:

    This routine creates the keyboard class device object.


Arguments:

    DriverObject - Pointer to driver object created by system.

    TmpDeviceExtension - Pointer to the template device extension.

    FullDeviceName - Pointer to the Unicode string that is the full path name
        for the class device object.

    ClassDeviceObject - Pointer to a pointer to the class device object.

Return Value:

    The function value is the final status from the operation.

--*/

{
    NTSTATUS            status;
    ULONG               uniqueErrorValue;
    PDEVICE_EXTENSION   deviceExtension = NULL;
    NTSTATUS            errorCode = STATUS_SUCCESS;
    UNICODE_STRING      fullClassName = {0,0,0};
    ULONG               dumpCount = 0;
    ULONG               dumpData[DUMP_COUNT];
    ULONG               i;
    WCHAR               nameIndex;

    PAGED_CODE ();

    KbdPrint((1,"\n\nKBDCLASS-KbdCreateClassObject: enter\n"));

    //
    // Create a non-exclusive device object for the keyboard class device.
    //

    ExAcquireFastMutex (&Globals.Mutex);

    //
    // Make sure ClassDeviceObject isn't pointing to a random pointer value
    //
    *ClassDeviceObject = NULL;

	////创建大师设备对象时下面为真，因为Globals.GrandMaster还没有，将在此创建
    if (NULL == Globals.GrandMaster) { 
		//第1/2种情况:
		// (1)1:1的情况，有设备名，设备名从0开始
		// (2)1:m的情况首次创建大师，大师的设备名为0
        //
        // Create a legacy name for this DO.
        //
        ExReleaseFastMutex (&Globals.Mutex);

        //
        // Set up space for the class's full device object name.
        //
        fullClassName.MaximumLength = sizeof(L"\\Device\\") +
                                    + Globals.BaseClassName.Length
                                    + sizeof(L"0"); //多1个字符

        if (Globals.ConnectOneClassToOnePort && Legacy) {
            fullClassName.MaximumLength += sizeof(L"Legacy");
        }

        fullClassName.Buffer = ExAllocatePool(PagedPool,
                                              fullClassName.MaximumLength);

        if (!fullClassName.Buffer) {

            KbdPrint((
                1,
                "KbdCLASS-KeyboardClassInitialize: Couldn't allocate string for device object name\n"
                ));

            status = STATUS_UNSUCCESSFUL;
            errorCode = KBDCLASS_INSUFFICIENT_RESOURCES;
            uniqueErrorValue = KEYBOARD_ERROR_VALUE_BASE + 6;
            dumpData[0] = (ULONG) fullClassName.MaximumLength;
            dumpCount = 1;
            goto KbdCreateClassObjectExit;
        }

        RtlZeroMemory(fullClassName.Buffer, fullClassName.MaximumLength);
        RtlAppendUnicodeToString(&fullClassName, L"\\Device\\");
        RtlAppendUnicodeToString(&fullClassName, Globals.BaseClassName.Buffer);

        if (Globals.ConnectOneClassToOnePort && Legacy) {
            RtlAppendUnicodeToString(&fullClassName, L"Legacy");
        }

        RtlAppendUnicodeToString(&fullClassName, L"0"); //后面加个序号0

        //
        // Using the base name start trying to create device names until
        // one succeeds.  Everytime start over at 0 to eliminate gaps.
        //
        nameIndex = 0; //每次都从0开始，原来本函数会被多次调用

        do {
            fullClassName.Buffer [ (fullClassName.Length / sizeof (WCHAR)) - 1]
                = L'0' + nameIndex++; //注意这里的++

            KbdPrint((
                1,
                "KBDCLASS-KbdCreateClassObject: Creating device object named %ws\n",
                fullClassName.Buffer
                ));
				
				
			//有名字，采用了一个防止gap出现的技巧，但是调用IoCreateDevice的次数太多了，前面都是废的
            status = IoCreateDevice(DriverObject,
                                    sizeof (DEVICE_EXTENSION),
                                    &fullClassName,       //设备名是输入
                                    FILE_DEVICE_KEYBOARD, //设备类型必须是这个
                                    0,
                                    FALSE,
                                    ClassDeviceObject); //输出PDEVICE_OBJECT  

        } while (STATUS_OBJECT_NAME_COLLISION == status); //为什么会有返回STATUS_OBJECT_NAME_COLLISION？

        *FullDeviceName = fullClassName.Buffer;//有设备名，还给调用者

    } else {
	
		//第2/2种情况:有大师，不需要设备名称
	    ExReleaseFastMutex (&Globals.Mutex);
		
		//没有名字，不会发生名称冲突的情况，当然一次成功
        status = IoCreateDevice(DriverObject,
                                sizeof(DEVICE_EXTENSION),
                                NULL, // no name for this FDO，仅这里不同
                                FILE_DEVICE_KEYBOARD, //设备类型必须是这个
                                0,
                                FALSE,
                                ClassDeviceObject);//输出，PDEVICE_OBJECT  
        *FullDeviceName = NULL; //没有设备名，还给调用者
    }

    if (!NT_SUCCESS(status)) {
        KbdPrint((
            1,
            "KBDCLASS-KbdCreateClassObject: Could not create class device object = %ws\n",
            fullClassName.Buffer
            ));

        errorCode = KBDCLASS_COULD_NOT_CREATE_DEVICE;
        uniqueErrorValue = KEYBOARD_ERROR_VALUE_BASE + 6;
        dumpData[0] = (ULONG) fullClassName.MaximumLength;
        dumpCount = 1;
        goto KbdCreateClassObjectExit;
    }

    //
    // Do buffered I/O.  I.e., the I/O system will copy to/from user data
    // from/to a system buffer.
    //
	/*
typedef struct _DEVICE_OBJECT {
  CSHORT                      Type;
  USHORT                      Size;
  LONG                        ReferenceCount;
  struct _DRIVER_OBJECT  *DriverObject;
  struct _DEVICE_OBJECT  *NextDevice;
  struct _DEVICE_OBJECT  *AttachedDevice;
  struct _IRP  *CurrentIrp;
  PIO_TIMER                   Timer;
  ULONG                       Flags; //下面要指定为DO_BUFFERED_IO
  ULONG                       Characteristics;
  __volatile PVPB             Vpb;
  PVOID                       DeviceExtension; //下面重点要操作这个地址指向的结构，先模板后个别改
  DEVICE_TYPE                 DeviceType;
  CCHAR                       StackSize;
  union {
    LIST_ENTRY         ListEntry;
    WAIT_CONTEXT_BLOCK Wcb;
  } Queue;
  ULONG                       AlignmentRequirement;
  KDEVICE_QUEUE               DeviceQueue;
  KDPC                        Dpc;
  ULONG                       ActiveThreadCount;
  PSECURITY_DESCRIPTOR        SecurityDescriptor;
  KEVENT                      DeviceLock;
  USHORT                      SectorSize;
  USHORT                      Spare1;
  struct _DEVOBJ_EXTENSION  *  DeviceObjectExtension;
  PVOID                       Reserved;
} DEVICE_OBJECT, *PDEVICE_OBJECT;
*/
	//对刚刚得到的FDO进行设置，因为键盘是已知的
    (*ClassDeviceObject)->Flags |= DO_BUFFERED_IO;
	
	//对FDO设置只有一小点，对设备扩展的设置就是一大点！
	//下面操作 PDEVICE_OBJECT->DeviceExtension这个大结构，该结构是驱动程序自定义的
    deviceExtension =
        (PDEVICE_EXTENSION)(*ClassDeviceObject)->DeviceExtension;
	
    *deviceExtension = *TmpDeviceExtension; //初始化设备扩展

	//把设备扩展和FDO互连起来，毕竟这是一块而诞生的！
    deviceExtension->Self = *ClassDeviceObject;
	
	//初始化锁，别让这FDO随便不见了
    IoInitializeRemoveLock (&deviceExtension->RemoveLock, KEYBOARD_POOL_TAG, 0, 0);

    //
    // Initialize spin lock for critical sections.
    //
    KeInitializeSpinLock (&deviceExtension->SpinLock);

    //
    // Initialize the read queue
    //
    InitializeListHead (&deviceExtension->ReadQueue);

    //
    // No trusted subsystem has sent us an open yet.
    //

    deviceExtension->TrustedSubsystemCount = 0;

    //
    // Allocate the ring buffer for the keyboard class input data.
    //

    deviceExtension->InputData =
        ExAllocatePool(
            NonPagedPool,
            deviceExtension->KeyboardAttributes.InputDataQueueLength
            );

    if (!deviceExtension->InputData) {

        //
        // Could not allocate memory for the keyboard class data queue.
        //

        KbdPrint((
            1,
            "KBDCLASS-KbdCreateClassObject: Could not allocate input data queue for %ws\n",
            FullDeviceName
            ));

        status = STATUS_INSUFFICIENT_RESOURCES;

        //
        // Log an error.
        //

        errorCode = KBDCLASS_NO_BUFFER_ALLOCATED;
        uniqueErrorValue = KEYBOARD_ERROR_VALUE_BASE + 20;
        goto KbdCreateClassObjectExit;
    }

    //
    // Initialize keyboard class input data queue.
    //

	//下面继续操作 PDEVICE_OBJECT->DeviceExtension这个大结构
	//使用了一个专门的函数来初始化Ring Buffer！
	
    KbdInitializeDataQueue((PVOID)deviceExtension); 

KbdCreateClassObjectExit:

    if (status != STATUS_SUCCESS) {

        //
        // Some part of the initialization failed.  Log an error, and
        // clean up the resources for the failed part of the initialization.
        //
        RtlFreeUnicodeString (&fullClassName);
        *FullDeviceName = NULL;

        if (errorCode != STATUS_SUCCESS) {
            KeyboardClassLogError (
                (*ClassDeviceObject == NULL) ?
                    (PVOID) DriverObject : (PVOID) *ClassDeviceObject,
                errorCode,
                uniqueErrorValue,
                status,
                dumpCount,
                dumpData,
                0);
        }

        if ((deviceExtension) && (deviceExtension->InputData)) {
            ExFreePool (deviceExtension->InputData);
            deviceExtension->InputData = NULL;
        }
        if (*ClassDeviceObject) {
            IoDeleteDevice(*ClassDeviceObject);
            *ClassDeviceObject = NULL;
        }
    }

    KbdPrint((1,"KBDCLASS-KbdCreateClassObject: exit\n"));

    return(status);
}

//连接和写注册表（DEVICEMAP下面）
//所谓连接是利用回调连接两个
//ADD的意思是什么？
NTSTATUS
KeyboardAddDeviceEx(
    IN PDEVICE_EXTENSION    ClassData, //代表FDO
    IN PWCHAR               FullClassName,
    IN PFILE_OBJECT         File //只是在有大师的情况下保存在Glabal中，并未使用
    )
 /*++ Description:
  *
  * Called whenever the Keyboard Class driver is loaded to control a device.
  *
  * Two possible reasons.
  * 1) Plug and Play found a PNP enumerated keyboard port.
  * 2) Driver Entry found this device via old crusty执拗的 legacy reasons.
  *
  * Arguments:
  *
  *
  * Return:
  *
  * STATUS_SUCCESS - if successful STATUS_UNSUCCESSFUL - otherwise
  *
  * --*/
{
    NTSTATUS                errorCode = STATUS_SUCCESS;
    NTSTATUS                status = STATUS_SUCCESS;
    PDEVICE_EXTENSION       trueClassData;
    PPORT                   classDataList;
    ULONG                   uniqueErrorValue = 0;
    PIO_ERROR_LOG_PACKET    errorLogEntry;
    ULONG                   dumpCount = 0;
    ULONG                   dumpData[DUMP_COUNT];
    ULONG                   i;

    PAGED_CODE ();

    KeInitializeSpinLock (&ClassData->WaitWakeSpinLock); //初始化spin锁

	//关于trueClassData：在1：m映射下为大师，在1:1情况下为自身
    if (Globals.ConnectOneClassToOnePort) {

        ASSERT (NULL == Globals.GrandMaster);
        trueClassData = ClassData;

    } else {
        trueClassData = Globals.GrandMaster; //大师前面已经创建好
    }
	
	//设置TrueClassDevice的地方，只有这个地方会设置
	ClassData->TrueClassDevice = trueClassData->Self;

    if ((Globals.GrandMaster != ClassData) &&
        (Globals.GrandMaster == trueClassData)) {
        //
        // We have a grand master, and are adding a port device object.
        //

        //
        // Connect to port device.
        //
        status = KbdSendConnectRequest(ClassData, KeyboardClassServiceCallback);

        //
        // Link this class device object in the list of class devices object
        // associated with the true class device object
        //
        ExAcquireFastMutex (&Globals.Mutex);

        for (i=0; i < Globals.NumAssocClass; i++) {
            if (Globals.AssocClassList[i].Free) {
                Globals.AssocClassList[i].Free = FALSE;
                break;
            }
        }

        if (i == Globals.NumAssocClass) {
            classDataList = ExAllocatePool (
                                NonPagedPool,
                                (Globals.NumAssocClass + 1) * sizeof (PORT));

            if (NULL == classDataList) {
                status = STATUS_INSUFFICIENT_RESOURCES;
                // ISSUE: log error

                ExReleaseFastMutex (&Globals.Mutex);

                goto KeyboardAddDeviceExReject;
            }

            RtlZeroMemory (classDataList,
                           (Globals.NumAssocClass + 1) * sizeof (PORT));

            if (0 != Globals.NumAssocClass) {
                RtlCopyMemory (classDataList,
                               Globals.AssocClassList,
                               Globals.NumAssocClass * sizeof (PORT));

                ExFreePool (Globals.AssocClassList);
            }
            Globals.AssocClassList = classDataList;
            Globals.NumAssocClass++;
        }

        ClassData->UnitId = i;
        Globals.AssocClassList [i].Port = ClassData;
        Globals.AssocClassList [i].File = File;

		//这句话能看出点ADD的影子
        trueClassData->Self->StackSize =
            MAX (trueClassData->Self->StackSize, ClassData->Self->StackSize);

        ExReleaseFastMutex (&Globals.Mutex);

    } else if ((Globals.GrandMaster != ClassData) &&
               (ClassData == trueClassData)) {

        //
        // Connect to port device.
        //
        status = KbdSendConnectRequest(ClassData, KeyboardClassServiceCallback);
        ASSERT (STATUS_SUCCESS == status);
    }

    if (ClassData == trueClassData) {

        ASSERT (NULL != FullClassName); //要写注册表，怎么能为空呢？

        //
        // Load the device map information into the registry so
        // that setup can determine which keyboard class driver is active.
        //

        status = RtlWriteRegistryValue( //参数都是输入
                     RTL_REGISTRY_DEVICEMAP,
                     Globals.BaseClassName.Buffer, // key name = L"KeyboardClass"
                     FullClassName, // value name
                     REG_SZ,
                     Globals.RegistryPath.Buffer, // The value
                     Globals.RegistryPath.Length + sizeof(UNICODE_NULL));

        if (!NT_SUCCESS(status)) {

            KbdPrint((
                1,
                "KBDCLASS-KeyboardClassInitialize: Could not store %ws in DeviceMap\n",
                FullClassName));

            KeyboardClassLogError (ClassData,
                                   KBDCLASS_NO_DEVICEMAP_CREATED,
                                   KEYBOARD_ERROR_VALUE_BASE + 14,
                                   status,
                                   0,
                                   NULL,
                                   0);
        } else {

            KbdPrint((
                1,
                "KBDCLASS-KeyboardClassInitialize: Stored %ws in DeviceMap\n",
                FullClassName));

        }
    }

    return status;

KeyboardAddDeviceExReject:

    //
    // Some part of the initialization failed.  Log an error, and
    // clean up the resources for the failed part of the initialization.
    //
    if (errorCode != STATUS_SUCCESS) {

        errorLogEntry = (PIO_ERROR_LOG_PACKET)
            IoAllocateErrorLogEntry(
                trueClassData->Self,
                (UCHAR) (sizeof(IO_ERROR_LOG_PACKET)
                         + (dumpCount * sizeof(ULONG)))
                );

        if (errorLogEntry != NULL) {

            errorLogEntry->ErrorCode = errorCode;
            errorLogEntry->DumpDataSize = (USHORT) (dumpCount * sizeof (ULONG));
            errorLogEntry->SequenceNumber = 0;
            errorLogEntry->MajorFunctionCode = 0;
            errorLogEntry->IoControlCode = 0;
            errorLogEntry->RetryCount = 0;
            errorLogEntry->UniqueErrorValue = uniqueErrorValue;
            errorLogEntry->FinalStatus = status;
            for (i = 0; i < dumpCount; i++)
                errorLogEntry->DumpData[i] = dumpData[i];

            IoWriteErrorLogEntry(errorLogEntry);
        }

    }

    return status;
}


//连接是个同步要做的事情，不会异步完成，难怪启动慢
//是上层的FDO向下层的PDO发送
//下层的PDO会把回调函数装上
NTSTATUS
KbdSendConnectRequest(
    IN PDEVICE_EXTENSION ClassData, //FDO
    IN PVOID ServiceCallback
    )

/*++

Routine Description:

    This routine sends a connect request to the port driver.

Arguments:

    DeviceObject - Pointer to class device object.

    ServiceCallback - Pointer to the class service callback routine.

    PortIndex - The index into the PortDeviceObjectList[] for the current
        connect request.

Return Value:

    Status is returned.

--*/

{
    PIRP irp;
    IO_STATUS_BLOCK ioStatus;
    NTSTATUS status;
    KEVENT event;
	
/*Kbdmou.h
typedef struct _CONNECT_DATA {
  PDEVICE_OBJECT ClassDeviceObject;
  PVOID          ClassService;
} CONNECT_DATA, *PCONNECT_DATA;

*/
    CONNECT_DATA connectData; //必须驻留系统内存
	
    PAGED_CODE ();

    KbdPrint((2,"KBDCLASS-KbdSendConnectRequest: enter\n"));

    //
    // Create notification event object to be used to signal the
    // request completion.
    //

    KeInitializeEvent(&event, NotificationEvent, FALSE); //初始化event为false

    //
    // Build the synchronous request to be sent to the port driver
    // to perform the request.  Allocate an IRP to issue the port internal
    // device control connect call.  The connect parameters are passed in
    // the input buffer, and the keyboard attributes are copied back
    // from the port driver directly into the class device extension.
    //

	//填充CONNECT_DATA结构
    connectData.ClassDeviceObject = ClassData->TrueClassDevice; //一定要填真设备
    connectData.ClassService = ServiceCallback; //输入本函数的回调地址

    irp = IoBuildDeviceIoControlRequest( //同步处理设备控制请求
            IOCTL_INTERNAL_KEYBOARD_CONNECT,
            ClassData->TopPort,
            &connectData, //InputBuffer
            sizeof(CONNECT_DATA), //InputBufferLength
            NULL, //OutputBuffer
            0,    //OutputBufferLength
            TRUE,  //InternalDeviceIoControl
            &event, //刚刚初始化为false状态
            &ioStatus
            );

    if (irp) {
                 
        //
        // Call the port driver to perform the operation.  If the returned status
        // is PENDING, wait for the request to complete.
        //

        status = IoCallDriver(ClassData->TopPort, irp);

        if (status == STATUS_PENDING) {
            (VOID) KeWaitForSingleObject( //同步等待
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
        
        //资源不足
		ioStatus.Status = STATUS_INSUFFICIENT_RESOURCES;

    }



    KbdPrint((2,"KBDCLASS-KbdSendConnectRequest: exit\n"));

    return(ioStatus.Status);

} // end KbdSendConnectRequest()


//在别的地方也用吧，很简单
VOID
KbdInitializeDataQueue (
    IN PVOID Context
    )

/*++

Routine Description:

    This routine initializes the input data queue.  IRQL is raised to
    DISPATCH_LEVEL to synchronize with StartIo, and the device object
    spinlock is acquired.

Arguments:

    Context - Supplies a pointer to the device extension.

Return Value:

    None.

--*/

{
    KIRQL oldIrql;
    PDEVICE_EXTENSION deviceExtension;

    KbdPrint((3,"KBDCLASS-KbdInitializeDataQueue: enter\n"));

    //
    // Get address of device extension.
    //

    deviceExtension = (PDEVICE_EXTENSION)Context;

    //
    // Acquire the spinlock to protect the input data
    // queue and associated pointers.
    //

    KeAcquireSpinLock(&deviceExtension->SpinLock, &oldIrql);

    //
    // Initialize the input data queue.
    //

    deviceExtension->InputCount = 0;
    deviceExtension->DataIn = deviceExtension->InputData;
    deviceExtension->DataOut = deviceExtension->InputData;

    deviceExtension->OkayToLogOverflow = TRUE;

    //
    // Release the spinlock and lower IRQL.
    //

    KeReleaseSpinLock(&deviceExtension->SpinLock, oldIrql);

    KbdPrint((3,"KBDCLASS-KbdInitializeDataQueue: exit\n"));

}