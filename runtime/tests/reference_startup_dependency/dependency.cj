package reference_startup_dependency

import std.collection.*
import std.ref.*
import std.sync.*

public let startupFinalized = AtomicInt64(0)

public class StartupFinalizable {
    public let value: Int64
    public init(value: Int64) { this.value = value }
    ~init() { startupFinalized.store(value) }
}

func initializeDependency(): WeakRef<StartupFinalizable> {
    let values = ArrayList<Int64>()
    values.add(1356)
    WeakRef<StartupFinalizable>(StartupFinalizable(values[0]), EAGER)
}

public let startupWeak = initializeDependency()
