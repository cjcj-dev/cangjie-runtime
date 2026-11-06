package readscopeprobe

public class ReadScopeValue {
    public let value: Int64 = 37
}

public func readScopeValue(): Int64 {
    return ReadScopeValue().value
}
