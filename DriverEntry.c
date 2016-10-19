NTSTATUS
DriverEntry( //本函数被IO Manager调用
    IN PDRIVER_OBJECT DriverObject, //IO manager创建的
    IN PUNICODE_STRING InputRegistryPath //L"\计算机\HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\kbdclass"
    )

/*++

Routine Description:

    This routine initializes the keyboard class driver.

Arguments:

    DriverObject - Pointer to driver object created by system.

    InputRegistryPath - Pointer to the Unicode name of the registry path
        for this driver.

Return Value:

    The function value is the final status from the initialization operation.

--*/

{
    NTSTATUS                status = STATUS_SUCCESS;
    PDEVICE_EXTENSION       tmp_deviceExtension = NULL;
    PDEVICE_OBJECT          tmp_classDeviceObject = NULL;
    ULONG                   dumpCount = 0;
    ULONG                   dumpData[DUMP_COUNT];
    ULONG                   i;
    ULONG                   tmp_numPorts;
    ULONG                   uniqueErrorValue;
    UNICODE_STRING          tmp_basePortName;
    UNICODE_STRING          tmp_fullPortName;
    WCHAR                   tmp_basePortBuffer[NAME_MAX];
    PWCHAR                  tmp_fullClassName = NULL;
    PFILE_OBJECT            tmp_fileobject;
    PLIST_ENTRY             entry;

    KbdPrint((1,"\n\nKBDCLASS-KeyboardClassInitialize: enter\n"));

    //
    // Zero-initialize various structures.
    //
    RtlZeroMemory(&Globals, sizeof(GLOBALS)); //Globals全局变量从全新的0开始！

    Globals.Debug = DEFAULT_DEBUG_LEVEL;

    InitializeListHead (&Globals.LegacyDeviceList);//初始化腰带

    tmp_fullPortName.MaximumLength = 0;

    ExInitializeFastMutex (&Globals.Mutex);//初始化mutex
	
	//把BaseClassName整好，目前什么东西都没有
    Globals.BaseClassName.Buffer = Globals.BaseClassBuffer;
    Globals.BaseClassName.Length = 0;
    Globals.BaseClassName.MaximumLength = NAME_MAX * sizeof(WCHAR);

	//把临时变量整好，目前什么东西都没有
    RtlZeroMemory(tmp_basePortBuffer, NAME_MAX * sizeof(WCHAR));
    tmp_basePortName.Buffer = tmp_basePortBuffer;
    tmp_basePortName.Length = 0;
    tmp_basePortName.MaximumLength = NAME_MAX * sizeof(WCHAR);

    //
    // Need to ensure that the registry path is null-terminated.
    // Allocate pool to hold a null-terminated copy of the path.
    //
	//把注册表路径永久地保存起来
    Globals.RegistryPath.Length = InputRegistryPath->Length;
    Globals.RegistryPath.MaximumLength = InputRegistryPath->Length
                                       + sizeof (UNICODE_NULL);

    Globals.RegistryPath.Buffer = ExAllocatePool(
                                      NonPagedPool,
                                      Globals.RegistryPath.MaximumLength);

    if (!Globals.RegistryPath.Buffer) {
        KbdPrint((
            1,
            "KBDCLASS-KeyboardClassInitialize: Couldn't allocate pool for registry path\n"
            ));

        dumpData[0] = (ULONG) InputRegistryPath->Length + sizeof(UNICODE_NULL);

        KeyboardClassLogError (DriverObject,
                               KBDCLASS_INSUFFICIENT_RESOURCES,
                               KEYBOARD_ERROR_VALUE_BASE + 2,
                               STATUS_UNSUCCESSFUL,
                               1,
                               dumpData,
                               0);

        goto KeyboardClassInitializeExit;
    }

	//必须保存起来，否则I/O manager将会释放RegistryPath buffer
    RtlMoveMemory(Globals.RegistryPath.Buffer,
                  InputRegistryPath->Buffer, //来自I/O manager
                  InputRegistryPath->Length);
    Globals.RegistryPath.Buffer [InputRegistryPath->Length / sizeof (WCHAR)] = L'\0';

    //
    // Get the configuration information for this driver.
    //

    KbdConfiguration();//读取注册表参数配置到Gloabals

    //
    // If there is only one class device object then create it as the grand
    // master device object.  Otherwise let all the FDOs also double as the
    // class DO.
    //
	
	// 只有一个类设备对象(class device object)的情况：非1个class<-对->1个port
	// 在Windows7、10上，ConnectMultiplePorts = 0, 那么Globals.ConnectOneClassToOnePort=1
    if (!Globals.ConnectOneClassToOnePort) { //来自注册表键值
		
		//创建大师设备对象
        status = KbdCreateClassObject (DriverObject,//本函数的输入参数
                                       &Globals.InitExtension,//输入
                                       &tmp_classDeviceObject,//输出，其设备扩展将为大师
                                       &tmp_fullClassName,//输出，L"\DEVICE\KeyboardClass0"
                                       TRUE); //传统设备
        if (!NT_SUCCESS (status)) {
            // ISSUE:  should log an error that we could not create a GM
            goto KeyboardClassInitializeExit;
        }

		//永久性地保存Extension为祖父级，这对应第一个设备对象
        tmp_deviceExtension = (PDEVICE_EXTENSION)tmp_classDeviceObject->DeviceExtension;
		//只在这里设置大师
        Globals.GrandMaster = tmp_deviceExtension; //Extension的信息多
		
		//Extension是驱动程序自己管理的地盘
        tmp_deviceExtension->PnP = FALSE;//大师本身不是pnp，大师是负责很多端口处理的，这些端口可能是pnp的
		
		//下面并未真正添加设备，仅仅是写注册表
		//HKEY = HKEY_LOCAL_MACHINE\HARDWARE\DEVICEMAP\KeyboardClass
		//value name = \DEVICE\KeyboardClass0
		//value = HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\kbdclass
        KeyboardAddDeviceEx (tmp_deviceExtension, tmp_fullClassName/*L"\DEVICE\KeyboardClass0"*/, NULL/*未用*/);

        ASSERT (NULL != tmp_fullClassName);
        ExFreePool (tmp_fullClassName); //tmp_fullClassName没用了
        tmp_fullClassName = NULL;//回到初始状态

        tmp_classDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING; //快结束了？没啊
    }

	//在Windows7上，不会有大师？
    //
    // Set up the base device name for the associated port device.
    // It is the same as the base class name, with "Class" replaced
    // by "Port".
    //
	// 设置好tmp_basePortName为"KeyboardPort"
    RtlCopyUnicodeString(&tmp_basePortName, &Globals.BaseClassName);
    tmp_basePortName.Length -= (sizeof(L"Class") - sizeof(UNICODE_NULL));
    RtlAppendUnicodeToString(&tmp_basePortName, L"Port");

    //
    // Determine how many (static) ports this class driver is to service.
    //
    //
    // If this returns zero, then all ports will be dynamically PnP added later
    //
	//通过读注册表键值并进行统计获得服务的端口数
    KbdDeterminePortsServiced(&tmp_basePortName, &tmp_numPorts/*输出，我们想要的端口数*/);

    ASSERT (tmp_numPorts <= MAXIMUM_PORTS_SERVICED);

    KbdPrint((
        1,
        "KBDCLASS-KeyboardClassInitialize: Will service %d port devices\n",
        tmp_numPorts
        ));

    //
    // Set up space for the class's full device object name.
    //
    RtlInitUnicodeString(&tmp_fullPortName, NULL);

    tmp_fullPortName.MaximumLength = sizeof(L"\\Device\\")
                                + tmp_basePortName.Length
                                + sizeof (UNICODE_NULL);

    tmp_fullPortName.Buffer = ExAllocatePool(PagedPool,
                                         tmp_fullPortName.MaximumLength);

    if (!tmp_fullPortName.Buffer) {

        KbdPrint((
            1,
            "KBDCLASS-KeyboardClassInitialize: Couldn't allocate string for device object name\n"
            ));

        status = STATUS_UNSUCCESSFUL;
        dumpData[0] = (ULONG) tmp_fullPortName.MaximumLength;

        KeyboardClassLogError (DriverObject,
                               KBDCLASS_INSUFFICIENT_RESOURCES,
                               KEYBOARD_ERROR_VALUE_BASE + 6,
                               status,
                               1,
                               dumpData,
                               0);

        goto KeyboardClassInitializeExit;

    }

    RtlZeroMemory(tmp_fullPortName.Buffer, tmp_fullPortName.MaximumLength);
    RtlAppendUnicodeToString(&tmp_fullPortName, L"\\Device\\");
    RtlAppendUnicodeToString(&tmp_fullPortName, tmp_basePortName.Buffer);
    RtlAppendUnicodeToString(&tmp_fullPortName, L"0");

    //
    // Set up the class device object(s) to handle the associated
    // port devices.
    //
    for (i = 0; (i < Globals.PortsServiced) && (i < tmp_numPorts); i++) {

        //
        // Append the suffix to the device object name string.  E.g., turn
        // \Device\KeyboardClass into \Device\KeyboardClass0.  Then attempt
        // to create the device object.  If the device object already
        // exists increment the suffix and try again.
        //

        tmp_fullPortName.Buffer[(tmp_fullPortName.Length / sizeof(WCHAR)) - 1]
            = L'0' + (WCHAR) i;

        //
        // Create the class device object.
        //
        status = KbdCreateClassObject (DriverObject,//本函数的输入参数
                                       &Globals.InitExtension,//输入
                                       &tmp_classDeviceObject,//输出，新创建了一个port的设备对象
                                       &tmp_fullClassName,//输出
                                       TRUE);
									   
		//创建的设备对象tmp_classDeviceObject会被修改，为什么要改？
		//    (1)tmp_classDeviceObject->DeviceExtension->PnP = FALSE;实际上是驱动自定义的东西
		//    (2)tmp_classDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING
		//    (3)tmp_classDeviceObject->DeviceExtension->TopPort = ？
		//	  (4)tmp_classDeviceObject->StackSize逐渐增加
        if (!NT_SUCCESS(status)) {
            KeyboardClassLogError (DriverEntry,
                                   KBDCLASS_INSUFFICIENT_RESOURCES,
                                   KEYBOARD_ERROR_VALUE_BASE + 8,
                                   status,
                                   0,
                                   NULL,
                                   0);
            continue;
        }

        tmp_deviceExtension = (PDEVICE_EXTENSION)tmp_classDeviceObject->DeviceExtension;
        tmp_deviceExtension->PnP = FALSE; //随i变化

        tmp_classDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

        //
        // Connect to the port device.
        // 知识：msdn说了，IoGetDeviceObjectPointer establishes a "connection" between the caller and the next-lower-level driver.
		//下面函数的功能：
		//返回1：returns a pointer to the top object in the named device object's stack
		//返回2：a pointer to the corresponding file object
        status = IoGetDeviceObjectPointer (&tmp_fullPortName,//"\Device\KeyboardClass0..."
                                           FILE_READ_ATTRIBUTES, //规定权限为读
                                           &tmp_fileobject,//输出，FileObject
                                           &tmp_deviceExtension->TopPort); //输出，PDEVICE_OBJECT，随i变化

        //
        // In case of failure, just delete the device and continue
        //
        if (!NT_SUCCESS(status)) {
            // ISSUE: log error，删除设备(delete the device)
			// 使用IoDeleteDevice删除deviceExtension所属的device object
            KeyboardClassDeleteLegacyDevice (tmp_deviceExtension);//
            continue;
        }

		//随i肯定是增加的，越叠越高
        tmp_classDeviceObject->StackSize = 1 + tmp_deviceExtension->TopPort->StackSize;//随i变化
		
		//1.Connect to port device.
		//2.要写到注册表中区
        status = KeyboardAddDeviceEx (tmp_deviceExtension, tmp_fullClassName, tmp_fileobject);

        if (tmp_fullClassName) {
            ExFreePool (tmp_fullClassName);
            tmp_fullClassName = NULL;
        }

        if (!NT_SUCCESS (status)) {
            if (Globals.GrandMaster == NULL) { //失败处理1：传统端口设备
                if (tmp_deviceExtension->File) {
                    tmp_fileobject = tmp_deviceExtension->File; //马上要减去一个引用
                    tmp_deviceExtension->File = NULL;
                }
            }
            else {
                PPORT port; //失败处理2：非传统端口设备，有大师传统端口设备

                ExAcquireFastMutex (&Globals.Mutex);

                tmp_fileobject = Globals.AssocClassList[tmp_deviceExtension->UnitId].File;//马上要减去一个引用
                Globals.AssocClassList[tmp_deviceExtension->UnitId].File = NULL;
                Globals.AssocClassList[tmp_deviceExtension->UnitId].Free = TRUE;
                Globals.AssocClassList[tmp_deviceExtension->UnitId].Port = NULL;

                ExReleaseFastMutex (&Globals.Mutex);
            }

            if (tmp_fileobject) { //有可能已经为NULL了
                ObDereferenceObject (tmp_fileobject);//减去一个引用
            }

            KeyboardClassDeleteLegacyDevice (tmp_deviceExtension);
            continue;
        }

        //
        // Store this device object in a linked list regardless if we are in
        // grand master mode or not
        //
        InsertTailList (&Globals.LegacyDeviceList, &tmp_deviceExtension->Link);
    } // for

    //
    // If we had any failures creating legacy device objects, we must still
    // succeed b/c we need to service pnp ports later on
    //
    status = STATUS_SUCCESS;

    //
    // Count the number of legacy device ports we created
    //
    for (entry = Globals.LegacyDeviceList.Flink;
         entry != &Globals.LegacyDeviceList;
         entry = entry->Flink) {
        Globals.NumberLegacyPorts++;
    }

KeyboardClassInitializeExit:

    //
    // Free the unicode strings.
    //
    if (tmp_fullPortName.MaximumLength != 0){
        ExFreePool (tmp_fullPortName.Buffer);
    }

    if (tmp_fullClassName) {
        ExFreePool (tmp_fullClassName);
    }

    if (NT_SUCCESS (status)) {

		//注册its Reinitialize routine
		
        IoRegisterDriverReinitialization(DriverObject,
                                         KeyboardClassFindMorePorts,
                                         NULL);

        //
        // Set up the device driver entry points.
        //
        DriverObject->MajorFunction[IRP_MJ_CREATE]         = KeyboardClassCreate;
        DriverObject->MajorFunction[IRP_MJ_CLOSE]          = KeyboardClassClose;
        DriverObject->MajorFunction[IRP_MJ_READ]           = KeyboardClassRead;
        DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS]  = KeyboardClassFlush;
        DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = KeyboardClassDeviceControl;
        DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] =
                                                             KeyboardClassPassThrough;
        DriverObject->MajorFunction[IRP_MJ_CLEANUP]        = KeyboardClassCleanup;
        DriverObject->MajorFunction[IRP_MJ_PNP]            = KeyboardPnP;
        DriverObject->MajorFunction[IRP_MJ_POWER]          = KeyboardClassPower;
        DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = KeyboardClassSystemControl;
        DriverObject->DriverExtension->AddDevice           = KeyboardAddDevice;

        // DriverObject->DriverUnload = KeyboardClassUnload;

        status = STATUS_SUCCESS;

    } else {
        //
        // Clean up all the pool we created and delete the GM if it exists
        //
        if (Globals.RegistryPath.Buffer != NULL) {
            ExFreePool (Globals.RegistryPath.Buffer);
            Globals.RegistryPath.Buffer = NULL;
        }

        if (Globals.AssocClassList) {
            ExFreePool (Globals.AssocClassList);
            Globals.AssocClassList = NULL;
        }

        if (Globals.GrandMaster) {
            KeyboardClassDeleteLegacyDevice(Globals.GrandMaster);
            Globals.GrandMaster = NULL;
        }
    }

    KbdPrint((1,"KBDCLASS-KeyboardClassInitialize: exit\n"));

    return status;
}

