import std.reflect.*
import std.runtime.*
import std.sync.*
public struct Big {
    public let head: String
    public let body: VArray<Int64, $4096>
    public let tail: String
    public init(head: String, fill: Int64, tail: String) {
        this.head = head
        this.body = VArray<Int64, $4096>(repeat: fill)
        this.tail = tail
    }
}

foreign func StableStackCallback(callback: CFunc<() -> Int64>): Int64

public class Boundary {
    public static func value(x: Big, y: Big): String {
        gc(heavy: true)
        "${x.head}|${x.body[4095]}|${y.tail}|${y.body[1]}"
    }
    public static func raiseAfterNested(x: Big, y: Big): String {
        let got = invokeValue(x, y)
        if (got != "xh|17|yt|19") { throw IllegalStateException("wrong nested result") }
        throw IllegalStateException("expected nested exception")
    }
}
func invokeValue(x: Big, y: Big): String {
    let ti = ClassTypeInfo.of<Boundary>()
    let bt = StructTypeInfo.of<Big>()
    let f = ti.getStaticFunction("value", [bt, bt])
    (f.apply(ti, [x, y]) as String).getOrThrow()
}
func callbackBody(): Int64 {
    let x = Big("xh", 17, "xt")
    let y = Big("yh", 19, "yt")
    if (invokeValue(x,y) == "xh|17|yt|19") { 73 } else { -1 }
}
main(args: Array<String>): Int64 {
    let mode = args[0]
    let result = spawn {
        let x = Big("xh", 17, "xt")
        let y = Big("yh", 19, "yt")
        if (mode == "exception") {
            let ti = ClassTypeInfo.of<Boundary>()
            let bt = StructTypeInfo.of<Big>()
            let f = ti.getStaticFunction("raiseAfterNested", [bt, bt])
            var caught = false
            try { f.apply(ti, [x,y]) } catch (e: IllegalStateException) {
                caught = e.message == "expected nested exception"
            }
            let after = invokeValue(x,y)
            if (caught && after == "xh|17|yt|19") { 0 } else { 1 }
        } else if (mode == "dynamic") {
            let fn = { a: Any, b: Any => invokeValue((a as Big).getOrThrow(), (b as Big).getOrThrow()) }
            let ft = (TypeInfo.of(fn) as FunctionTypeInfo).getOrThrow()
            let got = (ft.apply(fn, [x,y]) as String).getOrThrow()
            if (got == "xh|17|yt|19") { 0 } else { 2 }
        } else {
            let callback: CFunc<() -> Int64> = { => callbackBody() }
            let got = unsafe { StableStackCallback(callback) }
            if (got == 73) { 0 } else { 3 }
        }
    }.get()
    println("STABLE_BOUNDARY ${mode} result=${result}")
    result
}
