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
public class Probe {
    public static func big2(x: Big, y: Big): String {
        gc(heavy: true)
        "${x.head}|${x.body[4095]}|${y.tail}|${y.body[1]}"
    }
}
public class Outer {
    public let s: String
    public init(x: Big, y: Big, mode: Int64) {
        eprintln("INNER_BEGIN mode=${mode}")
        this.s = if (mode == 0) { Probe.big2(x, y) } else {
            let ti = ClassTypeInfo.of<Probe>()
            let bt = StructTypeInfo.of<Big>()
            let f = ti.getStaticFunction("big2", [bt, bt])
            (f.apply(ti, [x, y]) as String).getOrThrow()
        }
        eprintln("INNER_END ${s}")
    }
}
main(args: Array<String>): Int64 {
    let group = if (args.size > 0) { args[0] } else { "nested" }
    let bt = StructTypeInfo.of<Big>()
    let ot = ClassTypeInfo.of<Outer>()
    let ctor = ot.getConstructor([bt, bt, TypeInfo.of<Int64>()])
    let x = Big("xh", 17, "xt")
    let y = Big("yh", 19, "yt")
    let mode: Int64 = if (group == "reflect-direct" || group == "direct-direct") { 0 } else { 1 }
    eprintln("OUTER_BEGIN ${group}")
    let got = spawn {
        let obj = if (group == "direct-nested" || group == "direct-direct") { Outer(x, y, mode) } else {
            (ctor.apply([x, y, mode]) as Outer).getOrThrow()
        }
        gc(heavy: true)
        obj.s
    }.get()
    eprintln("OUTER_END ${group}: ${got}")
    if (got == "xh|17|yt|19") { println("NESTED_OK"); return 0 }
    return 1
}