VOID
KbdConfiguration() //从注册表中：\计算机\HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\kbdclass\Parameters
                   //键值读出5个参数信息，放在Glabal中，很多信息放在InitExtension用于创建设备的模板


/*++

Routine Description:

    This routine stores the configuration information for this device.

Return Value:

    None.  As a side-effect, sets fields in
    DeviceExtension->KeyboardAttributes.

--*/

{
    PRTL_QUERY_REGISTRY_TABLE parameters = NULL; //查询表
    ULONG defaultDataQueueSize = DATA_QUEUE_SIZE;
    ULONG defaultMaximumPortsServiced = 1;
    ULONG defaultConnectMultiplePorts = 1;
    ULONG defaultSendOutputToAllPorts = 0;
    NTSTATUS status = STATUS_SUCCESS;
    UNICODE_STRING parametersPath;
    UNICODE_STRING defaultUnicodeName; //将为L"KeyboardClass" ，为什么不是const？要来自头文件
    PWSTR path = NULL;
    USHORT queriesPlusOne = 6;

    PAGED_CODE ();

    parametersPath.Buffer = NULL;

    //
    // Registry path is already null-terminated, so just use it.
    //

	//path指向L"\计算机\HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\kbdclass"
    path = Globals.RegistryPath.Buffer; //已经包括数据

    //
    // Allocate the Rtl query table.
    //
	//分配注册表查询表数组，含6个，有用的5个
    parameters = ExAllocatePool(
                     PagedPool,
                     sizeof(RTL_QUERY_REGISTRY_TABLE) * queriesPlusOne
                     );

    if (!parameters) {

        KbdPrint((
            1,
            "KBDCLASS-KbdConfiguration: Couldn't allocate table for Rtl query to parameters for %ws\n",
            path
            ));

        status = STATUS_UNSUCCESSFUL;

    } else {

        RtlZeroMemory(
            parameters,
            sizeof(RTL_QUERY_REGISTRY_TABLE) * queriesPlusOne
            );

        //
        // Form a path to this driver's Parameters subkey.
        //

        RtlInitUnicodeString(
            &parametersPath,
            NULL
            );

        parametersPath.MaximumLength = Globals.RegistryPath.Length +
                                       sizeof(L"\\Parameters");

        parametersPath.Buffer = ExAllocatePoolExAllocatePool(
                                    PagedPool, //在分页内存上，有什么问题？
                                    parametersPath.MaximumLength
                                    );

        if (!parametersPath.Buffer) {

            KbdPrint((
                1,
                "KBDCLASS-KbdConfiguration: Couldn't allocate string for path to parameters for %ws\n",
                path
                ));

            status = STATUS_UNSUCCESSFUL;

        }
    }

    if (NT_SUCCESS(status)) {

        //
        // Form the parameters path.
        //

        RtlZeroMemory(parametersPath.Buffer, parametersPath.MaximumLength);
		//path指向L"\计算机\HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\kbdclass"
        RtlAppendUnicodeToString(&parametersPath, path);
		
		//parametersPath为L"\计算机\HKEY_LOCAL_MACHINE\System\CurrentControlSet\Services\kbdclass\Parameters"
        RtlAppendUnicodeToString(&parametersPath, L"\\Parameters");

        KbdPrint((
            1,
            "KBDCLASS-KbdConfiguration: parameters path is %ws\n",
             parametersPath.Buffer
            ));

        //
        // Form the default keyboard class device name, in case it is not
        // specified in the registry.
        //
		//要找的Parameters子键可能不存在，为防止万一，创建一个缺省的keyboard class device name
		//在kbdmou.h中，#define DD_KEYBOARD_CLASS_BASE_NAME_U   L"KeyboardClass" 
        RtlInitUnicodeString(
            &defaultUnicodeName,
            DD_KEYBOARD_CLASS_BASE_NAME_U
            ); //defaultUnicodeName现在为L"KeyboardClass"

        //
        // Gather all of the "user specified" information from
        // the registry.
        // 查找注册表的5个信息
		//当.Flags=RTL_QUERY_REGISTRY_DIRECT时，.EntryContext为输出，且QueryRoutine不用且必须为NULL
		//正是现在的情况
        parameters[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
        parameters[0].Name = L"KeyboardDataQueueSize";
        parameters[0].EntryContext =
            &Globals.InitExtension.KeyboardAttributes.InputDataQueueLength; //输出,100
        parameters[0].DefaultType = REG_DWORD;
        parameters[0].DefaultData = &defaultDataQueueSize;
        parameters[0].DefaultLength = sizeof(ULONG);

        parameters[1].Flags = RTL_QUERY_REGISTRY_DIRECT;
        parameters[1].Name = L"MaximumPortsServiced";
        parameters[1].EntryContext = &Globals.PortsServiced;//输出，3
        parameters[1].DefaultType = REG_DWORD;
        parameters[1].DefaultData = &defaultMaximumPortsServiced;
        parameters[1].DefaultLength = sizeof(ULONG);

        parameters[2].Flags = RTL_QUERY_REGISTRY_DIRECT;
        parameters[2].Name = L"KeyboardDeviceBaseName";
        parameters[2].EntryContext = &Globals.BaseClassName; //输出
        parameters[2].DefaultType = REG_SZ;
        parameters[2].DefaultData = defaultUnicodeName.Buffer;
        parameters[2].DefaultLength = 0;

        //
        // Using this parameter in an inverted fashion, registry key is
        // backwards from global variable.  (Note comment below).
        //
        parameters[3].Flags = RTL_QUERY_REGISTRY_DIRECT;
        parameters[3].Name = L"ConnectMultiplePorts";
        parameters[3].EntryContext = &Globals.ConnectOneClassToOnePort;//输出，0
        parameters[3].DefaultType = REG_DWORD;
        parameters[3].DefaultData = &defaultConnectMultiplePorts;
        parameters[3].DefaultLength = sizeof(ULONG);

        parameters[4].Flags = RTL_QUERY_REGISTRY_DIRECT;
        parameters[4].Name = L"SendOutputToAllPorts";
        parameters[4].EntryContext =
            &Globals.SendOutputToAllPorts;//输出,1
        parameters[4].DefaultType = REG_DWORD;
        parameters[4].DefaultData = &defaultSendOutputToAllPorts;
        parameters[4].DefaultLength = sizeof(ULONG);

		//查找注册表,找5个配置键值
        status = RtlQueryRegistryValues(
                     RTL_REGISTRY_ABSOLUTE | RTL_REGISTRY_OPTIONAL, //输入，查询方式
                     parametersPath.Buffer, //输入，Path
                     parameters, //输入和输出参数，windows规定的查询表PRTL_QUERY_REGISTRY_TABLE
                     NULL, //Context，因为此时QueryRoutine根本没用
                     NULL  //Environment，因为没有REG_EXPAND_SZ这种要查询的类型
                     );

        if (!NT_SUCCESS(status)) {
            KbdPrint((
                1,
                "KBDCLASS-KbdConfiguration: RtlQueryRegistryValues failed with 0x%x\n",
                status
                ));
        }
    }

    if (!NT_SUCCESS(status)) {

        //
        // Go ahead and assign driver defaults.
        //

        Globals.InitExtension.KeyboardAttributes.InputDataQueueLength =
            defaultDataQueueSize; //DATA_QUEUE_SIZE = 100
        Globals.PortsServiced = defaultMaximumPortsServiced; //1
        Globals.ConnectOneClassToOnePort = defaultConnectMultiplePorts;//1
        Globals.SendOutputToAllPorts = defaultSendOutputToAllPorts;//0
        RtlCopyUnicodeString(&Globals.BaseClassName, &defaultUnicodeName);
    }

    KbdPrint((
        1,
        "KBDCLASS-KbdConfiguration: Keyboard class base name = %ws\n",
        Globals.BaseClassName.Buffer
        ));

    if (Globals.InitExtension.KeyboardAttributes.InputDataQueueLength == 0) {

        KbdPrint((
            1,
            "KBDCLASS-KbdConfiguration: overriding KeyboardInputDataQueueLength = 0x%x\n",
            Globals.InitExtension.KeyboardAttributes.InputDataQueueLength
            ));

        Globals.InitExtension.KeyboardAttributes.InputDataQueueLength =
            defaultDataQueueSize;
    }

	//修正一下,有可能键值KeyboardDataQueueSize设置太大
    if ( MAXULONG/sizeof(KEYBOARD_INPUT_DATA) < Globals.InitExtension.KeyboardAttributes.InputDataQueueLength ) {
        // 
        // This is to prevent an Integer Overflow.
        //
        Globals.InitExtension.KeyboardAttributes.InputDataQueueLength = 
            defaultDataQueueSize * sizeof(KEYBOARD_INPUT_DATA);
    } else {
        Globals.InitExtension.KeyboardAttributes.InputDataQueueLength *=
            sizeof(KEYBOARD_INPUT_DATA);
    }

    KbdPrint((
        1,
        "KBDCLASS-KbdConfiguration: KeyboardInputDataQueueLength = 0x%x\n",
        Globals.InitExtension.KeyboardAttributes.InputDataQueueLength
        ));

    KbdPrint((
        1,
        "KBDCLASS-KbdConfiguration: MaximumPortsServiced = %d\n",
        Globals.PortsServiced
        ));

    //
    // Invert the flag that specifies the type of class/port connections.
    // We used it in the RtlQuery call in an inverted fashion.
    //
	//把ConnectMultiplePorts翻译成ConnectOneClassToOnePort而已，意思是正确的
    Globals.ConnectOneClassToOnePort = !Globals.ConnectOneClassToOnePort; 

    KbdPrint((
        1,
        "KBDCLASS-KbdConfiguration: Connection Type = %d\n",
        Globals.ConnectOneClassToOnePort
        ));

    //
    // Free the allocated memory before returning.
    //

    if (parametersPath.Buffer)
        ExFreePool(parametersPath.Buffer);
    if (parameters)
        ExFreePool(parameters);

}

NTSTATUS
KbdDeterminePortsServiced(
    IN PUNICODE_STRING BasePortName, //L"\Device\KeyboardPort"
    IN OUT PULONG NumberPortsServiced //希望找到的服务端口数量
    )
// 要么 一个device object 每个port device object 
// 要么 一个class device object连到多个port device objects
/*++

Routine Description:

    This routine reads the DEVICEMAP portion of the registry to determine
    how many ports the class driver is to service.  Depending on the
    value of DeviceExtension->ConnectOneClassToOnePort, the class driver
    will eventually create one device object per port device serviced, or
    one class device object that connects to multiple port device objects.

    Assumptions:

	下面第一条可能现在已经不存在了，不是KeyboardPort而是KeyboardClass
        1.  If the base device name for the class driver is "KeyboardClass",
                                                                     ^^^^^
            then the port drivers it can service are found under the
            "KeyboardPort" subkey in the DEVICEMAP portion of the registry.
                     ^^^^

        2.  The port device objects are created with suffixes in strictly
            ascending order, starting with suffix 0.  E.g.,
            \Device\KeyboardPort0 indicates the first keyboard port device,
            \Device\KeyboardPort1 the second, and so on.  There are no gaps
            in the list.

        3.  If ConnectOneClassToOnePort is non-zero, there is a 1:1
            correspondence between class device objects and port device
            objects.  I.e., \Device\KeyboardClass0 will connect to
            \Device\KeyboardPort0, \Device\KeyboardClass1 to
            \Device\KeyboardPort1, and so on.

        4.  If ConnectOneClassToOnePort is zero, there is a 1:many
            correspondence between class device objects and port device
            objects.  I.e., \Device\KeyboardClass0 will connect to
            \Device\KeyboardPort0, and \Device\KeyboardPort1, and so on.


    Note that for Product 1, the Raw Input Thread (Windows USER) will
    only deign to open and read from one keyboard device.  Hence, it is
    safe to make simplifying assumptions because the driver is basically
    providing  much more functionality than the RIT will use.

Arguments:

    BasePortName - Pointer to the Unicode string that is the base path name
        for the port device.

    NumberPortsServiced - Pointer to storage that will receive the
        number of ports this class driver should service.

Return Value:

    The function value is the final status from the operation.

--*/

{

    NTSTATUS status;
    PRTL_QUERY_REGISTRY_TABLE registryTable = NULL;
    USHORT queriesPlusOne = 2; //实际只有一个查询，后面一个为结束

    PAGED_CODE ();

    //
    // Initialize the result.
    //

    *NumberPortsServiced = 0;

    //
    // Allocate the Rtl query table.
    //

    registryTable = ExAllocatePool(
                        PagedPool,
                        sizeof(RTL_QUERY_REGISTRY_TABLE) * queriesPlusOne
                     );

    if (!registryTable) {

        KbdPrint((
            1,
            "KBDCLASS-KbdDeterminePortsServiced: Couldn't allocate table for Rtl query\n"
            ));

        status = STATUS_UNSUCCESSFUL;

    } else {

        RtlZeroMemory(
            registryTable,
            sizeof(RTL_QUERY_REGISTRY_TABLE) * queriesPlusOne
            );

        //
        // Set things up so that KbdDeviceMapQueryCallback will be
        // called once for every value in the keyboard port section
        // of the registry's hardware devicemap.
        //

		//知识：如果希望执行统计什么的，驱动程序必须实现QueryRoutine
		//知识：如果只是查找注册表的value值，那么不需要实现希望QueryRoutine，QueryRoutine=NULL
        registryTable[0].QueryRoutine = KbdDeviceMapQueryCallback; //回调函数，每个值都call一遍，好统计
		//If Name is NULL, the QueryRoutine function specified for this table entry 
		//is called for all values associated with the current registry key
        registryTable[0].Name = NULL;

		//RTL_REGISTRY_DEVICEMAP的意思：Path is relative to \Registry\Machine\Hardware\DeviceMap.
		//RTL_REGISTRY_OPTIONAL的意思：Specifies that the key referenced by this parameter and the Path parameter are optional.

		//查询的过程中还能完成统计之类的回调任务，这里我们希望得到NumberPortsServiced的统计
        status = RtlQueryRegistryValues(
                     RTL_REGISTRY_DEVICEMAP | RTL_REGISTRY_OPTIONAL,
                     BasePortName->Buffer, //Path
                     registryTable, //输入，也是输出表
                     NumberPortsServiced, //可选输入Context，这个要传给QueryRoutine来实现驱动程序员希望的统计
                     NULL
                     );

        if (!NT_SUCCESS(status)) {
            KbdPrint((
                1,
                "KBDCLASS-KbdDeterminePortsServiced: RtlQueryRegistryValues failed with 0x%x\n",
                status
                ));
        }

        ExFreePool(registryTable);
    }

    return(status);
}

//回调函数
VOID
KeyboardClassFindMorePorts (
    PDRIVER_OBJECT  DriverObject,
    PVOID           Context,
    ULONG           Count
    )
/*++
Routine Description:

    This routine is called from
    serviced by the boot device drivers and then called again by the
    IO system to find disk devices serviced by nonboot device drivers.

Arguments:

    DriverObject
    Context -
    Count - Used to determine if this is the first or second time called.

Return Value:

    None

--*/

{
    NTSTATUS                status;
    PDEVICE_EXTENSION       tmp_deviceExtension = NULL;
    PDEVICE_OBJECT          tmp_classDeviceObject = NULL;
    ULONG                   dumpData[DUMP_COUNT];
    ULONG                   i;
    ULONG                   tmp_numPorts; //现在未知
    ULONG                   successfulCreates;
    UNICODE_STRING          tmp_basePortName; //静态的
    UNICODE_STRING          tmp_fullPortName; //分配的
    WCHAR                   tmp_basePortBuffer[NAME_MAX];//服务于tmp_basePortName
    PWCHAR                  tmp_fullClassName = NULL;
    PFILE_OBJECT            tmp_fileobject;

    PAGED_CODE ();

    //初始化tmp_fullPortName
	tmp_fullPortName.MaximumLength = 0;

	//初始化tmp_basePortName
    RtlZeroMemory(tmp_basePortBuffer, NAME_MAX * sizeof(WCHAR)); //清零
    tmp_basePortName.Buffer = tmp_basePortBuffer;
    tmp_basePortName.Length = 0; 
    tmp_basePortName.MaximumLength = NAME_MAX * sizeof(WCHAR);

    //
    // Set up the base device name for the associated port device.
    // It is the same as the base class name, with "Class" replaced
    // by "Port".
    // 构造端口名L"KeyboardPort"
    RtlCopyUnicodeString(&tmp_basePortName, &Globals.BaseClassName); //L"KeyboardClass",来自注册表
    tmp_basePortName.Length -= (sizeof(L"Class") - sizeof(UNICODE_NULL));//L"Keyboard"
    RtlAppendUnicodeToString(&tmp_basePortName, L"Port");//L"KeyboardPort"

    //
    // Set up space for the full device object name for the ports.
    //

    tmp_fullPortName.MaximumLength = sizeof(L"\\Device\\")
                               + tmp_basePortName.Length
                               + sizeof (UNICODE_NULL);

    tmp_fullPortName.Buffer = ExAllocatePool(PagedPool,
                                         tmp_fullPortName.MaximumLength);

    if (!tmp_fullPortName.Buffer) {

        KbdPrint((
            1,
            "KBDCLASS-KeyboardClassInitialize: Couldn't allocate string for port device object name\n"
            ));

        dumpData[0] = (ULONG) tmp_fullPortName.MaximumLength;
        KeyboardClassLogError (DriverObject,
                               KBDCLASS_INSUFFICIENT_RESOURCES,
                               KEYBOARD_ERROR_VALUE_BASE + 8,
                               STATUS_UNSUCCESSFUL,
                               1,
                               dumpData,
                               0);

        goto KeyboardFindMorePortsExit;

    }

	//构造键名：L"\Device\KeyboardClass"
    RtlZeroMemory(tmp_fullPortName.Buffer, tmp_fullPortName.MaximumLength);
    RtlAppendUnicodeToString(&tmp_fullPortName, L"\\Device\\");
    RtlAppendUnicodeToString(&tmp_fullPortName, tmp_basePortName.Buffer);
    RtlAppendUnicodeToString(&tmp_fullPortName, L"0");

    KbdDeterminePortsServiced(&tmp_basePortName, //上面刚刚构造的：L"\Device\KeyboardPort"
                              &tmp_numPorts); //想知道服务几个端口，现在知道了

    //
    // Set up the class device object(s) to handle the associated
    // port devices.
    //

	//=====================================================================
	//设置类设备对象来应对关联的端口设备，什么意思？
	//=====================================================================
	//已知：服务几个端口、Globals.NumberLegacyPorts、Globals.PortsServiced、
	//fullPortName、
	
	//注意不是从0开始
    for (i = Globals.NumberLegacyPorts, successfulCreates = 0;
         ((i < Globals.PortsServiced) && (i < tmp_numPorts)); 
		 //    ---------------------
		 //     MaximumPortsServiced键值，一般是3
         i++) {

        //
        // Append the suffix to the device object name string.  E.g., turn
        // \Device\PointerClass into \Device\PointerClass0.  Then attempt
        // to create the device object.  If the device object already
        // exists increment the suffix and try again.
        //

        tmp_fullPortName.Buffer[(tmp_fullPortName.Length / sizeof(WCHAR)) - 1]
            = L'0' + (WCHAR) i; //L"\Device\KeyboardPort0"
			                    //L"\Device\KeyboardPort1"...

        //
        // Create the class device object.
        // 创建类设备对象
        status = KbdCreateClassObject (DriverObject,//本函数的输入参数
                                       &Globals.InitExtension,//输入
                                       &tmp_classDeviceObject,//输出，PDEVICE_OBJECT
                                       &tmp_fullClassName,//输出，FullDeviceName
                                       TRUE);

        if (!NT_SUCCESS(status)) {
            KeyboardClassLogError (DriverObject,
                                   KBDCLASS_INSUFFICIENT_RESOURCES,
                                   KEYBOARD_ERROR_VALUE_BASE + 8,
                                   status,
                                   0,
                                   NULL,
                                   0);
            continue;
        }

		//为什么要该pnp标志为假？
        tmp_deviceExtension = (PDEVICE_EXTENSION)tmp_classDeviceObject->DeviceExtension;
        tmp_deviceExtension->PnP = FALSE;

        //
        // Connect to the port device.
        //
		// 下面函数returns (1) a pointer to the top object in the named device object's stack
		//                 (2) a pointer to the corresponding tmp_fileobject object
        status = IoGetDeviceObjectPointer (&tmp_fullPortName, //名字，来自Globals和上面的加工
                                           FILE_READ_ATTRIBUTES, //DesiredAccess 
                                           &tmp_fileobject, //输出，FileObject 
                                           &tmp_deviceExtension->TopPort);//Pointer to the device object that represents 
										                              //the named logical, virtual, or physical device

        if (status != STATUS_SUCCESS) {
            // ISSUE: log error
            KeyboardClassDeleteLegacyDevice (tmp_deviceExtension);
            continue;
        }

		//盖房子一样把设备对象放到设备栈上面
		//所以越上面的StackSize就越大
        tmp_classDeviceObject->StackSize = 1 + tmp_deviceExtension->TopPort->StackSize;
        status = KeyboardAddDeviceEx (tmp_deviceExtension,//上面修改后的：pnp和
									tmp_fullClassName, //KbdCreateClassObject得到的
									tmp_fileobject);//IoGetDeviceObjectPointer得到的
        tmp_classDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

        if (tmp_fullClassName) {
            ExFreePool (tmp_fullClassName);
            tmp_fullClassName = NULL;
        }

        if (!NT_SUCCESS (status)) {
            if (Globals.GrandMaster == NULL) { //失败处理1：传统端口设备
                if (tmp_deviceExtension->File) {
                    tmp_fileobject = tmp_deviceExtension->File;//马上要减去一个引用
                    tmp_deviceExtension->File = NULL;
                }
            }
            else {
                PPORT port; //失败处理2：非传统端口设备，有大师

                ExAcquireFastMutex (&Globals.Mutex);

                tmp_fileobject = Globals.AssocClassList[tmp_deviceExtension->UnitId].File;//马上要减去一个引用
                Globals.AssocClassList[tmp_deviceExtension->UnitId].File = NULL;
                Globals.AssocClassList[tmp_deviceExtension->UnitId].Free = TRUE;
                Globals.AssocClassList[tmp_deviceExtension->UnitId].Port = NULL;

                ExReleaseFastMutex (&Globals.Mutex);
            }

            if (tmp_fileobject) {//可能已经为NULL
                ObDereferenceObject (tmp_fileobject);//减去一个引用
            }

            KeyboardClassDeleteLegacyDevice (tmp_deviceExtension);
            continue;
        }

        //
        // We want to only add it to our list if everything went alright
        // 什么是传统的设备？来自注册表的设备就是
        InsertTailList (&Globals.LegacyDeviceList, &tmp_deviceExtension->Link);
        successfulCreates++;
    } // for
    Globals.NumberLegacyPorts = i;

KeyboardFindMorePortsExit:

    //
    // Free the unicode strings.
    //

    if (tmp_fullPortName.MaximumLength != 0) {
        ExFreePool(tmp_fullPortName.Buffer);
    }

    if (tmp_fullClassName) {
        ExFreePool (tmp_fullClassName);
    }
}



//注册表查询回调函数，按windows要求，驱动必须实现该函数
NTSTATUS
KbdDeviceMapQueryCallback(
    IN PWSTR ValueName,
    IN ULONG ValueType,
    IN PVOID ValueData,
    IN ULONG ValueLength,
    IN PVOID Context,
    IN PVOID EntryContext
    )

/*++

Routine Description:

    This is the callout routine specified in a call to
    RtlQueryRegistryValues.  It increments the value pointed
    to by the Context parameter.

Arguments:

    ValueName - Unused.

    ValueType - Unused.

    ValueData - Unused.

    ValueLength - Unused.

    Context - Pointer to a count of the number of times this
        routine has been called.  This is the number of ports
        the class driver needs to service.

    EntryContext - Unused.

Return Value:

    The function value is the final status from the operation.

--*/

{
    PAGED_CODE ();

    *(PULONG)Context += 1;

    return(STATUS_SUCCESS);
}
