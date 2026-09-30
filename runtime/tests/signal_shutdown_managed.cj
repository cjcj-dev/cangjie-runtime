package shutdownprobe

foreign func ShutdownInstallCallback(callback: CFunc<(Int32, CPointer<Unit>, CPointer<Unit>) -> Bool>): Unit
foreign func ShutdownNativeBlock(): Unit
foreign func ShutdownManagedProgress(stage: Int32): Unit

@C
func shutdownManagedCallback(_: Int32, _: CPointer<Unit>, _: CPointer<Unit>): Bool {
    unsafe { ShutdownManagedProgress(1) }
    unsafe { ShutdownNativeBlock() }
    unsafe { ShutdownManagedProgress(2) }
    return true
}

func installCallback(): Int64 {
    unsafe { ShutdownInstallCallback(shutdownManagedCallback) }
    return 1
}

public let shutdownInstalled: Int64 = installCallback()
