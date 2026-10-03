// Real compiler/stdlib byte RawArray production through Array's frozen init.
// The boundary cases are observed before heap allocation by the GDB fixture;
// they do not claim complete multi-GiB initialization.
main(args: Array<String>): Int64 {
    let length: Int64 = match (args[0]) {
        case "below" => 4294967272
        case "equal" => 4294967280
        case "above" => 4294967296
        case _ => 17
    }
    let bytes = Array<UInt8>(length, repeat: 0)
    let valid = bytes.size == length && bytes[0] == 0 && bytes[length - 1] == 0
    println("RAW_ARRAY_INITIALIZATION_TARGET length=${length} valid=${valid}")
    return if (valid) { 0 } else { 1 }
}
