// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// Licensed under Apache-2.0 with Runtime Library Exception.
// Run only with a qualified CR01 compiler/LLVM/runtime and freshly built std.
import std.reflect.*
import std.runtime.*

public open class ReflectedBase {
    public let baseValue: Int64
    public init(value: Int64) { baseValue = value }
}

public class ReflectedDerived <: ReflectedBase {
    public let derivedValue: Int64
    public init(value: Int64) {
        super(value)
        derivedValue = value + 1
        gc(heavy: true)
    }
    public init() { this(731) }
    ~init() {}
}

public class ReflectedGeneric<T> {
    public let value: T
    public init(value: T) {
        this.value = value
        gc(heavy: true)
    }
}

public struct ReflectedValue {
    public let value: Int64
    public init(value: Int64) { this.value = value }
}

public struct ReflectedGenericValue<T> {
    public let value: T
    public init(value: T) { this.value = value }
}

public class ReflectedThrowing {
    public init() {
        gc(heavy: true)
        throw IllegalArgumentException("reflection_ctor_normal_edge_absent")
    }
}

class ReflectionChecks {
    private var failures = 0
    func check(name: String, ok: Bool): Unit {
        println("REFLECTION_MANAGED_TARGET name=${name} pass=${ok}")
        if (!ok) { failures++ }
    }
    func result(): Int64 { failures }
}

main(): Int64 {
    let checks = ReflectionChecks()
    let derivedType = (TypeInfo.of<ReflectedDerived>() as ClassTypeInfo).getOrThrow()
    let derived = (derivedType.construct([761]) as ReflectedDerived).getOrThrow()
    checks.check("super_receiver_fields", derived.baseValue == 761 && derived.derivedValue == 762)
    let delegated = (derivedType.construct([]) as ReflectedDerived).getOrThrow()
    checks.check("this_receiver_fields", delegated.baseValue == 731 && delegated.derivedValue == 732)
    let genericType = (TypeInfo.of<ReflectedGeneric<ReflectedDerived>>() as ClassTypeInfo).getOrThrow()
    let generic = (genericType.construct([derived]) as ReflectedGeneric<ReflectedDerived>).getOrThrow()
    checks.check("generic_receiver_root", generic.value.baseValue == 761 && generic.value.derivedValue == 762)
    let valueType = (TypeInfo.of<ReflectedValue>() as StructTypeInfo).getOrThrow()
    let value = (valueType.construct([781]) as ReflectedValue).getOrThrow()
    checks.check("struct_abi", value.value == 781)
    let genericValueType = (TypeInfo.of<ReflectedGenericValue<Int64>>() as StructTypeInfo).getOrThrow()
    let genericValue = (genericValueType.construct([791]) as ReflectedGenericValue<Int64>).getOrThrow()
    checks.check("generic_struct_abi", genericValue.value == 791)
    var normalEdge = false
    var expectedException = false
    try {
        let throwingType = (TypeInfo.of<ReflectedThrowing>() as ClassTypeInfo).getOrThrow()
        let _ = throwingType.construct([])
        normalEdge = true
    } catch (e: Exception) {
        expectedException = e.message.contains("reflection_ctor_normal_edge_absent")
    }
    checks.check("exception_has_no_receiver_result", expectedException && !normalEdge)
    // A subsequent successful reflection call must not see a stale pending
    // exception or leaked native receiver root from the throwing constructor.
    let after = (derivedType.construct([811]) as ReflectedDerived).getOrThrow()
    checks.check("after_exception", after.baseValue == 811 && after.derivedValue == 812)
    println("REFLECTION_MANAGED_RESULT failures=${checks.result()}")
    checks.result()
}
