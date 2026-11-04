/* Installed std.reflect public consumer. Metadata stays loaded for this run. */
package reflectcache

import std.reflect.*

public class First {
    public init() {}
}

public class Second {
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
    let parcel = (TypeInfo.of<Parcel>() as StructTypeInfo).getOrThrow()
    var missingConstructor = false
    try {
        parcel.construct(Array<Any>())
    } catch (_: InfoNotFoundException) {
        missingConstructor = true
    }
    require(missingConstructor, "construct-missing-exception-name")
    println("REFLECT_CACHE_PUBLIC_PASS")
    return 0
}
