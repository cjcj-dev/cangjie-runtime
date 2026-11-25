/* Installed std.reflect public consumer. Metadata stays loaded for this run. */
package reflectcache

import std.reflect.*

foreign {
    func ReflectFixtureInit(): Int32
    func ReflectFixtureSet(index: Int32, value: UInt32): Unit
    func ReflectFixtureSameName(): Unit
    func ReflectFixtureRestore(): Unit
}

public class First {
    public init() {}
}

public class Second {
    public init() {}
}

public class Box<T> {
    public init() {}
}

public struct Parcel {
    public let value: Int64
    public init(value: Int64) { this.value = value }
}

func require(ok: Bool, name: String): Unit {
    if (!ok) { throw Exception("REFLECT_CACHE_ASSERT ${name}") }
}

main(args: Array<String>): Int64 {
    if (args.size > 0 && args[0] == "control") {
        require(TypeInfo.of<Int64>().name == "Int64", "primitive-control")
        println("REFLECT_CACHE_CONTROL_PASS")
        return 0
    }
    let a = spawn { => TypeInfo.of<First>() }
    let b = spawn { => TypeInfo.of<Second>() }
    let first = a.get()
    let second = b.get()
    require(first != second && !refEq(first, second), "different-types")
    require(refEq(first, TypeInfo.of<First>()), "warm-pointer-identity")
    require(refEq(second, TypeInfo.of<Second>()), "second-warm-pointer-identity")
    require(refEq(first, TypeInfo.get(first.qualifiedName)), "name-current-identity")
    require(refEq(second, TypeInfo.get(second.qualifiedName)), "second-name-current-identity")
    let generic = TypeInfo.of<Box<Int64>>()
    require(refEq(generic, TypeInfo.get(generic.qualifiedName)), "generic-name-current-identity")
    let parcel = (TypeInfo.of<Parcel>() as StructTypeInfo).getOrThrow()
    var missingConstructor = false
    try {
        parcel.construct(Array<Any>())
    } catch (_: InfoNotFoundException) {
        missingConstructor = true
    }
    require(missingConstructor, "construct-missing-exception-name")
    println("REFLECT_CACHE_PUBLIC_PASS")
    // Only this process's two live fixture types change, after the threads join.
    // This models serial generations; it is not a loader/unload safety test.
    require(unsafe { ReflectFixtureInit() } == 0, "serial-fixture-prepared")
    try {
        unsafe { ReflectFixtureSet(0, 0); ReflectFixtureSet(1, 0) }
        let zeroA = TypeInfo.of<First>()
        let zeroB = TypeInfo.of<Second>()
        require(!refEq(zeroA, zeroB), "zero-distinct-types")
        require(!refEq(zeroA, TypeInfo.of<First>()), "zero-not-memoized")
        unsafe { ReflectFixtureSet(0, 0x70000001); ReflectFixtureSet(1, 0x70000002) }
        let generationA = TypeInfo.of<First>()
        require(!refEq(first, generationA), "address-new-uuid")
        require(refEq(generationA, TypeInfo.of<First>()), "new-generation-warm")
        unsafe { ReflectFixtureSet(0, 0x70000003) }
        let byName = TypeInfo.get(first.qualifiedName)
        require(!refEq(generationA, byName), "name-resolves-current-generation")
        require(refEq(byName, TypeInfo.of<First>()), "name-and-pointer-agree")
        // Reusing a UUID on another live address must evict every memo alias.
        unsafe { ReflectFixtureSet(1, 0x70000003) }
        let aliasB = TypeInfo.of<Second>()
        let afterAlias = TypeInfo.of<First>()
        require(!refEq(byName, afterAlias), "uuid-alias-clears-pointer-memo")
        require(!refEq(aliasB, afterAlias), "uuid-alias-not-wrapper-identity")
        unsafe { ReflectFixtureSet(1, 0x70000004); ReflectFixtureSameName() }
        let sameNameOtherAddress = TypeInfo.of<Second>()
        require(sameNameOtherAddress.qualifiedName == first.qualifiedName, "same-name-fixture-applied")
        require(!refEq(afterAlias, sameNameOtherAddress), "same-name-new-address")
        require(!refEq(afterAlias, TypeInfo.of<First>()), "name-alias-clears-old-pointer")
    } finally {
        unsafe { ReflectFixtureRestore() }
    }
    println("REFLECT_CACHE_SERIAL_FIXTURE_PASS")
    return 0
}
