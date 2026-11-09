import generic_read_payload.Box as PayloadBox
import generic_read_payload.Payload

main(args: Array<String>): Int64 {
    if (args.size != 1) { return 2 }
    if (args[0] == "primitive") {
        let box = PayloadBox<Int64>(739391)
        let got = box.read()
        println("TARGET_GENERIC_PRIMITIVE_EXECUTED got=${got}")
        if (got != 739391 || box.anchor != "container anchor") { return 1 }
        return 0
    }
    if (args[0] == "references") {
        let box = PayloadBox<Payload>(Payload("kept reference", 99173))
        let got = box.read()
        println("CONTROL_GENERIC_REFERENCES_EXECUTED number=${got.number} text=${got.text}")
        if (got.number != 99173 || got.text != "kept reference" || box.anchor != "container anchor") { return 1 }
        return 0
    }
    return 2
}
